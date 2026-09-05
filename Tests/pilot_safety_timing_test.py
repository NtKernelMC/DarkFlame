"""Proximity warnings and recovery from the short frame stall seen on Mirny."""
import unittest

import pilot_controller_test as controller


class TimingTests(unittest.TestCase):
    setUp = controller.MathTests.setUp
    start, update = controller.MathTests.start, controller.MathTests.update

    def test_short_stall_uses_fresh_aircraft_state_and_recovers_clock(self):
        for gap in (301, 444, 1000, 2500, 5000):
            with self.subTest(gap=gap):
                self.setUp()
                d = controller.observation(on_ground=False, agl_terrain_m=168,
                    position_m=[0, 0, 193.7], speed_kmh=260, horizontal_speed_kmh=260)
                d['navigation'].update(marker_type='ring', position=[0, 800, 193.7],
                    distance_2d_m=800, distance_3d_m=800)
                self.start(d)
                self.update(d)
                d['roll_deg'] = 20
                d['roll_rate_dps'] = 4
                out = self.update(d, ms=gap)
                self.assertTrue(self.c.enabled)
                self.assertLess(out['aileron'], 0)  # Correct the fresh bank, not the old level state.
                self.assertEqual(self.c.detail['frame_gap_ms'], gap)
                self.assertEqual(self.c.detail['timing_resync_reason'], 'short_stall')
                self.update(d)
                self.assertFalse(self.c.detail['timing_resynced'])
                self.assertIsNone(self.c.detail['timing_resync_reason'])

    def test_stall_recovery_keeps_vehicle_world_damage_position_and_attitude_checks(self):
        for change in ({'driver': False}, {'vehicle': 'other'}, {'dimension': 5},
                       {'health': 100}, {'in_water': True}, {'pitch_deg': float('nan')},
                       {'position_m': [50000, 0, 1000]}, {'roll_deg': 80}):
            with self.subTest(change=change):
                self.setUp()
                d = controller.observation(on_ground=False, agl_terrain_m=200, speed_kmh=250)
                d['navigation']['marker_type'] = 'ring'
                self.start(d)
                self.update(d)
                d.update(change)
                self.update(d, ms=444)
                self.assertFalse(self.c.enabled)

    def test_stall_recovery_does_not_reset_missing_marker_timeout(self):
        d = controller.observation(on_ground=False, agl_terrain_m=200, speed_kmh=250)
        self.start(d)
        self.update(d)
        d['navigation'] = None
        for _ in range(4):
            self.update(d, ms=444)
        self.assertFalse(self.c.enabled)
        self.assertIn('Нет текущего маркера', self.c.status)

    def test_new_start_does_not_inherit_recovery_permission(self):
        d = controller.observation()
        self.start(d)
        self.update(d)
        self.c.stop(self.c, 'test')
        self.start(d)
        self.update(d, ms=444)
        self.assertFalse(self.c.enabled)
        self.assertIn('при запуске', self.c.status)


class SafetyTests(unittest.TestCase):
    setUp = controller.AdapterTests.setUp
    arm = controller.AdapterTests.arm
    run_lua, records = controller.AdapterTests.run_lua, controller.AdapterTests.records

    def nearby(self, distance):
        self.run_lua('''
            localPlayer.position=plane.position
            nearby={kind='player',valid=true,parent=root,name='Nearby_Player',id=42,
                position={localPlayer.position[1]+%s,localPlayer.position[2],localPlayer.position[3]},
                dimension=0,interior=0}
            players[1]=nearby
        ''' % distance)

    def test_fifty_metre_boundary_only_sounds_without_stopping_controls_or_recorder(self):
        for distance, expected in ((50, 1), (50.01, 0), (194.0197448, 0)):
            with self.subTest(distance=distance):
                self.setUp()
                self.arm()
                self.nearby(distance)
                self.run_lua('step(45)')
                self.assertEqual(self.lua.eval('alertCalls'), expected)
                self.assertEqual(self.lua.globals().ui['autopilot'], '1')
                self.assertEqual(self.lua.globals().ui['recording'], '1')
                self.assertGreater(self.lua.eval('analog.accelerate'), 0)
                self.assertEqual(self.records('autopilot_stop'), [])
                self.assertEqual(self.lua.eval('#soundRequests'), 1)

    def test_short_frame_pause_after_siren_keeps_flying_and_records_resync(self):
        for recording in (False, True):
            with self.subTest(recording=recording):
                self.setUp()
                self.run_lua("ground=false; plane.position={0,0,193.7}; plane.velocity={0,1.45,0}; "
                             "m1.markerType='ring'; m1.position={0,800,193.7}")
                self.arm(recording)
                self.nearby(20)
                self.run_lua('step(45); frame(444); step(8)')
                self.assertEqual(self.lua.eval('alertCalls'), 1)
                self.assertEqual(self.lua.globals().ui['autopilot'], '1')
                self.assertEqual(self.records('autopilot_stop'), [])
                if recording:
                    resync = self.records('autopilot_timing_resync')[-1]['data']
                    self.assertEqual(resync['reason'], 'short_stall')
                    self.assertEqual(resync['frame_gap_ms'], 444)
                    self.assertEqual(self.lua.globals().ui['recording'], '1')

    def test_slow_sound_call_is_measured_and_clock_refreshed_before_control_update(self):
        self.arm()
        self.nearby(20)
        self.run_lua('alertDelayMs=444; step(45)')
        alert = self.records('safety_alert')[-1]['data']
        self.assertEqual(alert['play_call_ms'], 444)
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertEqual(self.records('autopilot_timing_resync')[-1]['data']['reason'], 'short_stall')
        self.assertEqual(self.records('autopilot_stop'), [])


if __name__ == '__main__':
    unittest.main()
