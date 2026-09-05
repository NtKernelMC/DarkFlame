"""Recorded false holds and bounded ground-path queries; no live game replay."""
import copy
import json
import unittest
from pathlib import Path

import pilot_controller_test as controller
import pilot_taxi_test as taxi

CASES = json.loads(Path(__file__).with_name('pilot_obstacle_fixture.json').read_text(encoding='utf-8'))['cases']


class ProbeTests(unittest.TestCase):
    setUp, check = taxi.GeometryTests.setUp, taxi.GeometryTests.check

    def data(self):
        return controller.observation(speed_kmh=10, velocity_body_rfu_mps=[0, 10/3.6, 0],
            position_m=[0, 0, 28.8], basis_world_rfu=[[1, 0, 0], [0, 1, 0], [0, 0, 1]])

    def test_recorded_takeoff_no_longer_projects_pitched_nose_into_runway(self):
        case = next(c for c in CASES if c['name'] == 'takeoff_ground')
        data = case['sample']
        ground = data['surface']['ground_z_m']
        def runway(*args):
            return min(args[2], args[5]) <= ground <= max(args[2], args[5])
        self.assertTrue(any(runway(*(r['from'] + r['to'])) for r in case['old_probe']['rays']))
        self.lua.globals().wall = runway
        ap = self.lua.globals().autopilot
        ap.notify(ap, 'Взлет разрешен!', 1000)
        self.assertIs(self.check(data), True)
        probe = self.lua.globals().state['probe']
        self.assertEqual(probe['status'], 'clear')
        self.assertTrue(probe['complete'])
        for _, ray in probe['rays'].items():
            self.assertGreater(min(ray['from'][3], ray['to'][3]), ground)
            if ray['kind'] != 'reaction':
                self.assertAlmostEqual(ray['from'][3], ray['to'][3], places=7)

    def test_recorded_taxi_budget_exhaustion_resumes_unchecked_rays(self):
        for case in CASES:
            if not case['name'].startswith('taxi_budget'):
                continue
            with self.subTest(case=case['name']):
                self.setUp()
                self.assertFalse(case['old_probe']['complete'])
                self.assertTrue(all(r['clear'] for r in case['old_probe']['rays']))
                self.lua.execute('function isLineOfSightClear(...) now=now+1; return true end')
                self.assertIsNone(self.check(case['sample']))
                probe = self.lua.globals().state['probe']
                first = len(probe['rays'])
                self.assertEqual(probe['status'], 'pending')
                self.assertLessEqual(probe['cost_ms'], 6)
                for i in range(1, 6):
                    result = self.check(case['sample'], 1000 + i*40)
                    probe = self.lua.globals().state['probe']
                    self.assertEqual(probe['probe_id'], 1000)
                    self.assertGreater(len(probe['rays']), first)
                    first = len(probe['rays'])
                    if result is True:
                        break
                self.assertIs(result, True)
                self.assertEqual(probe['status'], 'clear')
                self.assertTrue(probe['complete'])

    def test_fresh_clear_cache_covers_slow_query_but_expires(self):
        data = self.data()
        self.assertIs(self.check(data), True)
        self.lua.execute('function isLineOfSightClear(...) now=now+1; return true end')
        self.assertIs(self.check(data, 1200), True)
        self.assertEqual(self.lua.globals().state['probe']['status'], 'pending')
        self.assertEqual(self.lua.globals().state['probe']['cached_clear_tick_ms'], 1000)
        self.assertIsNone(self.check(data, 1251))
        self.lua.execute('function isLineOfSightClear(...) return true end')
        self.assertIs(self.check(data, 1267), True)

    def test_changed_world_pose_or_direction_cannot_borrow_clear_cache(self):
        variants = [dict(heading_deg=10), dict(pitch_deg=4), dict(roll_deg=4),
            dict(position_m=[0, 0, 30]), dict(position_m=[10, 0, 28.8]),
            dict(speed_kmh=40), dict(dimension=1), dict(interior=1)]
        for change in variants:
            with self.subTest(change=change):
                self.setUp()
                data = self.data()
                self.assertIs(self.check(data), True)
                self.lua.execute('function isLineOfSightClear(...) now=now+1; return true end')
                data.update(change)
                self.assertIsNone(self.check(data, 1200))
        self.setUp()
        data = self.data()
        self.assertIs(self.check(data), True)
        self.lua.globals().autopilot['instruction'] = 'reverse'
        self.lua.execute('function isLineOfSightClear(...) now=now+1; return true end')
        self.assertIsNone(self.check(data, 1200))

    def test_confirmed_hit_overrides_clear_cache_and_ends_query_early(self):
        data = self.data()
        self.assertIs(self.check(data), True)
        self.lua.globals().wall = lambda *args: True
        self.assertIs(self.check(data, 1200), False)
        probe = self.lua.globals().state['probe']
        self.assertEqual(probe['status'], 'blocked')
        self.assertEqual(probe['blocked_ray'], 1)
        self.assertEqual(len(probe['rays']), 1)
        self.assertIsNone(self.lua.globals().state['lastClearProbe'])

    def test_api_failure_is_unknown_and_never_a_confirmed_obstacle(self):
        data = self.data()
        self.assertIs(self.check(data), True)
        self.lua.execute('function isLineOfSightClear(...) return nil end')
        self.assertIsNone(self.check(data, 1200))
        self.assertEqual(self.lua.globals().state['probe']['status'], 'unavailable')

    def test_turn_avoids_distant_straight_line_post_but_checks_reaction_path(self):
        data = self.data()
        data['navigation']['heading_error_deg'] = 90
        def post_at(y):
            def hit(*args):
                a, b = args[:3], args[3:6]
                if abs(b[1]-a[1]) < 1e-8:
                    return False
                t = (y-a[1])/(b[1]-a[1])
                x, z = a[0]+t*(b[0]-a[0]), a[2]+t*(b[2]-a[2])
                return 0 <= t <= 1 and abs(x) < .15 and 31 <= z <= 32
            return hit
        far = post_at(19)
        self.assertTrue(far(0, 14, 31.36, 0, 20, 31.36))
        self.lua.globals().wall = far
        self.assertIs(self.check(data), True)
        self.lua.globals().wall = post_at(15)
        self.assertIs(self.check(data, 1200), False)
        probe = self.lua.globals().state['probe']
        self.assertEqual(probe['rays'][probe['blocked_ray']]['kind'], 'reaction')

    def test_wing_samples_do_not_invent_low_wing_but_keep_low_body(self):
        self.assertIs(self.check(self.data()), True)
        rays = self.lua.globals().state['probe']['rays']
        wings = [r for _, r in rays.items() if 'wing' in r['part']]
        self.assertTrue(wings)
        self.assertTrue(all(r['from'][3] >= 28.8 + 3.94 for r in wings))
        self.assertTrue(any(r['part'] == 'nose' and r['from'][3] < 30 for _, r in rays.items()))


class ControllerTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def test_pending_scan_does_not_latch_taxi_hold(self):
        data = controller.observation(speed_kmh=3, velocity_body_rfu_mps=[0, 3/3.6, 0])
        self.start(data)
        out = self.update(data, clear=None)
        self.assertEqual(self.c.phase, 'probe_wait')
        self.assertGreater(out['brake'], 0)
        self.assertFalse(self.c.blocked)
        out = self.update(data)
        self.assertEqual(self.c.phase, 'taxi')
        self.assertGreater(out['throttle'], 0)

    def test_takeoff_waits_for_probe_then_resumes_and_still_brakes_for_real_hit(self):
        data = controller.observation(speed_kmh=78)
        data['navigation'].update(marker_type='ring', heading_error_deg=12)
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.start(data)
        self.assertGreater(self.update(data, clear=None)['brake'], 0)
        self.assertEqual(self.c.phase, 'probe_wait')
        self.assertEqual(self.update(data)['throttle'], 1)
        self.assertGreater(self.update(data, clear=False)['brake'], 0)
        self.assertEqual(self.c.phase, 'obstacle_hold')

    def test_takeoff_geometry_uses_restricted_runway_rudder(self):
        data = controller.observation()
        data['navigation'].update(marker_type='ring', heading_error_deg=20)
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.start(data)
        out = self.update(data)
        intent = self.c.groundIntent(self.c, self.lua.table_from(data, recursive=True), self.now)
        self.assertTrue(intent['runway'])
        self.assertAlmostEqual(intent['yaw'], out['rudder'] * 8.8)
        self.assertLessEqual(abs(intent['rudder']), .8)
        self.assertEqual(self.c.phase, 'takeoff_align')


if __name__ == '__main__':
    unittest.main()
