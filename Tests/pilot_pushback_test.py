"""Liberty departure: progressing pushback must survive the old 12-second cutoff."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller

FIXTURE = json.loads(Path(__file__).with_name('pilot_pushback_fixture.json').read_text(encoding='utf-8'))
ENTRY = next(row['data'] for row in FIXTURE['samples'] if row['decision'].get('pushback_align'))
REVERSE = 'Двигайтесь аккуратно назад для начала маршрута'
TAXI = 'Двигайтесь медленно по меткам для выруливания на взлетную полосу'


class PushbackTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def begin_align(self, side=1):
        d = copy.deepcopy(ENTRY)
        d['position_m'][0] *= side
        d['heading_deg'] = (d['heading_deg'] * side) % 360
        d['heading_rate_dps'] *= side
        n = d['navigation']
        n['position'][0] *= side
        n['heading_error_deg'] *= side
        n['bearing_deg'] = (n['bearing_deg'] * side) % 360
        self.c.notify(self.c, REVERSE, self.now)
        self.start(d)
        self.c.notify(self.c, TAXI, self.now-300)
        self.update(d)
        self.assertEqual(self.c.phase, 'pushback_align')
        return d

    def test_recorded_progress_survives_exact_time_of_previous_disengagement(self):
        rows = FIXTURE['samples']
        self.now = rows[0]['elapsed_ms']-50
        self.c.notify(self.c, REVERSE, self.now)
        self.start(rows[0]['data'])
        notices = iter(FIXTURE['notifications'])
        notice = next(notices, None)
        for row in rows:
            while notice and notice['elapsed_ms'] <= row['elapsed_ms']:
                self.c.notify(self.c, notice['text'], notice['elapsed_ms'])
                notice = next(notices, None)
            self.update(row['data'], row['elapsed_ms']-self.now)
            self.assertTrue(self.c.enabled, self.c.status)
        out = self.update(rows[-1]['data'], FIXTURE['stop']['elapsed_ms']-self.now)
        self.assertTrue(self.c.enabled, self.c.status)
        self.assertGreater(self.c.detail['pushback_elapsed_ms'], 12000)
        self.assertGreater(self.c.detail['pushback_progress_deg'], 70)
        self.assertLess(self.c.detail['pushback_remaining_deg'], 8)
        self.assertLess(self.c.detail['pushback_no_progress_ms'], 500)
        self.assertLess(self.c.detail['pushback_travel_m'], 14)
        self.assertLess(out['rudder'], -.9)
        self.assertGreater(out['brake'], 0)

    def test_recorded_initial_pose_finishes_turn_and_stops_before_forward_in_lagged_model(self):
        for side in (-1,1):
            for gain, lag in ((5.5,.3),(6.2,.25),(8.8,.4)):
                with self.subTest(side=side, gain=gain, lag=lag):
                    self.setUp()
                    d = self.begin_align(side)
                    forward, yaw = d['velocity_body_rfu_mps'][1], d['heading_rate_dps']
                    align_travel, saw_stopped, align_ms = 0, False, 0
                    for _ in range(600):
                        out = self.update(d)
                        self.assertTrue(self.c.enabled, self.c.status)
                        if self.c.phase == 'pushback_align':
                            align_ms += 50
                            align_travel += abs(forward)*.05
                        saw_stopped |= self.c.phase == 'direction_change' and abs(forward)<.18
                        if self.c.phase == 'taxi' and forward>.4:
                            break
                        acceleration = 6.5*out['throttle']-4*out['brake']-.8*forward
                        forward += acceleration*.05
                        if out['handbrake']:
                            forward *= .3
                        yaw_goal = math.copysign(1,forward)*gain*out['rudder']*min(1,abs(forward))
                        yaw += (yaw_goal-yaw)*.05/lag
                        d['heading_deg'] = (d['heading_deg']+yaw*.05)%360
                        h = math.radians(d['heading_deg'])
                        d['position_m'][0] += math.sin(h)*forward*.05
                        d['position_m'][1] += math.cos(h)*forward*.05
                        dx,dy = (d['navigation']['position'][i]-d['position_m'][i] for i in (0,1))
                        bearing = math.degrees(math.atan2(dx,dy))%360
                        distance = math.hypot(dx,dy)
                        d['navigation'].update(bearing_deg=bearing, heading_error_deg=(bearing-d['heading_deg']+180)%360-180,
                            distance_2d_m=distance, distance_3d_m=math.hypot(distance,d['navigation']['altitude_error_m']))
                        d.update(speed_kmh=abs(forward)*3.6, velocity_body_rfu_mps=[0,forward,0], heading_rate_dps=yaw)
                    self.assertEqual(self.c.phase, 'taxi')
                    self.assertGreater(forward, .4)
                    self.assertTrue(saw_stopped)
                    self.assertLess(align_travel, 18)
                    self.assertLess(abs(d['navigation']['heading_error_deg']), 55)
                    if gain <= 6.2:
                        self.assertGreater(align_ms, 12000)

    def test_stuck_heading_stops_with_useful_diagnostics(self):
        d = self.begin_align()
        for _ in range(125):
            self.update(d)
        self.assertFalse(self.c.enabled)
        self.assertEqual(self.c.detail['pushback_stop_reason'], 'no_progress')
        self.assertGreaterEqual(self.c.detail['pushback_no_progress_ms'], 6000)
        self.assertEqual(self.c.detail['pushback_progress_deg'], 0)

    def test_small_heading_oscillation_cannot_reset_progress_watchdog(self):
        d = self.begin_align()
        error = d['navigation']['heading_error_deg']
        for i in range(125):
            d['navigation']['heading_error_deg'] = error + (1 if i%2 else -1)
            self.update(d)
        self.assertFalse(self.c.enabled)
        self.assertEqual(self.c.detail['pushback_stop_reason'], 'no_progress')

    def test_obstacle_wait_does_not_consume_progress_time_and_movement_stays_blocked(self):
        for clear in (False, None):
            with self.subTest(clear=clear):
                self.setUp()
                d = self.begin_align()
                for _ in range(300):
                    out = self.update(d, clear=clear)
                    self.assertTrue(self.c.enabled, self.c.status)
                    self.assertEqual(out['brake'], 0)
                    self.assertEqual(out['rudder'], 0)
                self.assertLess(self.c.detail['pushback_no_progress_ms'], 100)
                self.assertGreater(self.c.detail['pushback_elapsed_ms'], 12000)
                for _ in range(40):
                    self.update(d)
                self.assertTrue(self.c.enabled)
                for _ in range(120):
                    self.update(d)
                self.assertFalse(self.c.enabled)
                self.assertEqual(self.c.detail['pushback_stop_reason'], 'no_progress')

    def test_distance_limit_remains_even_when_heading_improves(self):
        d = self.begin_align()
        d['position_m'][0] += 18.1
        d['navigation']['heading_error_deg'] -= 4
        self.update(d)
        self.assertFalse(self.c.enabled)
        self.assertEqual(self.c.detail['pushback_stop_reason'], 'distance_limit')
        self.assertGreater(self.c.detail['pushback_progress_deg'], 0)

    def test_cumulative_travel_cannot_be_hidden_by_circling_near_start(self):
        d = self.begin_align()
        x,y,z = d['position_m']
        for i in range(1,44):
            d['position_m'] = [x+5*math.sin(i*.1), y+5*(1-math.cos(i*.1)), z]
            d['navigation']['heading_error_deg'] -= .5
            self.update(d, 100)
            if not self.c.enabled:
                break
        self.assertFalse(self.c.enabled)
        self.assertEqual(self.c.detail['pushback_stop_reason'], 'distance_limit')
        self.assertLess(self.c.detail['pushback_distance_m'], 11)
        self.assertGreater(self.c.detail['pushback_travel_m'], 18)


if __name__ == '__main__':
    unittest.main()
