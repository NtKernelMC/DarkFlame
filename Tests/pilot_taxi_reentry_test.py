"""Overlap regression and a lagged taxi model; no live server events are synthesized."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller_tests
from pilot_controller_test import observation

FIXTURE = json.loads(Path(__file__).with_name('pilot_taxi_reentry_fixture.json').read_text(encoding='utf-8'))


def geometry(data):
    n = data['navigation']
    dx, dy, dz = [n['position'][i] - data['position_m'][i] for i in range(3)]
    distance = math.hypot(dx, dy)
    bearing = math.degrees(math.atan2(dx, dy)) % 360
    n.update(distance_2d_m=distance, distance_3d_m=math.hypot(distance, dz), altitude_error_m=dz,
        heading_error_deg=(bearing - data['heading_deg'] + 180) % 360 - 180, bearing_deg=bearing,
        marker_inside=math.hypot(distance, dz) <= n['marker_size_m'])


class ReentryTests(unittest.TestCase):
    setUp, start, update = controller_tests.MathTests.setUp, controller_tests.MathTests.start, controller_tests.MathTests.update

    def recorded_start(self):
        data = copy.deepcopy(FIXTURE['samples'][1]['data'])
        self.c.notify(self.c, 'Двигайтесь медленно по меткам для выруливания на взлетную полосу', self.now)
        self.start(data)
        return data

    def stationary(self, data):
        data.update(speed_kmh=0, velocity_body_rfu_mps=[0, 0, 0], heading_rate_dps=0)

    def test_recorded_overlap_waits_then_exits_straight_backwards(self):
        data = self.recorded_start()
        out = self.update(data)
        self.assertEqual(self.c.phase, 'taxi_reentry_wait')
        self.assertEqual(out['rudder'], 0)
        self.assertGreater(out['throttle'], 0)  # braking the recorded backward velocity
        self.stationary(data)
        for _ in range(21): out = self.update(data)
        self.assertEqual(self.c.phase, 'taxi_reentry_exit')
        self.assertEqual(out['rudder'], 0)
        self.assertGreater(out['brake'], 0)
        self.assertEqual(self.c.detail['motion_direction'], -1)
        self.assertLess(self.c.detail['taxi_reentry_exit_distance_m'], 19)
        self.assertEqual(self.c.detail['taxi_reentry_reason'], 'target_appeared_inside')

    def test_delayed_ack_cancels_without_exit_even_with_same_element_id(self):
        data = self.recorded_start()
        self.stationary(data)
        for _ in range(15): self.update(data)
        data['navigation']['position'][0] += 150
        geometry(data)
        out = self.update(data)
        self.assertIsNone(self.c.taxiReentry)
        for _ in range(5): out = self.update(data)
        self.assertEqual(self.c.phase, 'taxi')
        self.assertGreater(out['throttle'], 0)

    def test_exact_center_does_not_require_an_undefined_bearing(self):
        data = self.recorded_start()
        self.stationary(data)
        data['position_m'][:2] = data['navigation']['position'][:2]
        geometry(data)
        data['navigation'].update(heading_error_deg=None, bearing_deg=None)
        for _ in range(22): self.update(data)
        self.assertTrue(self.c.enabled, self.c.status)
        self.assertEqual(self.c.phase, 'taxi_reentry_exit')
        self.assertAlmostEqual(self.c.detail['taxi_reentry_exit_distance_m'], 32)

    def test_obstacle_or_unknown_probe_prevents_recovery_movement(self):
        for clear in (False, None):
            self.setUp()
            data = self.recorded_start()
            self.stationary(data)
            for _ in range(25):
                out = self.update(data, clear=clear)
                self.assertEqual(out['throttle'], 0)
                self.assertEqual(out['brake'], 0)
                self.assertEqual(out['rudder'], 0)
                self.assertTrue(out['handbrake'])
            self.assertIn(self.c.phase, ('obstacle_hold', 'probe_wait'))

    def test_stalled_reentry_and_cross_track_loss_end_without_orbit(self):
        for cross in (0, 4):
            self.setUp()
            data = self.recorded_start()
            self.stationary(data)
            for _ in range(23): self.update(data)
            h = math.radians(data['heading_deg'])
            data['position_m'][0] += math.cos(h) * cross
            data['position_m'][1] -= math.sin(h) * cross
            geometry(data)
            for _ in range(165):
                self.update(data)
                if not self.c.enabled: break
            self.assertFalse(self.c.enabled)
            self.assertIn('ручной перехват', self.c.status)
            self.assertIn('линии' if cross else 'прогресса', self.c.status)

    def test_yellow_parking_and_air_ring_do_not_start_reentry(self):
        for kind, color, air in [('checkpoint', [255,255,0,255], False), ('ring', [255,0,0,128], True)]:
            self.setUp()
            data = self.recorded_start()
            data.update(on_ground=not air, agl_terrain_m=100 if air else 1)
            data['navigation'].update(marker_type=kind, color_rgba=color)
            self.c.start(self.c, self.lua.table_from(data, recursive=True), self.now)
            self.update(data)
            self.assertIsNone(self.c.taxiReentry)

    def test_normal_entry_waits_for_new_target_before_it_can_chase_center(self):
        data = observation(speed_kmh=0, velocity_body_rfu_mps=[0,0,0])
        data['navigation']['marker_inside'] = False
        self.start(data)
        self.update(data)
        data['navigation'].update(marker_inside=True, distance_2d_m=29, distance_3d_m=29)
        self.update(data)
        self.assertEqual(self.c.phase, 'taxi_reentry_wait')
        self.assertEqual(self.c.detail['taxi_reentry_reason'], 'entry_not_acknowledged')

    def test_observed_membership_must_confirm_exit(self):
        data = self.recorded_start()
        self.stationary(data)
        for _ in range(23): self.update(data)
        rec = self.c.taxiReentry
        distance = rec['exitDistance'] + .5
        data['position_m'][0] += rec['fx'] * rec['direction'] * distance
        data['position_m'][1] += rec['fy'] * rec['direction'] * distance
        geometry(data)
        data['navigation']['marker_inside'] = None
        out = self.update(data)
        self.assertEqual(self.c.taxiReentry['stage'], 'exit')
        self.assertEqual(out['throttle'], 0)
        self.assertEqual(out['brake'], 0)

    def simulate(self, position=None, heading=None, delay=.4, response=.25, mirror=False, move_target=True):
        data = self.recorded_start()
        if position is not None:
            data['position_m'][:2] = position
            self.stationary(data)
        if heading is not None: data['heading_deg'] = heading
        if mirror:
            data['position_m'][0] *= -1
            data['navigation']['position'][0] *= -1
            data['heading_deg'] = -data['heading_deg'] % 360
        geometry(data)
        self.c.start(self.c, self.lua.table_from(data, recursive=True), self.now)
        velocity = data['velocity_body_rfu_mps'][1]
        yaw = acceleration = 0
        old_inside = True
        entry_tick = None
        directions = []
        phases = set()
        max_distance = 0
        settled = 0
        for _ in range(950):
            out = self.update(data)
            phases.add(self.c.phase)
            if not self.c.enabled:
                self.assertFalse(move_target, self.c.status)
                self.assertIn('не подтверждён', self.c.status)
                return
            direction = self.c.detail['motion_direction']
            if self.c.phase == 'direction_change':
                self.assertEqual(out['rudder'], 0)
                settled += abs(velocity) < .18
            if self.c.phase.startswith('taxi_reentry_') and self.c.detail['goal_speed_kmh'] > 0:
                if not directions or directions[-1] != direction: directions.append(direction)
            goal = 16*out['throttle'] - 6*out['brake'] - .3*velocity
            acceleration += (goal-acceleration) * .05 / response
            velocity += acceleration*.05
            if out['handbrake']:
                velocity *= .3
                acceleration *= .3
            yaw_goal = math.copysign(1, velocity) * 8.8*out['rudder']*min(1, abs(velocity))
            yaw += (yaw_goal-yaw)*.05/.25
            data['heading_deg'] = (data['heading_deg']+yaw*.05) % 360
            h = math.radians(data['heading_deg'])
            data['position_m'][0] += math.sin(h)*velocity*.05
            data['position_m'][1] += math.cos(h)*velocity*.05
            data.update(speed_kmh=abs(velocity)*3.6, velocity_body_rfu_mps=[0,velocity,0], heading_rate_dps=yaw)
            geometry(data)
            inside = data['navigation']['marker_inside']
            max_distance = max(max_distance, data['navigation']['distance_2d_m'])
            if not old_inside and inside and entry_tick is None: entry_tick = self.now
            old_inside = inside
            if entry_tick is not None and self.now-entry_tick >= delay*1000 and move_target:
                data['navigation']['position'][0] += 150
                geometry(data)
                self.update(data)
                self.assertIsNone(self.c.taxiReentry)
                self.assertEqual(len(directions), 2, directions)
                self.assertEqual(directions[0], -directions[1])
                self.assertGreaterEqual(settled, 3)
                self.assertLess(max_distance, 35)
                self.assertIn('taxi_reentry_return', phases)
                return (self.now-1000)/1000
        self.fail(('No exit/re-entry/ack', self.c.phase, data['position_m'], self.c.status))

    def test_recorded_overlap_exits_and_reenters_with_response_lag(self):
        for mirror in (False, True):
            for delay in (.1, .8):
                for response in (.15, .35):
                    with self.subTest(mirror=mirror, delay=delay, response=response):
                        self.setUp()
                        self.assertLess(self.simulate(mirror=mirror, delay=delay, response=response), 25)

    def test_center_and_front_back_side_positions_reenter(self):
        n = FIXTURE['samples'][1]['data']['navigation']['position']
        for dx, dy, heading in [(0,0,0), (0,-24,0), (0,24,0), (24,0,0), (-24,0,0), (0,0,359)]:
            with self.subTest(dx=dx, dy=dy, heading=heading):
                self.setUp()
                self.simulate(position=[n[0]+dx,n[1]+dy], heading=heading)

    def test_second_entry_without_ack_never_loops_again(self):
        self.simulate(move_target=False)


class ReentryAdapterTests(unittest.TestCase):
    setUp, arm = controller_tests.AdapterTests.setUp, controller_tests.AdapterTests.arm
    run_lua, records = controller_tests.AdapterTests.run_lua, controller_tests.AdapterTests.records

    def test_probe_and_controls_use_the_same_exit_and_return_direction(self):
        self.run_lua('function getMarkerSize() return 30 end; m1.position={0,23,6.2}')
        self.arm()
        self.run_lua('step(90)')
        d = self.records('sample')[-1]['data']['autopilot']
        self.assertEqual(d['phase'], 'taxi_reentry_exit')
        self.assertEqual(d['decision']['motion_direction'], -1)
        self.assertEqual(d['obstacle_probe']['requested_direction'], -1)
        self.assertGreater(self.lua.eval('analog.brake_reverse'), 0)
        self.assertEqual(self.lua.eval('analog.accelerate'), 0)
        self.run_lua('plane.position={0,-9,6.2}; localPlayer.position=plane.position; step(30)')
        d = self.records('sample')[-1]['data']['autopilot']
        self.assertEqual(d['phase'], 'taxi_reentry_return')
        self.assertEqual(d['decision']['motion_direction'], 1)
        self.assertEqual(d['obstacle_probe']['requested_direction'], 1)
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)
        self.run_lua('plane.position={0,-6.8,6.2}; localPlayer.position=plane.position; step(8)')
        self.assertEqual(self.records('sample')[-1]['data']['autopilot']['phase'], 'taxi_reentry_ack')
        self.assertEqual(self.lua.eval('analog.accelerate'), 0)
        self.run_lua('m1.position={0,150,6.2}; step(18)')
        self.assertEqual(self.records('sample')[-1]['data']['autopilot']['phase'], 'taxi')
        self.assertEqual(self.records('collector_error'), [])


if __name__ == '__main__':
    unittest.main()
