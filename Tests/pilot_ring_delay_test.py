"""Recorded delayed ring update and closed-loop flight through a stale target."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller
import pilot_flight_test as flight

FIXTURE = json.loads(Path(__file__).with_name('pilot_ring_delay_fixture.json').read_text(encoding='utf-8'))


class RingPassTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def replay_capture(self):
        window = FIXTURE['capture_window']
        self.now = window[0]['tick_ms'] - 50
        self.start(window[0])
        results = []
        for sample in window:
            out = self.update(sample, ms=sample['tick_ms'] - self.now)
            results.append((sample, dict(self.c.detail.items()), dict(out.items())))
        return results

    def test_recorded_centre_crossing_does_not_command_a_turn_back_or_nose_dive(self):
        results = self.replay_capture()
        passed = [r for r in results if r[0]['elapsed_ms'] >= 461589]
        self.assertTrue(self.c.enabled)
        self.assertGreater(len(passed), 4)
        for sample, detail, out in passed:
            self.assertTrue(detail['ring_flythrough'])
            self.assertLess(detail['ring_along_m'], 0)
            self.assertLess(abs(detail['goal_roll_deg']), 10)
            self.assertGreater(detail['goal_pitch_deg'], 3)
            self.assertGreater(out['aileron'], -.25)
        pitches = [d['goal_pitch_deg'] for _, d, _ in results if d['ring_flythrough']]
        self.assertAlmostEqual(min(pitches), max(pitches))

    def test_same_element_moving_to_next_ring_releases_pass_immediately(self):
        self.replay_capture()
        sample = FIXTURE['late_target_entry']
        out = self.update(sample, ms=sample['tick_ms'] - self.now)
        self.assertTrue(self.c.enabled)
        self.assertTrue(self.c.detail['marker_changed'])
        self.assertFalse(self.c.detail['ring_flythrough'])
        self.assertGreater(self.c.detail['goal_roll_deg'], 40)
        self.assertGreater(out['aileron'], 0)
        self.assertGreater(out['rudder'], 0)

    def test_stale_target_timeout_releases_controls_without_orbiting(self):
        self.replay_capture()
        sample = copy.deepcopy(FIXTURE['capture_window'][-1])
        for _ in range(34):
            out = self.update(sample)
        self.assertFalse(self.c.enabled)
        self.assertIn('Нет следующего кольца', self.c.status)
        self.assertTrue(all(out[k] == 0 for k in ('aileron', 'elevator', 'rudder', 'throttle', 'brake')))

    def test_capture_requires_vertical_and_lateral_interception_not_just_proximity(self):
        for dz, track_error, bank in ((45, 0, 0), (0, 40, 0), (0, 0, 35)):
            with self.subTest(dz=dz, track_error=track_error, bank=bank):
                self.setUp()
                d = controller.observation(on_ground=False, agl_terrain_m=150, speed_kmh=250,
                    horizontal_speed_kmh=250, position_m=[0, 0, 150], roll_deg=bank)
                d['navigation'].update(marker_type='ring', position=[0, 40, 150 + dz],
                    distance_2d_m=40, distance_3d_m=math.hypot(40, dz), altitude_error_m=dz,
                    heading_error_deg=track_error, track_error_deg=track_error)
                self.start(d)
                self.update(d)
                self.assertFalse(self.c.detail['ring_flythrough'])

    def test_disturbance_before_crossing_releases_capture_for_correction(self):
        d = copy.deepcopy(next(s for s in FIXTURE['capture_window'] if s['elapsed_ms'] == 460899))
        self.start(d)
        self.update(d)
        self.assertTrue(self.c.detail['ring_flythrough'])
        d['climb_mps'] = -40
        self.update(d)
        self.assertFalse(self.c.detail['ring_flythrough'])
        self.assertTrue(self.c.enabled)

    def test_crossing_outside_vertical_corridor_is_not_treated_as_a_pass(self):
        d = controller.observation(on_ground=False, agl_terrain_m=150, speed_kmh=250,
            horizontal_speed_kmh=250, position_m=[0, 0, 150])
        d['navigation'].update(marker_type='ring', position=[0, 40, 150], distance_2d_m=40, distance_3d_m=40)
        self.start(d)
        self.update(d)
        self.assertTrue(self.c.detail['ring_flythrough'])
        d.update(position_m=[0, 41, 175], agl_terrain_m=175)
        d['navigation'].update(distance_2d_m=1, distance_3d_m=math.hypot(1, 25),
            altitude_error_m=-25, heading_error_deg=180, track_error_deg=180, bearing_deg=180)
        self.update(d, ms=200)
        self.assertFalse(self.c.detail['ring_flythrough'])
        self.assertIsNone(self.c.ringPass)


class DelayedRingDynamicsTests(unittest.TestCase):
    setUp = flight.FlightDynamicsTests.setUp

    def simulate(self, delay, side, gain, lag, dt=.05):
        d = copy.deepcopy(FIXTURE['previous_entry'])
        d['vehicle'] = 'plane'
        d['position_m'][0] *= side
        for key in ('heading_deg', 'roll_deg', 'heading_rate_dps', 'roll_rate_dps'):
            d[key] *= side
        targets = [FIXTURE['previous_ring'][:], FIXTURE['next_ring'][:]]
        for target in targets:
            target[0] *= side
        now, target_index, entered = 1000, 0, None
        self.assertIs(self.c.start(self.c, self.lua.table_from(d, recursive=True), now), True)
        bank, pitch = math.radians(d['roll_deg']), math.radians(d['pitch_deg'])
        p = d['roll_rate_dps']
        q = d['pitch_rate_dps'] * math.cos(bank) + d['heading_rate_dps'] * math.cos(pitch) * math.sin(bank)
        r = d['heading_rate_dps'] * math.cos(pitch) * math.cos(bank) - d['pitch_rate_dps'] * math.sin(bank)
        speed = d['speed_kmh'] / 3.6
        terrain = d['position_m'][2] - d['agl_terrain_m']
        minimum, peak_roll, delayed_pass = [1e9, 1e9], 0, False
        for _ in range(round(20 / dt)):
            target = targets[target_index]
            delta = [target[i] - d['position_m'][i] for i in range(3)]
            horizontal, distance = math.hypot(*delta[:2]), math.sqrt(sum(v*v for v in delta))
            minimum[target_index] = min(minimum[target_index], distance)
            if distance < 28:
                if target_index == 1:
                    return minimum, peak_roll, delayed_pass
                if entered is None:
                    entered = now
            if target_index == 0 and entered is not None and now - entered >= delay * 1000:
                target_index = 1
                continue
            bearing = math.degrees(math.atan2(delta[0], delta[1])) % 360
            error = (bearing - d['heading_deg'] + 180) % 360 - 180
            d['track_deg'] = d['heading_deg']
            d['navigation'].update(position=target, distance_2d_m=horizontal, distance_3d_m=distance,
                altitude_error_m=delta[2], bearing_deg=bearing, heading_error_deg=error, track_error_deg=error)
            now += round(dt * 1000)
            out = self.c.update(self.c, self.lua.table_from(d, recursive=True), now, True)
            if not self.c.enabled:
                break
            delayed_pass |= bool(self.c.detail['ring_exit_wait_ms'])
            self.apply(now)
            a = self.lua.globals().analog
            roll_input, pitch_input = a['vehicle_right'] - a['vehicle_left'], a['steer_back'] - a['steer_forward']
            p += (47 * gain * roll_input - p) * dt / lag
            q += ((30.5 if pitch_input >= 0 else 21) * gain * pitch_input - q) * dt / lag
            r += (12.4 * gain * out['rudder'] - r) * dt / lag
            bank, pitch = math.radians(d['roll_deg']), math.radians(d['pitch_deg'])
            d['roll_rate_dps'] = p
            d['pitch_rate_dps'] = q * math.cos(bank) - r * math.sin(bank)
            d['heading_rate_dps'] = (q * math.sin(bank) + r * math.cos(bank)) / max(.5, math.cos(pitch)) + .12 * d['roll_deg']
            d['roll_deg'] += d['roll_rate_dps'] * dt
            d['pitch_deg'] += d['pitch_rate_dps'] * dt
            d['heading_deg'] = (d['heading_deg'] + d['heading_rate_dps'] * dt) % 360
            gamma, heading = math.radians(d['pitch_deg'] + 1.4), math.radians(d['heading_deg'])
            d['climb_mps'] = speed * math.sin(gamma)
            d['horizontal_speed_kmh'] = d['speed_kmh'] * math.cos(gamma)
            d['position_m'] = [d['position_m'][0] + speed * math.cos(gamma) * math.sin(heading) * dt,
                d['position_m'][1] + speed * math.cos(gamma) * math.cos(heading) * dt,
                d['position_m'][2] + d['climb_mps'] * dt]
            d['agl_terrain_m'] = d['position_m'][2] - terrain
            peak_roll = max(peak_roll, abs(d['roll_deg']))
        self.fail((minimum, peak_roll, self.c.status))

    def test_two_recorded_rings_with_delayed_update_and_coupled_aircraft_axes(self):
        for delay in (0, .8, 1.2):
            for side in (-1, 1):
                for gain, lag in ((.7, .3), (1, .2), (1.3, .15), (1, .6)):
                    with self.subTest(delay=delay, side=side, gain=gain, lag=lag):
                        self.setUp()
                        minimum, peak_roll, _ = self.simulate(delay, side, gain, lag)
                        self.assertLess(max(minimum), 28)
                        self.assertLess(peak_roll, 65)


if __name__ == '__main__':
    unittest.main()
