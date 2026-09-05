"""Mirny gear timing: separate approach preparation from deployment."""
import copy
import json
import unittest
from pathlib import Path

import pilot_controller_test as controller

FIXTURE = json.loads(Path(__file__).with_name('pilot_gear_timing_fixture.json').read_text(encoding='utf-8'))


class GearTimingTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def approach(self, agl=131, climb=-7.8):
        data = copy.deepcopy(FIXTURE['samples']['early_approach']['data'])
        data.update(agl_terrain_m=agl, climb_mps=climb, landing_gear_down=False)
        self.start(data)
        self.c.landing = True
        return data

    def test_recorded_heuristic_starts_approach_without_releasing_gear_at_131m(self):
        data = copy.deepcopy(FIXTURE['samples']['before_approach']['data'])
        self.start(data)
        self.c.descendingWaypoints = 4
        self.update(data)
        data = copy.deepcopy(FIXTURE['samples']['early_approach']['data'])
        out = self.update(data, ms=64)
        self.assertEqual(self.c.phase, 'approach')
        self.assertFalse(out['gear_down'])
        self.assertEqual(self.c.detail['gear_command_reason'], 'approach_wait')
        self.assertEqual(self.c.detail['goal_speed_kmh'], 245)

    def test_recorded_60m_window_leaves_about_21_seconds_before_observed_touchdown(self):
        data = copy.deepcopy(FIXTURE['samples']['below_60']['data'])
        data['landing_gear_down'] = False  # counterfactual actuator decision; not a physical replay
        self.start(data)
        self.c.landing = True
        self.assertTrue(self.update(data)['gear_down'])
        self.assertEqual(self.c.detail['gear_command_reason'], 'approach_altitude')
        margin = (FIXTURE['samples']['touchdown']['tick_ms'] - FIXTURE['samples']['below_60']['tick_ms']) / 1000
        self.assertGreater(margin, 20)
        self.assertLess(margin, 22)

    def test_fast_descent_uses_time_margin_before_the_height_threshold(self):
        data = self.approach(agl=95, climb=-12)
        self.assertTrue(self.update(data)['gear_down'])
        self.assertEqual(self.c.detail['gear_command_reason'], 'descent_time')
        self.assertLess(self.c.detail['gear_contact_linear_s'], 10)

    def test_notice_before_start_is_preserved_and_releases_at_any_height(self):
        self.c.notify(self.c, 'Выпустите шасси.', self.now)
        data = self.approach(agl=200)
        self.assertTrue(self.update(data)['gear_down'])
        self.assertEqual(self.c.detail['gear_command_reason'], 'job_notice')

    def test_deployed_gear_does_not_retract_if_terrain_height_or_climb_changes(self):
        data = self.approach(agl=59)
        self.assertTrue(self.update(data)['gear_down'])
        data.update(agl_terrain_m=90, climb_mps=2, landing_gear_down=True)
        self.assertTrue(self.update(data)['gear_down'])
        self.assertEqual(self.c.detail['gear_command_reason'], 'approach_altitude')

    def test_unknown_height_and_final_landing_never_delay_deployment(self):
        for agl in (None, 4):
            self.setUp()
            data = self.approach()
            data['agl_terrain_m'] = agl
            self.assertTrue(self.update(data)['gear_down'])
            self.assertEqual(self.c.detail['gear_command_reason'], 'agl_unavailable' if agl is None else 'final_landing')

    def test_manual_gear_on_approach_and_restarting_ap_preserve_down(self):
        data = self.approach()
        data['landing_gear_down'] = True
        self.assertTrue(self.update(data)['gear_down'])
        self.assertEqual(self.c.detail['gear_command_reason'], 'already_down_on_approach')
        self.c.stop(self.c, 'test')
        self.start(data)
        self.assertTrue(self.update(data)['gear_down'])

    def test_next_takeoff_clears_old_landing_gear_order(self):
        data = self.approach()
        self.c.notify(self.c, 'Выпустите шасси.', self.now)
        self.update(data)
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.assertFalse(self.c.gearJobRequest)
        self.assertIsNone(self.c.gearDeployReason)
        for _ in range(25): out = self.update(data)
        self.assertFalse(out['gear_down'])

    def test_low_climb_after_takeoff_is_not_treated_as_approach(self):
        data = self.approach(agl=30, climb=5)
        self.c.landing = False
        for _ in range(25): out = self.update(data)
        self.assertFalse(out['gear_down'])
        self.assertIsNone(self.c.gearDeployReason)


class GearReasonAdapterTests(unittest.TestCase):
    setUp, arm = controller.AdapterTests.setUp, controller.AdapterTests.arm
    run_lua, records = controller.AdapterTests.run_lua, controller.AdapterTests.records

    def test_actual_command_event_includes_job_notification_reason(self):
        self.run_lua('''
            ground=false; gear=false; plane.position={0,0,131}; plane.velocity={0,1.3,-.08}
            m1.position={0,800,100}; m1.markerType='ring'
        ''')
        self.arm()
        self.run_lua("event('province:sendNotification',pilotRoot,'Выпустите шасси.'); step(12)")
        requests = self.records('autopilot_gear_request')
        self.assertEqual(len(requests), 1)
        self.assertTrue(requests[0]['data']['down'])
        self.assertEqual(requests[0]['data']['reason'], 'job_notice')
        self.assertTrue(self.lua.globals().gear)
        self.assertEqual(self.records('collector_error'), [])


if __name__ == '__main__':
    unittest.main()
