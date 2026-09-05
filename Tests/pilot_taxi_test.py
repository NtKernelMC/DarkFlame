"""Recorded taxi regression; geometry checks use synthetic walls, not a game replay."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller_tests
from pilot_controller_test import observation, controller_source
from pilot_telemetry_test import ROOT, LuaRuntime

FIXTURE = json.loads(Path(__file__).with_name('pilot_taxi_crash_fixture.json').read_text(encoding='utf-8'))


class TaxiTests(unittest.TestCase):
    setUp, start, update = controller_tests.MathTests.setUp, controller_tests.MathTests.start, controller_tests.MathTests.update

    def handoff(self):
        d = copy.deepcopy(FIXTURE['handoff'])
        d['navigation']['heading_error_deg'] = -179.4
        self.c.notify(self.c, 'Двигайтесь аккуратно назад для начала маршрута', self.now)
        self.start(d)
        self.update(d)
        self.c.notify(self.c, 'Двигайтесь медленно по меткам для выруливания на взлетную полосу', self.now)
        d['navigation'] = copy.deepcopy(FIXTURE['handoff']['navigation'])
        for _ in range(5):
            out = self.update(d)
        return d, out

    def test_recorded_side_marker_keeps_pushback_and_turns_nose_correctly(self):
        d, out = self.handoff()
        self.assertEqual(self.c.phase, 'pushback_align')
        self.assertEqual(self.c.detail['motion_direction'], -1)
        self.assertLess(out['rudder'], -.9)
        self.assertEqual(out['throttle'], 0)
        self.assertLessEqual(self.c.detail['goal_speed_kmh'], 6)

    def test_pushback_to_forward_requires_stationary_dwell_and_reverses_rudder(self):
        d, _ = self.handoff()
        d['navigation']['heading_error_deg'] = 45
        for _ in range(8):
            out = self.update(d)
            self.assertEqual(self.c.phase, 'direction_change')
            self.assertEqual(out['rudder'], 0)
            self.assertEqual(out['brake'], 0)
        d.update(speed_kmh=0, velocity_body_rfu_mps=[0, 0, 0])
        for _ in range(4):
            self.assertEqual(self.update(d)['throttle'], 0)
        self.assertGreater(self.update(d)['throttle'], 0)
        self.assertEqual(self.c.phase, 'taxi')
        d.update(speed_kmh=3, velocity_body_rfu_mps=[0, 3/3.6, 0])
        self.assertGreater(self.update(d)['rudder'], 0)

    def test_failed_pushback_does_not_drive_back_indefinitely(self):
        d, _ = self.handoff()
        for _ in range(245):
            self.update(d)
        self.assertFalse(self.c.enabled)
        self.assertIn('ручной перехват', self.c.status)

    def test_large_forward_turn_creeps_with_full_rudder_not_old_ten_kmh(self):
        d = copy.deepcopy(FIXTURE['preimpact'])
        self.start(d)
        out = self.update(d)
        self.assertGreater(out['brake'], 0)
        self.assertEqual(out['throttle'], 0)
        self.assertLessEqual(self.c.detail['goal_speed_kmh'], 4)
        self.assertGreater(out['rudder'], .8)

    def test_mirrored_pushback_turn(self):
        d, _ = self.handoff()
        d['navigation']['heading_error_deg'] = -93
        self.assertEqual(self.update(d)['rudder'], 0)
        for _ in range(3): self.update(d)
        self.assertGreater(self.update(d)['rudder'], .9)

    def test_obstacle_in_reverse_stops_before_any_align_command(self):
        d, _ = self.handoff()
        out = self.update(d, clear=False)
        self.assertEqual(self.c.phase, 'obstacle_hold')
        self.assertGreater(out['throttle'], 0)  # counter-thrust while physically rolling back
        self.assertEqual(out['rudder'], 0)

    def test_pushback_finishes_in_lagged_ground_model(self):
        d, _ = self.handoff()
        forward = d['velocity_body_rfu_mps'][1]
        yaw = 0
        saw_stopped = False
        for _ in range(300):
            out = self.update(d)
            self.assertTrue(self.c.enabled, self.c.status)
            acceleration = 6.5*out['throttle'] - 4*out['brake'] - .4*forward
            forward += acceleration*.05
            if out['handbrake']:
                forward *= .3
            yaw_goal = math.copysign(1, forward)*8.8*out['rudder']*min(1, abs(forward))
            yaw += (yaw_goal-yaw)*.05/.25
            d['heading_deg'] = (d['heading_deg']+yaw*.05) % 360
            radians = math.radians(d['heading_deg'])
            d['position_m'][0] += math.sin(radians)*forward*.05
            d['position_m'][1] += math.cos(radians)*forward*.05
            target = d['navigation']['position']
            dx, dy = target[0]-d['position_m'][0], target[1]-d['position_m'][1]
            bearing = math.degrees(math.atan2(dx, dy)) % 360
            error = (bearing-d['heading_deg']+180) % 360 - 180
            distance = math.hypot(dx, dy)
            d['navigation'].update(bearing_deg=bearing, heading_error_deg=error, distance_2d_m=distance, distance_3d_m=distance)
            d.update(speed_kmh=abs(forward)*3.6, velocity_body_rfu_mps=[0,forward,0], heading_rate_dps=yaw)
            saw_stopped |= abs(forward)<.18 and self.c.phase=='direction_change'
            if forward > .4 and self.c.phase=='taxi':
                self.assertTrue(saw_stopped)
                self.assertLess(abs(error), 55)
                return
        self.fail('No completed pushback / stationary handoff / forward taxi in 15 seconds')


class JobContextTests(unittest.TestCase):
    setUp = controller_tests.AdapterTests.setUp
    run_lua, records, arm = controller_tests.AdapterTests.run_lua, controller_tests.AdapterTests.records, controller_tests.AdapterTests.arm

    def test_destination_before_bot_and_recording_is_preserved_and_logged_as_context(self):
        self.run_lua("event('province:sendNotification',pilotRoot,'Ваш пункт назначения - Либерти Сити'); step(12)")
        self.assertEqual(self.lua.globals().ui['destination'], 'Либерти Сити')
        self.assertEqual(self.records('job_notification'), [])
        self.arm()
        self.assertEqual(self.lua.globals().ui['destination'], 'Либерти Сити')
        ctx = self.records('job_context')[-1]['data']
        self.assertEqual(ctx['destination'], 'Либерти Сити')
        self.assertLess(ctx['observed_tick_ms'], self.records('recording_start')[-1]['tick_ms'])
        self.assertIn('Либерти Сити', self.lua.globals().ui['notification'])

    def test_unrelated_notice_cannot_rename_route_and_new_job_updates_it(self):
        self.run_lua("event('province:sendNotification',pilotRoot,'Ваш пункт назначения - Либерти Сити')")
        self.arm()
        self.run_lua("event('province:sendNotification',resourceRoot,'Ваш пункт назначения - Неверный город'); step(12)")
        self.assertEqual(self.lua.globals().ui['destination'], 'Либерти Сити')
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(12); event('province:sendNotification',pilotRoot,'Ваш пункт назначения - Новый город')")
        self.assertEqual(self.lua.globals().ui['destination'], 'Новый город')
        self.arm()
        self.assertEqual(self.records('job_context')[-1]['data']['destination'], 'Новый город')

    def test_full_probes_logged_once_and_samples_reference_them(self):
        self.run_lua('m1.position={150,0,6.2}')
        self.arm()
        self.run_lua('step(60)')
        probes = self.records('autopilot_obstacle_probe')
        self.assertEqual(len(probes), len({r['data']['tick_ms'] for r in probes}))
        lookup = {r['data']['tick_ms']: r['data'] for r in probes}
        for sample in self.records('sample'):
            probe = sample['data']['autopilot']['obstacle_probe']
            self.assertNotIn('rays', probe)
            self.assertEqual(probe['ray_count'], len(lookup[probe['tick_ms']]['rays']))


class GeometryTests(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.cls = self.lua.execute(controller_source())
        self.lua.globals().autopilot = self.cls.new()
        self.lua.execute('''
            state, NULL, calls, now = {vehicle={}}, {}, {}, 1000
            function finite(v) return type(v)=='number' and v==v and math.abs(v)<math.huge end
            function number(v) if finite(v) then return v end end
            function elapsed(t,b) return b and (t-b)%4294967296 or 0 end
            function angle(v) return (v+180)%360-180 end
            function getTickCount() return now end
            function read(name,...) return _G[name](...) end
            function getElementBoundingBox() return -16.96250153,-20.21672249,3.944388628,16.96250153,14.28909397,16.36450005 end
            function isLineOfSightClear(...)
                local args={...}; assert(args[14]==state.vehicle)
                calls[#calls+1]=args
                return not wall(...) end
        ''')
        source = (ROOT / 'bin/Release/x86/PilotTelemetry.lua').read_text(encoding='utf-8')
        self.probe = self.lua.execute(source[source.index('local function groundProbe('):source.index('applyAutopilot = function(')] + '\nreturn groundProbe')
        self.lua.globals().wall = lambda *args: False

    def check(self, data, tick=1000):
        self.lua.globals().now = tick
        return self.probe(self.lua.table_from(data, recursive=True), tick)

    def test_recorded_contact_height_was_missed_by_old_rays(self):
        contact = FIXTURE['collision']['position']
        normal = FIXTURE['collision']['normal']
        def wall(*args):
            a, b = args[:3], args[3:6]
            da = sum((a[i]-contact[i])*normal[i] for i in range(3))
            db = sum((b[i]-contact[i])*normal[i] for i in range(3))
            if da*db > 0 or abs(da-db)<1e-8:
                return False
            t = da/(da-db)
            z = a[2] + (b[2]-a[2])*t
            return 31 <= z <= 35
        self.lua.globals().wall = wall
        d = FIXTURE['preimpact']
        self.assertTrue(all(not wall(*(r['from']+r['to'])) for r in d['old_probe']['rays']))
        self.assertIs(self.check(d), False)
        self.assertEqual(self.lua.globals().state['probe']['status'], 'blocked')
        self.assertGreater(self.lua.globals().state['probe']['blocked_ray'], 0)

    def test_turn_sweeps_rotate_wingtips_and_tail_even_before_yaw_begins(self):
        d = observation(speed_kmh=3, heading_rate_dps=0, position_m=[0, 0, 28.8],
            basis_world_rfu=[[1,0,0], [0,1,0], [0,0,1]])
        d['navigation'].update(heading_error_deg=90)
        self.assertIs(self.check(d), True)
        probe = self.lua.globals().state['probe']
        self.assertGreater(probe['predicted_yaw_deg'], 0)
        sweeps = [r for _,r in probe['rays'].items() if r['kind']=='turn_sweep']
        self.assertTrue(any(r['part']=='nose' and r['to'][1]>r['from'][1] for r in sweeps))
        self.assertTrue(any(r['part']=='tail' and r['to'][1]<r['from'][1] for r in sweeps))
        self.assertLessEqual(len(probe['rays']), 33)
        for r in sweeps:
            self.assertTrue(all(math.isfinite(x) for _,x in r['to'].items()))

    def test_direction_change_invalidates_cache_and_checks_actual_roll(self):
        d = copy.deepcopy(FIXTURE['handoff'])
        self.lua.globals().autopilot['instruction'] = 'reverse'
        self.check(d)
        count = len(self.lua.globals().calls)
        self.check(d, 1050)
        self.assertEqual(count, len(self.lua.globals().calls))
        self.lua.globals().autopilot['instruction'] = 'taxi'
        self.check(d, 1100)
        probe = self.lua.globals().state['probe']
        self.assertEqual(probe['tick_ms'], 1100)
        self.assertEqual(probe['motion_direction'], -1)
        self.assertEqual(probe['requested_direction'], 1)
        self.assertTrue(any(r['kind']=='reaction' for _,r in probe['rays'].items()))
        self.assertTrue(any(r['kind'] in ('planned', 'turn_sweep') for _,r in probe['rays'].items()))

    def test_slow_world_query_stops_probe_with_unknown_clearance(self):
        self.lua.execute('function isLineOfSightClear(...) now=now+8; return true end')
        self.assertIsNone(self.check(FIXTURE['preimpact']))
        self.assertFalse(self.lua.globals().state['probe']['complete'])
        self.assertEqual(len(self.lua.globals().state['probe']['rays']), 1)


if __name__ == '__main__':
    unittest.main()
