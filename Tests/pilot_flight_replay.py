"""Open-loop replay: exercises decisions on real observations, not a flight simulator."""
import argparse
import json
from pathlib import Path
from collections import Counter

from analyze_pilot_flight import read_flight
from pilot_controller_test import LuaRuntime, controller_source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    lua = LuaRuntime(unpack_returned_tuples=True)
    cls = lua.execute(controller_source())
    controller = cls.new()
    phases, transitions, requests = Counter(), [], Counter()
    started, last_phase, active_samples = False, None, 0
    for record in read_flight(args.log):
        data, tick = record.get('data', {}), record.get('tick_ms', 0)
        if record['type'] == 'job_notification' and data.get('channel') == 'province:sendNotification' and data.get('origin') == 'province_pilot':
            controller.notify(controller, data.get('text_plain', ''), tick)
        if record['type'] != 'sample':
            continue
        observation = lua.table_from(data, recursive=True)
        if not started and data.get('model') == 519 and data.get('driver'):
            assert controller.start(controller, observation, tick) is True
            started = True
        if not started:
            continue
        out = controller.update(controller, observation, tick, True)
        phase = controller.phase
        phases[phase] += 1
        for name in ('throttle', 'brake', 'rudder', 'elevator', 'aileron'):
            assert -1 <= out[name] <= 1, (record['elapsed_ms'], name, out[name])
            requests[name] += out[name] != 0
        if phase != last_phase:
            transitions.append(dict(time_s=record['elapsed_ms']/1000, phase=phase, status=controller.status))
            last_phase = phase
        if not controller.enabled:
            break
        active_samples += 1
    print(json.dumps(dict(active_samples=active_samples, phase_counts=phases, transitions=transitions,
        nonzero_requests=requests, meaning='Open-loop recorded observations; does not validate closed-loop flight'),
        indent=2, ensure_ascii=True))


if __name__ == '__main__':
    main()
