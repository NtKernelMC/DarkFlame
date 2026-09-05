"""Boarding stops at the live marker boundary rather than chasing its centre."""
import math
import unittest

import pilot_controller_test as controller_tests
from pilot_controller_test import observation


def boarding(distance=50, speed=0, size=30, inside=False):
    data = observation(speed_kmh=speed, velocity_body_rfu_mps=[0, speed / 3.6, 0],
        position_m=[0, -distance, 1.2])
    data['navigation'].update(position=[0, 0, 1.2], distance_2d_m=distance, distance_3d_m=distance,
        marker_size_m=size, marker_inside=inside, color_rgba=[255, 255, 0, 255])
    return data


class BoardingTests(unittest.TestCase):
    setUp, start, update = controller_tests.MathTests.setUp, controller_tests.MathTests.start, controller_tests.MathTests.update

    def test_approach_limits_speed_before_entering_large_marker(self):
        data = boarding(speed=26)
        self.start(data)
        out = self.update(data)
        self.assertEqual(self.c.phase, 'boarding_approach')
        self.assertEqual(out['throttle'], 0)
        self.assertGreater(out['brake'], 0)
        self.assertEqual(self.c.detail['goal_speed_kmh'], 16)
        data['navigation'].update(distance_2d_m=31, distance_3d_m=31)
        self.update(data)
        self.assertLess(self.c.detail['goal_speed_kmh'], 10)

    def test_entry_latches_stop_while_waiting_for_real_job_notice(self):
        data = boarding(distance=29.9, speed=8, inside=True)
        self.start(data)
        out = self.update(data)
        self.assertEqual(self.c.phase, 'boarding_hold')
        self.assertGreater(out['brake'], 0)
        self.assertEqual(out['throttle'], 0)
        self.assertEqual(self.c.detail['parking_hold_reason'], 'marker_entry')
        data.update(speed_kmh=0, velocity_body_rfu_mps=[0, 0, 0])
        data['navigation']['marker_inside'] = False
        for i in range(50):
            out = self.update(data, clear=i >= 5)
            self.assertEqual(out['throttle'], 0)
            self.assertEqual(out['brake'], 0)
            self.assertTrue(out['handbrake'])
        self.assertEqual(self.c.phase, 'boarding_hold')
        self.assertNotEqual(self.c.instruction, 'passengers')
        data['navigation'] = None
        for _ in range(30):
            self.assertEqual(self.update(data)['throttle'], 0)
        self.assertTrue(self.c.enabled)
        self.c.notify(self.c, 'Ожидайте, пока все пассажиры займут свои места в салоне', self.now)
        self.update(data)
        self.assertEqual(self.c.phase, 'passengers')

    def test_next_reverse_instruction_releases_boarding_hold(self):
        data = boarding(distance=29, inside=True)
        self.start(data)
        self.update(data)
        self.c.notify(self.c, 'Двигайтесь аккуратно назад для начала маршрута', self.now)
        data['navigation'].update(position=[0, -100, 1.2], heading_error_deg=180,
            bearing_deg=180, color_rgba=[255, 0, 0, 255], marker_inside=False)
        for _ in range(6):
            out = self.update(data)
        self.assertIsNone(self.c.parkingHold)
        self.assertEqual(self.c.phase, 'reverse')
        self.assertGreater(out['brake'], 0)

    def test_moved_marker_reusing_id_releases_hold(self):
        data = boarding(distance=29, inside=True)
        self.start(data)
        self.update(data)
        data['navigation'].update(position=[0, 150, 1.2], distance_2d_m=179, distance_3d_m=179, marker_inside=False)
        self.assertGreater(self.update(data)['throttle'], 0)
        self.assertEqual(self.c.phase, 'boarding_approach')

    def test_geometric_fallback_scales_with_marker_size(self):
        for size in (10, 30, 60):
            self.setUp()
            data = boarding(size=size, distance=size * .94, inside=None)
            self.start(data)
            out = self.update(data)
            self.assertEqual(out['throttle'], 0)
            self.assertEqual(self.c.phase, 'boarding_hold')
            self.assertEqual(self.c.detail['parking_hold_reason'], 'inner_boundary')

    def test_missing_size_holds_instead_of_guessing_a_parking_position(self):
        for size in (None, 0, float('nan')):
            self.setUp()
            data = boarding(size=size, speed=10)
            self.start(data)
            out = self.update(data)
            self.assertEqual(out['throttle'], 0)
            self.assertGreater(out['brake'], 0)
            self.assertEqual(self.c.phase, 'boarding_hold')

    def test_inside_red_taxi_awaits_ack_without_becoming_parking_hold(self):
        data = boarding(distance=29, inside=True)
        data['navigation']['color_rgba'] = [255, 0, 0, 255]
        self.start(data)
        self.assertEqual(self.update(data)['throttle'], 0)
        self.assertEqual(self.c.phase, 'taxi_reentry_wait')
        self.assertIsNone(self.c.parkingHold)
        self.setUp()
        data = boarding(distance=25, speed=190, inside=True)
        data.update(on_ground=False, agl_terrain_m=100)
        data['navigation']['marker_type'] = 'ring'
        self.start(data)
        self.assertGreater(self.update(data)['throttle'], 0)
        self.assertEqual(self.c.phase, 'flight')

    def test_response_lag_brakes_near_edge_not_nine_metres_from_centre(self):
        # Uncertain longitudinal response; not a simulation of the airport geometry.
        for brake_gain, lag in ((3.0, .30), (6.0, .2), (9.0, .15)):
            self.setUp()
            distance, speed, acceleration = 50.96, 0, 0
            data = boarding(distance=distance)
            self.start(data)
            saw_entry = False
            for _ in range(600):
                data['navigation'].update(distance_2d_m=distance, distance_3d_m=distance, marker_inside=distance <= 30)
                data.update(position_m=[0, -distance, 1.2], speed_kmh=speed * 3.6, velocity_body_rfu_mps=[0, speed, 0])
                out = self.update(data)
                self.assertTrue(self.c.enabled, self.c.status)
                goal_accel = 16 * out['throttle'] - brake_gain * out['brake'] - .12 * speed
                acceleration += (goal_accel - acceleration) * .05 / lag
                speed = max(0, speed + acceleration * .05)
                if out['handbrake']:
                    speed *= .6
                distance -= speed * .05
                saw_entry |= distance <= 30
                if self.c.phase == 'boarding_hold' and speed < .02:
                    break
            self.assertTrue(saw_entry, (brake_gain, lag, distance))
            self.assertEqual(self.c.phase, 'boarding_hold')
            self.assertGreater(distance, 25, (brake_gain, lag, distance))


