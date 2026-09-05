"""Summarize a recorded manual flight without altering the original JSONL file."""
import argparse
import csv
import hashlib
import json
import statistics
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def quantile(values, p):
    values = sorted(v for v in values if isinstance(v, (int, float)))
    if not values:
        return None
    i = (len(values) - 1) * p
    lo = int(i)
    return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (i - lo)


def read_flight(path):
    with path.open(encoding='utf-8') as stream:
        return [json.loads(line) for line in stream if line.strip()]


def summarize(records):
    samples = [(r['elapsed_ms'] / 1000, r['data']) for r in records if r['type'] == 'sample']
    notices, seen, episodes = [], set(), []
    for r in records:
        if r['type'] == 'job_notification':
            d = r['data']
            if d.get('channel') == 'province:sendNotification' and d.get('text_plain') not in seen:
                notices.append((r['elapsed_ms'] / 1000, d.get('origin'), d.get('text_plain')))
                seen.add(d.get('text_plain'))
    for t, d in samples:
        nav = d.get('navigation') or {}
        if not nav.get('position'):
            continue
        signature = (nav['id'], tuple(round(v, 2) for v in nav['position']), nav.get('marker_type'))
        if not episodes or signature != episodes[-1]['signature']:
            episodes.append(dict(signature=signature, start_s=t, end_s=t, id=nav['id'],
                marker_type=nav.get('marker_type'), color=nav.get('color_rgba'), position=nav['position'],
                minimum_distance_m=1e9, samples=0, final_distance_m=None, max_abs_roll_deg=0,
                initial_speed_kmh=d.get('speed_kmh'), initial_altitude_error_m=nav.get('altitude_error_m'),
                initial_agl_m=d.get('agl_terrain_m')))
        e = episodes[-1]
        e['end_s'], e['final_distance_m'] = t, nav['distance_3d_m']
        e['minimum_distance_m'] = min(e['minimum_distance_m'], nav['distance_3d_m'])
        e['max_abs_roll_deg'] = max(e['max_abs_roll_deg'], abs(d.get('roll_deg', 0)))
        e['samples'] += 1
    taxi = [d for t, d in samples if (43.241 <= t < 89.086 or 365.359 <= t < 399.261)
        and d.get('on_ground') is True and (d.get('navigation') or {}).get('heading_error_deg') is not None]
    bins = []
    for low, high in [(0, 5), (5, 15), (15, 30), (30, 60), (60, 181)]:
        selected = [d for d in taxi if low <= abs(d['navigation']['heading_error_deg']) < high]
        bins.append(dict(angle=f'{low}–{min(180, high)}', count=len(selected),
            speed_median=quantile([d['speed_kmh'] for d in selected], .5),
            speed_p90=quantile([d['speed_kmh'] for d in selected], .9)))
    responses = []
    for name, opposite, rate, ground in [('vehicle_look_left', 'vehicle_look_right', 'heading_rate_dps', True),
        ('vehicle_look_right', 'vehicle_look_left', 'heading_rate_dps', True),
        ('vehicle_left', 'vehicle_right', 'roll_rate_dps', False),
        ('vehicle_right', 'vehicle_left', 'roll_rate_dps', False),
        ('steer_back', 'steer_forward', 'pitch_rate_dps', False),
        ('steer_forward', 'steer_back', 'pitch_rate_dps', False)]:
        selected = [d.get(rate) for t, d in samples if t < 474.5 and d.get('on_ground') is ground
            and d.get('control_held_ms', {}).get(name, 0) >= 150
            and d.get('controls', {}).get('digital', {}).get(name)
            and not d.get('controls', {}).get('digital', {}).get(opposite)]
        responses.append(dict(control=name, ground=ground, rate=rate, count=len(selected), median=quantile(selected, .5)))
    return dict(counts=Counter(r['type'] for r in records), duration_s=max(t for t, d in samples),
        samples=len(samples), notices=notices, episodes=episodes, taxi_bins=bins, responses=responses)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / '.codex-temp-dia2dump/pilot-flight-analysis')
    args = parser.parse_args()
    data = summarize(read_flight(args.log))
    data['sha256'] = hashlib.sha256(args.log.read_bytes()).hexdigest()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'summary.json').write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
    episodes = [{k: v for k, v in e.items() if k != 'signature'} for e in data['episodes']]
    with (args.output / 'waypoints.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(episodes[0]))
        writer.writeheader()
        writer.writerows(episodes)
    print(json.dumps({k: v for k, v in data.items() if k != 'episodes'}, ensure_ascii=True, indent=2))
    print('Air waypoint episodes (time, AGL, target altitude error, closest distance):')
    for e in episodes:
        if e['marker_type'] == 'ring':
            print(e['start_s'], e['initial_agl_m'], e['initial_altitude_error_m'], e['minimum_distance_m'])


if __name__ == '__main__':
    main()
