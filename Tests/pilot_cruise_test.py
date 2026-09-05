"""Cruise speed regression; the recorder's velocity scale stays unchanged."""
import unittest

import pilot_controller_test as controller_tests
from pilot_controller_test import observation


def cruise(speed=257):
    data = observation(on_ground=False, agl_terrain_m=400, position_m=[0,0,400], pitch_deg=-1.4,
        speed_kmh=speed, horizontal_speed_kmh=speed, velocity_body_rfu_mps=[0,speed/3.6,0])
    data['navigation'].update(position=[0,1200,400], distance_2d_m=1200, distance_3d_m=1200,
        marker_type='ring', marker_inside=False)
    return data


class CruiseTests(unittest.TestCase):
    setUp, start, update = controller_tests.MathTests.setUp, controller_tests.MathTests.start, controller_tests.MathTests.update

    def test_straight_cruise_accelerates_past_recorded_old_speed_cap(self):
        data = cruise()
        self.start(data)
        out = self.update(data)
        self.assertEqual(self.c.detail['goal_speed_kmh'], 270)
        self.assertEqual(self.c.detail['cruise_speed_blend'], 1)
        self.assertEqual(out['throttle'], 1)
        self.assertEqual(out['brake'], 0)
        data.update(speed_kmh=279, horizontal_speed_kmh=279)
        self.assertGreater(self.update(data)['brake'], 0)

    def test_boost_fades_with_actual_bank_before_turn_limit_takes_over(self):
        data = cruise()
        self.start(data)
        targets = []
        for bank in (0,5,10,15,20):
            data['roll_deg'] = bank
            self.update(data)
            targets.append(self.c.detail['goal_speed_kmh'])
        self.assertEqual(targets, [270,270,262.5,255,255])

    def test_turning_towards_ring_keeps_existing_speed_reduction(self):
        data = cruise()
        data['navigation'].update(heading_error_deg=60, track_error_deg=60, bearing_deg=60,
            distance_2d_m=200, distance_3d_m=200)
        self.start(data)
        self.update(data)
        self.assertEqual(self.c.detail['cruise_speed_blend'], 0)
        self.assertLess(self.c.detail['goal_speed_kmh'], 255)

    def test_landing_keeps_old_target_without_cruise_boost(self):
        data = cruise(200)
        data['agl_terrain_m'] = 50
        self.c.notify(self.c, 'Выпустите шасси', self.now)
        self.start(data)
        self.update(data)
        self.assertEqual(self.c.detail['cruise_speed_blend'], 0)
        self.assertEqual(self.c.detail['goal_speed_kmh'], 177.5)


if __name__ == '__main__':
    unittest.main()
