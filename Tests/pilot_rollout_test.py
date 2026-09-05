"""Runway rollout, live job deadlines and position-based progress under recorded stalls."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller

FIXTURE = json.loads(Path(__file__).with_name('pilot_rollout_fixture.json').read_text(encoding='utf-8'))


class RolloutTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def initial(self):
        d = copy.deepcopy(FIXTURE['samples'][0]['data'])
        self.start(d)
        self.c.landing = self.c.flightSeen = self.c.finalLanding = True
        self.c.landingHeading = FIXTURE['initial_landing_heading_deg']
        return d

    def test_recorded_touchdown_retains_a_deadline_reserve_with_bounded_speed(self):
        d = self.initial()
        out = self.update(d)
        self.assertEqual(self.c.phase, 'landing_rollout')
        self.assertEqual(out['brake'], 1)
        self.assertGreater(self.c.detail['goal_speed_kmh'], 35)
        self.assertLessEqual(self.c.detail['goal_speed_kmh'], 55)
        self.assertAlmostEqual(self.c.detail['rollout_linear_margin_s'], self.c.detail['rollout_time_reserve_s'], places=4)

    def test_speed_falls_to_turn_entry_limit_before_ring_boundary(self):
        d = self.initial()
        self.c.finalLanding = False
        d.update(speed_kmh=55, horizontal_speed_kmh=55, velocity_body_rfu_mps=[0, 55/3.6, 0])
        d['waypoint_timer']['remaining_s'] = 3
        goals, limited = [], []
        for distance in (160, 100, 70, 55, 40, 30):
            d['navigation'].update(distance_2d_m=distance, distance_3d_m=distance)
            out = self.update(d)
            goals.append(self.c.detail['goal_speed_kmh'])
            limited.append(self.c.detail['rollout_deadline_limited'])
            if distance < 55:
                self.assertGreater(out['brake'], 0)
        self.assertEqual(goals, sorted(goals, reverse=True))
        self.assertEqual(goals[-1], 18)
        self.assertTrue(any(limited))

    def test_turn_reverse_checkpoint_stale_timer_and_missing_next_target_do_not_get_boost(self):
        for fault in ('turn', 'reverse', 'checkpoint', 'timer_stale', 'timer_absent', 'next_absent', 'frozen', 'airborne'):
            with self.subTest(fault=fault):
                self.setUp()
                d = self.initial()
                if fault == 'turn': d['navigation']['heading_error_deg'] = 40
                if fault == 'reverse': self.c.instruction = 'reverse'
                if fault == 'checkpoint': d['navigation']['marker_type'] = 'checkpoint'
                if fault == 'timer_stale': d['waypoint_timer']['age_ms'] = 2501
                if fault == 'timer_absent': d.pop('waypoint_timer')
                if fault == 'next_absent': d['navigation'].pop('next_position')
                if fault == 'frozen': d['frozen'] = True
                if fault == 'airborne': d['on_ground'] = False
                self.assertIsNone(self.c.rolloutPlan(self.c, self.lua.table_from(d, recursive=True)))

    def test_progress_estimator_uses_position_and_resets_for_new_flight(self):
        d = self.initial()
        self.c.finalLanding = False
        d.update(speed_kmh=35, horizontal_speed_kmh=35, velocity_body_rfu_mps=[0, 35/3.6, 0])
        self.update(d)
        for _ in range(60):
            d['position_m'][1] -= 35/3.6*.05*.4
            self.update(d)
        self.assertAlmostEqual(self.c.groundProgress['measuredRatio'], .4, places=4)
        self.assertLess(self.c.groundProgress['ratio'], .5)
        self.assertAlmostEqual(self.c.groundProgress['wallSpeed'], 14, places=4)
        self.c.stop(self.c, 'restart')
        self.start(d)
        self.assertIsNone(self.c.groundProgress)

    def test_deadline_never_overrides_a_blocked_or_unknown_ground_path(self):
        for clear in (False, None):
            with self.subTest(clear=clear):
                self.setUp()
                d = self.initial()
                self.c.finalLanding = False
                d.update(speed_kmh=35, horizontal_speed_kmh=35, velocity_body_rfu_mps=[0, 35/3.6, 0])
                d['waypoint_timer']['remaining_s'] = 4
                out = self.update(d, clear=clear)
                self.assertEqual(out['throttle'], 0)
                self.assertGreater(out['brake'], 0)
                self.assertEqual(self.c.detail['goal_speed_kmh'], 0)

    def test_uncertain_path_keeps_full_landing_brake_below_the_higher_rollout_speed(self):
        for clear in (False, None):
            self.setUp()
            d = self.initial()
            d.update(speed_kmh=45, horizontal_speed_kmh=45, velocity_body_rfu_mps=[0, 45/3.6, 0])
            d['waypoint_timer']['remaining_s'] = 8
            out = self.update(d, clear=clear)
            self.assertEqual(out['brake'], 1)
            self.assertEqual(out['throttle'], 0)
            self.assertEqual(self.c.phase, 'landing_rollout')

    def test_live_geometry_is_invariant_under_airport_translation_and_rotation(self):
        d = self.initial()
        original = self.c.rolloutPlan(self.c, self.lua.table_from(d, recursive=True))
        rotated = copy.deepcopy(d)
        theta = math.radians(83)
        for p in (rotated['position_m'], rotated['navigation']['position'], rotated['navigation']['next_position']):
            x, y = p[:2]
            p[:] = [x*math.cos(theta)+y*math.sin(theta)+1200,
                    -x*math.sin(theta)+y*math.cos(theta)-500, p[2]+50]
        rotated['heading_deg'] = (rotated['heading_deg']+83) % 360
        rotated['navigation']['bearing_deg'] = (rotated['navigation']['bearing_deg']+83) % 360
        transformed = self.c.rolloutPlan(self.c, self.lua.table_from(rotated, recursive=True))
        for name in ('speed', 'cap', 'entry', 'nextTurn', 'required'):
            self.assertAlmostEqual(original[name], transformed[name], places=6)

    def simulate(self, adaptive=True, throttle_gain=32, brake_gain=1, progress_scale=1, recorded_timer=False):
        self.setUp()
        d = self.initial()
        if not adaptive:
            self.c.rolloutPlan = self.lua.eval('function() return nil end')
        start_position = d['position_m'][:]
        target = d['navigation']['position']
        dx, dy = target[0]-start_position[0], target[1]-start_position[1]
        distance = math.hypot(dx, dy)
        ux, uy = dx/distance, dy/distance
        d.update(heading_deg=math.degrees(math.atan2(ux, uy)) % 360, heading_rate_dps=0, roll_deg=0)
        d['navigation'].update(bearing_deg=d['heading_deg'], heading_error_deg=0)
        self.c.landingHeading = d['heading_deg']
        ratios = {v['from_s']: min(1, v['ratio']) for v in FIXTURE['progress_profile']}
        coast = FIXTURE['plant_estimates']['coast_kmh_s_median']
        brake = FIXTURE['plant_estimates']['full_brake_kmh_s_median'] - coast
        deadline = d['waypoint_timer']['remaining_s']
        if recorded_timer:
            deadline = (FIXTURE['expiry_tick_ms']-FIXTURE['begin_tick_ms'])/1000
        speed, travelled, peak_goal = d['speed_kmh'], 0, 0
        for i in range(math.ceil(deadline/.05)):
            wall_time = i*.05
            if recorded_timer:
                tick = FIXTURE['begin_tick_ms']+wall_time*1000
                update = max((r for r in FIXTURE['timer_updates'] if r['tick_ms']<=tick), key=lambda r:r['tick_ms'])
                age = tick-update['tick_ms']
                d['waypoint_timer'].update(remaining_s=max(0, update['seconds']-age/1000), age_ms=age)
            else:
                d['waypoint_timer'].update(remaining_s=max(0, deadline-wall_time), age_ms=0)
            out = self.update(d)
            self.assertTrue(self.c.enabled, self.c.status)
            peak_goal = max(peak_goal, self.c.detail['goal_speed_kmh'])
            # Measured speed decay; throttle gain and braking response are varied,
            # since this is a longitudinal approximation, not GTA physics.
            physical_dt = .05*ratios.get(int(wall_time), 1)*progress_scale
            speed = max(0, speed + (throttle_gain*out['throttle'] - brake*brake_gain*out['brake'] - coast)*physical_dt)
            travelled += speed/3.6*physical_dt
            remaining = distance-travelled
            d['position_m'] = [start_position[0]+ux*travelled, start_position[1]+uy*travelled, start_position[2]]
            d.update(speed_kmh=speed, horizontal_speed_kmh=speed, velocity_body_rfu_mps=[0, speed/3.6, 0])
            d['navigation'].update(distance_2d_m=remaining, distance_3d_m=math.hypot(remaining, d['navigation']['altitude_error_m']))
            if remaining <= d['navigation']['marker_size_m']-2:
                return dict(arrived=True, time=wall_time+.05, speed=speed, remaining=remaining, peak_goal=peak_goal)
        return dict(arrived=False, time=deadline, speed=speed, remaining=remaining, peak_goal=peak_goal)

    def test_recorded_stall_profile_arrives_before_deadline_and_slows_for_next_turn(self):
        old = self.simulate(adaptive=False)
        self.assertFalse(old['arrived'], old)
        results = []
        for throttle, braking, progress in ((20, 1, 1), (32, 1, 1), (45, 1, 1), (32, .8, 1), (32, 1.2, 1), (32, 1, .9)):
            with self.subTest(throttle=throttle, braking=braking, progress=progress):
                result = self.simulate(throttle_gain=throttle, brake_gain=braking, progress_scale=progress)
                self.assertTrue(result['arrived'], result)
                self.assertLessEqual(result['peak_goal'], 55)
                self.assertLessEqual(result['speed'], 20)
                results.append(result)
        self.simulation_results = results

    def test_actual_timer_updates_and_expiry_from_log_also_arrive_in_time(self):
        old = self.simulate(adaptive=False, recorded_timer=True)
        self.assertFalse(old['arrived'], old)
        for throttle, progress in ((20, 1), (32, 1), (45, 1), (32, .9)):
            with self.subTest(throttle=throttle, progress=progress):
                result = self.simulate(recorded_timer=True, throttle_gain=throttle, progress_scale=progress)
                self.assertTrue(result['arrived'], result)
                self.assertLessEqual(result['speed'], 20)


class TimerTests(unittest.TestCase):
    setUp, arm = controller.AdapterTests.setUp, controller.AdapterTests.arm
    run_lua, records = controller.AdapterTests.run_lua, controller.AdapterTests.records

    def init_timer(self, source='pilotRoot', seconds=30, title='Достигните следующей точки'):
        self.run_lua(f"event('plrTimer:init',{source},{seconds},'Пилот','{title}')")

    def timer(self):
        return self.records('sample')[-1]['data'].get('waypoint_timer')

    def test_timer_is_captured_before_ap_start_and_reads_next_ring_position(self):
        self.init_timer()
        self.run_lua("function getMarkerTarget() return 0,300,6.2 end; m1.markerType='ring'")
        self.arm()
        self.assertIsNotNone(self.timer())
        self.assertLess(self.timer()['remaining_s'], 30)
        self.assertEqual(self.records('sample')[-1]['data']['navigation']['next_position'], [0, 300, 6.2])

    def test_seconds_update_requires_same_source_and_old_source_cannot_replace_new_timer(self):
        self.arm()
        self.run_lua("oldTimer={kind='timer',valid=true,parent=pilotRoot}; newTimer={kind='timer',valid=true,parent=pilotRoot}")
        self.init_timer('oldTimer')
        self.init_timer('newTimer', 20)
        self.run_lua("event('plrTimer:secs',oldTimer,1); step(8)")
        self.assertEqual(self.timer()['reported_s'], 20)
        self.run_lua("event('plrTimer:secs',newTimer,19); step(8)")
        self.assertEqual(self.timer()['reported_s'], 19)
        self.run_lua("event('onClientElementDestroy',oldTimer); step(8)")
        self.assertIsNotNone(self.timer())
        self.run_lua("event('onClientElementDestroy',newTimer); step(8)")
        self.assertIsNone(self.timer())

    def test_unrelated_timer_or_passenger_timer_does_not_authorize_rollout_boost(self):
        self.arm()
        self.init_timer('resourceRoot')
        self.run_lua('step(8)')
        self.assertIsNone(self.timer())
        self.init_timer()
        self.init_timer(seconds=30, title='Ожидание посадки пассажиров')
        self.run_lua('step(8)')
        self.assertIsNone(self.timer())

    def test_static_fallback_does_not_refresh_deadline_on_identical_snapshot(self):
        self.arm()
        self.run_lua("n={kind='notifications:Static',valid=true,parent=pilotRoot,"
                     "data={header='Пилот',text='Достигните следующей точки',min=0,sec=20,timer=true}}; "
                     "notifications[1]=n; event('onClientElementDataChange',n,'data'); step(8)")
        remaining = self.timer()['remaining_s']
        self.run_lua('step(30)')
        self.assertLess(self.timer()['remaining_s'], remaining-1)
        self.run_lua("event('onClientElementDestroy',n); n.valid=false; step(8)")
        self.assertIsNone(self.timer())

    def test_timer_updates_with_black_box_disabled_and_does_not_depend_on_file(self):
        self.arm(False)
        self.init_timer(seconds=17)
        self.run_lua("step(8); commands[#commands+1]='autopilot_telemetry:1'; step(8)")
        self.assertEqual(self.timer()['reported_s'], 17)
        self.assertLess(self.timer()['remaining_s'], 17)

    def test_rollout_controller_receives_live_timer_and_applies_throttle_without_black_box(self):
        self.run_lua("ground=false; plane.position={0,0,9.2}; plane.velocity={0,.2,0}; "
                     "m1.markerType='ring'; m1.position={0,350,9.2}; "
                     "function getMarkerTarget() return -25,400,7.2 end")
        self.arm(False)
        self.run_lua("ground=true; plane.position={0,0,6.2}; m1.position={0,350,7.2}; "
                     "event('province:sendNotification',pilotRoot,'Выпустите шасси.')")
        self.init_timer()
        self.run_lua('step(20)')
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)
        self.assertLessEqual(self.lua.eval('analog.accelerate'), .35)
        self.assertEqual(self.records(), [])


if __name__ == '__main__':
    unittest.main()
