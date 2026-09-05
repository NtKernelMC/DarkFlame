"""Liberty arrival, background lifecycle and delayed roll response regressions."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller
import pilot_flight_test as flight

FIXTURE = json.loads(Path(__file__).with_name('pilot_liberty_fixture.json').read_text(encoding='utf-8'))


class BackgroundTests(unittest.TestCase):
    arm = controller.AdapterTests.arm
    run_lua, records = controller.AdapterTests.run_lua, controller.AdapterTests.records
    assert_released = controller.AdapterTests.assert_released

    def setUp(self):
        controller.AdapterTests.setUp(self)
        self.run_lua('''
            timers={}
            function setTimer(fn,interval,repeats)
                local t={fn=fn,interval=interval,repeats=repeats,active=true}
                timers[#timers+1]=t; return t
            end
            function killTimer(t) t.active=false; return true end
            function timerTick(ms)
                now=now+ms
                for _,t in ipairs(timers) do if t.active then t.fn() end end
            end
        ''')

    def test_background_timer_controls_without_render_and_cleans_up_on_restore_and_unload(self):
        self.arm()
        self.run_lua("event('onClientMinimize',root); event('onClientMinimize',root)")
        self.assertEqual(self.lua.eval('#timers'), 1)
        before = len(self.records('sample'))
        self.run_lua('for i=1,30 do timerTick(50) end')
        self.assertGreater(len(self.records('sample')), before+20)
        self.assertEqual(self.records('sample')[-1]['data']['update_source'], 'background_timer')
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)
        self.run_lua("event('onClientRestore',root); step(10)")
        self.assertFalse(self.lua.eval('timers[1].active'))
        self.assertEqual(self.records('sample')[-1]['data']['update_source'], 'pre_render')
        self.run_lua("event('onClientMinimize',root); __DarkFlamePilotCleanup()")
        self.assertFalse(self.lua.eval('timers[2].active'))
        self.assert_released()

    def test_background_timer_does_not_duplicate_fresh_render_updates(self):
        self.arm()
        self.run_lua("event('onClientMinimize',root); frame(50)")
        calls = self.lua.eval('#actuatorCalls')
        self.run_lua('timerTick(1)')
        self.assertEqual(calls, self.lua.eval('#actuatorCalls'))

    def test_minimized_taxi_continues_controls_and_recording_after_long_frame_gap(self):
        self.arm()
        self.run_lua("event('onClientMinimize',root); frame(1800); step(15)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)
        self.assertEqual(self.records('autopilot_stop'), [])
        self.assertTrue(self.records('autopilot_timing_resync')[-1]['data']['minimized'])
        self.assertEqual(len(self.records('autopilot_sound')), 1)
        self.assertTrue(self.records('sample')[-1]['data']['window_minimized'])

    def test_restore_uses_fresh_clock_and_next_ordinary_stall_is_still_detected(self):
        self.arm()
        self.run_lua("event('onClientMinimize',root); now=now+4000; event('onClientRestore',root); step(6)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertTrue(self.records('autopilot_timing_resync')[-1]['data']['restored'])
        self.assertFalse(self.records('sample')[-1]['data']['window_minimized'])
        self.run_lua('frame(5001)')
        self.assertEqual(self.lua.globals().ui['autopilot'], '0')
        self.assertIn('5 с', self.records('autopilot_stop')[-1]['data']['reason'])
        self.assert_released()

    def test_frozen_unloading_stays_armed_and_resumes_reverse_in_background(self):
        self.arm()
        self.run_lua('''
            frozen=true; function isElementFrozen() return frozen end
            event('province:sendNotification',pilotRoot,'Ожидайте, пока все пассажиры выйдут, а новые займут свои места в салоне.')
            step(3); event('onClientMinimize',root); frame(3500); step(6)
        ''')
        self.assert_released()
        calls = self.lua.eval('#actuatorCalls')
        self.run_lua('step(40)')
        self.assertEqual(calls, self.lua.eval('#actuatorCalls'))
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.run_lua('''
            frozen=false; m1.position={0,-150,6.2}
            event('province:sendNotification',pilotRoot,'Двигайтесь аккуратно назад для начала маршрута')
            step(12)
        ''')
        self.assertGreater(self.lua.eval('analog.brake_reverse'), 0)
        self.assertEqual(self.records('autopilot_stop'), [])
        self.run_lua("event('province:sendNotification',pilotRoot,'Вы выполнили рейс!'); step(6)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '0')
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assert_released()


class ArrivalGuidanceTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def test_slow_background_frames_still_confirm_landing_and_release_frozen_plane(self):
        d = controller.observation(on_ground=False, agl_terrain_m=20, speed_kmh=150, window_minimized=True)
        self.start(d)
        d.update(on_ground=True, agl_terrain_m=1.2, speed_kmh=0, frozen=True)
        self.c.notify(self.c, 'Ожидайте, пока пассажиры выйдут', self.now)
        self.update(d, 800)
        self.update(d, 800)
        self.assertTrue(self.c.enabled)
        self.assertFalse(self.c.airborne)
        self.assertTrue(self.c.detail['controls_suspended'])

    def test_slow_background_frames_do_not_reset_missing_marker_timeout_forever(self):
        d = controller.observation(on_ground=False, agl_terrain_m=200, speed_kmh=250, window_minimized=True)
        self.start(d)
        self.update(d)
        d['navigation'] = None
        for _ in range(3):
            self.update(d, 800)
        self.assertFalse(self.c.enabled)
        self.assertIn('Нет текущего маркера', self.c.status)

    def test_long_background_gap_preserves_airborne_guidance_and_real_fault_checks(self):
        for fault in (None, 'driver', 'dimension', 'health', 'position_m'):
            with self.subTest(fault=fault):
                self.setUp()
                d = copy.deepcopy(FIXTURE['cases']['left_entry']['sample'])
                self.start(d)
                self.update(d)
                d['window_minimized'] = True
                if fault:
                    d[fault] = {'driver':False, 'dimension':5, 'health':100, 'position_m':[50000,0,1000]}[fault]
                out = self.update(d, 5000)
                self.assertEqual(self.c.enabled, fault is None)
                if fault is None:
                    self.assertTrue(self.c.detail['timing_resynced'])
                    self.assertEqual(self.c.detail['frame_gap_ms'], 5000)
                    self.assertLess(out['aileron'], 0)
                    self.assertEqual(self.c.phase, 'flight')

    def test_recorded_unloading_straight_is_faster_but_turn_and_edge_caps_remain(self):
        d = copy.deepcopy(FIXTURE['cases']['unloading_approach']['sample'])
        self.start(d)
        self.update(d)
        self.assertEqual(self.c.detail['goal_speed_kmh'], 16)
        d['navigation']['heading_error_deg'] = 40
        self.update(d)
        self.assertLessEqual(self.c.detail['goal_speed_kmh'], 7)
        d['navigation'].update(heading_error_deg=0, distance_2d_m=30.9, distance_3d_m=31)
        self.update(d)
        self.assertLess(self.c.detail['goal_speed_kmh'], 10)
        d['navigation']['marker_inside'] = True
        self.assertEqual(self.update(d)['throttle'], 0)
        self.assertEqual(self.c.phase, 'boarding_hold')

    def test_capture_corridor_avoids_recorded_opposite_bank_before_next_left_ring(self):
        rows = FIXTURE['capture_window']
        self.start(rows[0]['sample'])
        previous = rows[0]['elapsed_ms']
        saw_capture = False
        for row in rows:
            self.update(row['sample'], row['elapsed_ms'] - previous or 50)
            previous = row['elapsed_ms']
            self.assertTrue(self.c.enabled, self.c.status)
            if row['elapsed_ms'] == FIXTURE['cases']['near_ring']['elapsed_ms']:
                self.assertTrue(self.c.detail['ring_capture'])
                self.assertEqual(self.c.detail['goal_roll_deg'], 0)
                saw_capture = True
        self.assertTrue(saw_capture)
        self.assertFalse(self.c.detail['ring_capture'])
        self.assertLess(self.c.detail['goal_roll_deg'], -20)

    def test_capture_does_not_suppress_correction_outside_ring_or_with_unknown_size(self):
        d = copy.deepcopy(FIXTURE['cases']['near_ring']['sample'])
        for size, error, distance in ((30,14,85),(None,3,25),(0,3,25),(30,30,25)):
            with self.subTest(size=size, error=error, distance=distance):
                self.setUp()
                d['navigation'].update(marker_size_m=size, heading_error_deg=error,
                    track_error_deg=error, distance_2d_m=distance, distance_3d_m=distance)
                self.start(d)
                self.update(d)
                self.c.turnDirection = -1
                self.update(d)
                self.assertFalse(self.c.detail['ring_capture'])
                self.assertGreater(self.c.detail['goal_roll_deg'], 0)


class RollResponseTests(unittest.TestCase):
    setUp = flight.FlightDynamicsTests.setUp

    def test_recorded_gently_changing_bank_does_not_excite_delayed_roll_oscillation(self):
        # Approximate ARX response fitted to this log: gain ~94 deg/s, lag .3 s,
        # plus ~.126 s delay. Vary it: this is not a full GTA flight model.
        for gain, tau, delay in ((94,.3,.126),(72,.2,.1),(118,.45,.06)):
            with self.subTest(gain=gain, tau=tau, delay=delay):
                self.setUp()
                rows = FIXTURE['roll_window']
                d = copy.deepcopy(rows[0]['sample'])
                d['vehicle'] = 'plane'
                roll, rate = d['roll_deg'], d['roll_rate_dps']
                now, previous = 1000, rows[0]['elapsed_ms']
                self.assertIs(self.c.start(self.c,self.lua.table_from(d,recursive=True),now), True)
                history, results = [(0,0)], []
                for row in rows:
                    ms = row['elapsed_ms'] - previous or 63
                    dt, previous, now = ms/1000, row['elapsed_ms'], now+ms
                    d = copy.deepcopy(row['sample'])
                    d.update(vehicle='plane', roll_deg=roll, roll_rate_dps=rate)
                    self.c.update(self.c,self.lua.table_from(d,recursive=True),now,True)
                    self.assertTrue(self.c.enabled, self.c.status)
                    self.apply(now)
                    a = self.lua.globals().analog
                    command = a['vehicle_right'] - a['vehicle_left']
                    history.append((now/1000,command))
                    delayed = next(value for t,value in reversed(history) if t<=now/1000-delay)
                    rate += (gain*delayed-rate)*(1-math.exp(-dt/tau))
                    roll += rate*dt
                    results.append((roll,self.c.detail['goal_roll_deg'],command,rate))
                variation = sum(abs(b[2]-a[2]) for a,b in zip(results,results[1:]))
                rmse = math.sqrt(sum((a-b)**2 for a,b,_,_ in results)/len(results))
                self.assertLess(variation, 4)
                self.assertLess(rmse, 4.8)
                self.assertLess(max(abs(r[3]) for r in results), 36)


if __name__ == '__main__':
    unittest.main()