class BoardingAdapterTests(unittest.TestCase):
    setUp, arm = controller_tests.AdapterTests.setUp, controller_tests.AdapterTests.arm
    run_lua, records = controller_tests.AdapterTests.run_lua, controller_tests.AdapterTests.records

    def test_api_membership_is_logged_and_stops_already_inside_plane(self):
        self.run_lua('''
            function getMarkerSize() return 30 end
            function getMarkerColor() return 255,255,0,255 end
            m1.position={0,29,6.2}
        ''')
        self.arm()
        sample = self.records('sample')[-1]['data']
        self.assertTrue(sample['navigation']['vehicle_inside_marker'])
        self.assertTrue(sample['navigation']['marker_inside'])
        self.assertEqual(sample['autopilot']['phase'], 'boarding_hold')
        self.assertEqual(self.lua.eval('analog.accelerate'), 0)
        self.assertEqual(self.records('collector_error'), [])

    def test_unavailable_membership_is_unknown_and_uses_boundary_fallback(self):
        self.run_lua('''
            function getMarkerSize() return 30 end
            function getMarkerColor() return 255,255,0,255 end
            function isElementWithinMarker() return nil end
            m1.position={0,28.5,6.2}
        ''')
        self.arm()
        sample = self.records('sample')[-1]['data']
        self.assertIsNone(sample['navigation']['marker_inside'])
        self.assertIsNone(sample['navigation']['vehicle_inside_marker'])
        self.assertEqual(sample['autopilot']['decision']['parking_hold_reason'], 'inner_boundary')
        self.assertEqual(self.lua.eval('analog.accelerate'), 0)


if __name__ == '__main__':
    unittest.main()
