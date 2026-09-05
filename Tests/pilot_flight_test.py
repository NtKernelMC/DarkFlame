"""Shared MTA input axes and recorded runway/ring regression; synthetic dynamics."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller
import pilot_taxi_test as taxi
from pilot_telemetry_test import ROOT, LuaRuntime

FIXTURE = json.loads(Path(__file__).with_name('pilot_flight_miss_fixture.json').read_text(encoding='utf-8'))['cases']


class AxisTests(unittest.TestCase):
    setUp = controller.AdapterTests.setUp
    run_lua, records, arm = controller.AdapterTests.run_lua, controller.AdapterTests.records, controller.AdapterTests.arm
    assert_released = controller.AdapterTests.assert_released

    def test_left_and_nose_down_are_not_cleared_by_opposing_zero(self):
        self.run_lua("ground=false; plane.position={0,0,100}; plane.velocity={0,1.3,0}; m1.position={-200,800,50}; m1.markerType='ring'")
        self.arm()
        self.assertGreater(self.lua.eval('analog.vehicle_left'), .1)
        self.assertGreater(self.lua.eval('analog.steer_forward'), .1)
        self.assertEqual(self.lua.eval('analog.vehicle_right'), 0)
        self.assertEqual(self.lua.eval('analog.steer_back'), 0)
        applied = self.records('sample')[-1]['data']['autopilot']['applied']
        self.assertEqual(applied['analog_readback'], applied['analog'])
        self.assertEqual(applied['readback_stage'], 'after_write_before_game_frame')
        self.run_lua("m1.position={200,800,150}; step(12)")
        self.assertGreater(self.lua.eval('analog.vehicle_right'), .1)
        self.assertGreater(self.lua.eval('analog.steer_back'), .05)
        self.assertEqual(self.lua.eval('analog.vehicle_left'), 0)
        self.assertEqual(self.lua.eval('analog.steer_forward'), 0)
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(12)")
        self.assert_released()


class RunwayTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def test_recorded_start_aligns_before_full_throttle_then_commits(self):
        d = copy.deepcopy(FIXTURE['takeoff_start']['sample'])
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.start(d)
        out = self.update(d)
        self.assertEqual(self.c.phase, 'takeoff_align')
        self.assertLessEqual(out['throttle'], .3)
        d.update(speed_kmh=12, velocity_body_rfu_mps=[0, 12/3.6, 0])
        self.assertGreater(self.update(d)['rudder'], .6)
        d['navigation']['heading_error_deg'] = 2
        self.assertEqual(self.update(d)['throttle'], 1)
        self.assertEqual(self.c.phase, 'takeoff')
        d['navigation']['heading_error_deg'] = 4
        self.assertEqual(self.update(d)['throttle'], 1)
        self.assertEqual(self.c.phase, 'takeoff')

    def test_start_mid_roll_does_not_force_a_slow_alignment_stop(self):
        d = copy.deepcopy(FIXTURE['rolling']['sample'])
        d['speed_kmh'] = 100
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.start(d)
        out = self.update(d)
        self.assertEqual(self.c.phase, 'takeoff')
        self.assertEqual(out['throttle'], 1)
        self.assertGreater(out['rudder'], .4)
        self.assertLessEqual(out['rudder'], .55)

    def test_alignment_waits_for_clearance_and_stops_for_obstacle(self):
        d = copy.deepcopy(FIXTURE['takeoff_start']['sample'])
        d.update(speed_kmh=14, velocity_body_rfu_mps=[0, 14/3.6, 0])
        self.start(d)
        self.assertEqual(self.update(d)['throttle'], 0)
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.assertGreater(self.update(d, clear=False)['brake'], 0)
        self.assertEqual(self.c.phase, 'obstacle_hold')

    def test_runway_alignment_reduces_recorded_side_drift_in_lagged_model(self):
        for gain, lag in ((.7, .3), (1, .2), (1.3, .15)):
            self.setUp()
            d = copy.deepcopy(FIXTURE['takeoff_start']['sample'])
            origin, target = d['position_m'][:], d['navigation']['position']
            dx, dy = target[0]-origin[0], target[1]-origin[1]
            length = math.hypot(dx, dy)
            dx, dy = dx/length, dy/length
            self.c.notify(self.c, 'Взлет разрешен!', self.now)
            self.start(d)
            speed = yaw = 0
            max_cross = 0
            for _ in range(300):
                px, py = target[0]-d['position_m'][0], target[1]-d['position_m'][1]
                bearing = math.degrees(math.atan2(px, py)) % 360
                error = (bearing-d['heading_deg']+180) % 360-180
                d['navigation'].update(bearing_deg=bearing, heading_error_deg=error,
                    distance_2d_m=math.hypot(px,py), distance_3d_m=math.hypot(px,py), track_error_deg=error)
                d.update(speed_kmh=speed*3.6, heading_rate_dps=yaw, velocity_body_rfu_mps=[0,speed,0])
                out = self.update(d)
                speed = max(0, speed + (9*out['throttle'] - 8*out['brake'] - .04*speed)*.05)
                yaw += (8.8*gain*out['rudder']-yaw)*.05/lag
                d['heading_deg'] = (d['heading_deg']+yaw*.05) % 360
                h = math.radians(d['heading_deg'])
                d['position_m'][0] += math.sin(h)*speed*.05
                d['position_m'][1] += math.cos(h)*speed*.05
                cross = abs((d['position_m'][0]-origin[0])*dy - (d['position_m'][1]-origin[1])*dx)
                max_cross = max(max_cross, cross)
                if speed*3.6 >= 145:
                    break
            self.assertGreaterEqual(speed*3.6, 145)
            self.assertLess(max_cross, 6, (gain,lag,max_cross))
            self.assertLess(abs(error), 3)


class ProbeRefreshTests(unittest.TestCase):
    setUp, check = taxi.GeometryTests.setUp, taxi.GeometryTests.check

    def data(self):
        return controller.observation(speed_kmh=10, velocity_body_rfu_mps=[0,10/3.6,0],
            position_m=[0,0,28.8], basis_world_rfu=[[1,0,0],[0,1,0],[0,0,1]])

    def test_refresh_starts_from_geometry_age_not_late_completion(self):
        d = self.data()
        self.lua.execute('function isLineOfSightClear(...) now=now+1; return true end')
        self.assertIsNone(self.check(d))
        self.lua.execute('function isLineOfSightClear(...) return true end')
        self.assertIs(self.check(d, 1080), True)
        self.check(d, 1190)
        self.assertEqual(self.lua.globals().state['probe']['probe_id'], 1190)

    def test_small_steering_change_across_old_quantization_boundary_keeps_fresh_cache(self):
        d = self.data()
        d['navigation']['heading_error_deg'] = 4.27/.7
        self.assertIs(self.check(d), True)
        self.lua.execute('function isLineOfSightClear(...) now=now+1; return true end')
        d['navigation']['heading_error_deg'] = 3.81/.7
        self.assertIs(self.check(d, 1200), True)
        self.assertEqual(self.lua.globals().state['probe']['status'], 'pending')
        self.assertEqual(self.lua.globals().state['probe']['cached_clear_tick_ms'], 1000)
        d['navigation']['heading_error_deg'] = -6
        self.assertIsNone(self.check(d, 1220))


class FlightDynamicsTests(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        cls = self.lua.execute(controller.controller_source())
        self.c = cls.new()
        self.lua.globals().autopilot = self.c
        self.lua.execute('''
            state, NULL, analog, digital, localPlayer, gear = {vehicle='plane',gearAttempts=0}, {}, {}, {}, {}, true
            ownedAnalog = {'accelerate','brake_reverse','vehicle_left','vehicle_right','steer_forward','steer_back'}
            ownedDigital = {'vehicle_look_left','vehicle_look_right','handbrake','sub_mission'}
            function number(v) if type(v)=='number' then return v end end
            function elapsed(t,b) return b and (t-b)%4294967296 or 0 end
            function read(name,...) return _G[name](...) end
            function emit() end
            function stopAutopilot(reason) error(reason) end
            function getVehicleLandingGearDown() return gear end
            function getAnalogControlState(name) return analog[name] or 0 end
            function setAnalogControlState(name,value,force)
                local opposite={vehicle_left='vehicle_right',vehicle_right='vehicle_left',steer_forward='steer_back',steer_back='steer_forward'}
                local steps=opposite[name] and 128 or 255
                analog[name]=math.floor(value*steps)/steps
                if opposite[name] then analog[opposite[name]]=0 end
                return true
            end
            function setPedControlState(ped,name,value)
                if name=='sub_mission' and value and not digital[name] then gear=not gear end
                digital[name]=value; return true
            end
        ''')
        source = (ROOT/'bin/Release/x86/PilotTelemetry.lua').read_text(encoding='utf-8')
        self.apply = self.lua.execute(source[source.index('applyAutopilot = function('):source.index('updateAutopilot = function(')]+'\nreturn applyAutopilot')

    def test_recorded_left_correction_reaches_axis_after_real_apply_function(self):
        case = FIXTURE['left_correction']
        self.assertLess(case['requested']['aileron'], -.6)
        self.assertEqual(case['observed_analog']['vehicle_left'], 0)
        self.c.enabled = True
        self.c.started = 1000
        self.c.output = self.lua.table_from(case['requested'])
        self.apply(1050)
        analog = self.lua.globals().analog
        self.assertGreater(analog['vehicle_left'], .6)
        self.assertEqual(analog['vehicle_right'], 0)
        self.assertGreater(analog['steer_forward'], .1)
        self.assertEqual(analog['steer_back'], 0)

    def test_two_recorded_rings_with_quantized_axes_and_uncertain_flight_response(self):
        scenarios = [(start,gain,lag) for start in ('first_airborne','left_correction')
            for gain,lag in ((.7,.3), (1,.2), (1.3,.15), (1,.6))]
        for start, gain, lag in scenarios:
            with self.subTest(start=start,gain=gain,lag=lag):
                self.setUp()
                d = copy.deepcopy(FIXTURE[start]['sample'])
                d.update(vehicle='plane', on_ground=False)
                now = 1000
                self.assertIs(self.c.start(self.c, self.lua.table_from(d, recursive=True), now), True)
                self.c.airborne = self.c.flightSeen = True
                targets = [FIXTURE['first_airborne']['sample']['navigation']['position'],
                    FIXTURE['second_target']['sample']['navigation']['position']]
                if start == 'left_correction':
                    targets = targets[1:]
                reached, closest = 0, [1e9,1e9]
                for _ in range(350):
                    target = targets[reached]
                    delta = [target[i]-d['position_m'][i] for i in range(3)]
                    distance, horizontal = math.sqrt(sum(x*x for x in delta)), math.hypot(*delta[:2])
                    closest[reached] = min(closest[reached], distance)
                    if distance < 28:
                        reached += 1
                        if reached == len(targets):
                            break
                        continue
                    bearing = math.degrees(math.atan2(delta[0],delta[1])) % 360
                    error = (bearing-d['heading_deg']+180) % 360-180
                    d['navigation'].update(id='ring'+str(reached), position=target, distance_2d_m=horizontal,
                        distance_3d_m=distance, heading_error_deg=error, track_error_deg=error,
                        bearing_deg=bearing, altitude_error_m=delta[2])
                    now += 50
                    out = self.c.update(self.c, self.lua.table_from(d, recursive=True), now, True)
                    if not self.c.enabled:
                        break
                    self.apply(now)
                    a = self.lua.globals().analog
                    roll_input, pitch_input = a['vehicle_right']-a['vehicle_left'], a['steer_back']-a['steer_forward']
                    d['roll_rate_dps'] += (47*gain*roll_input-d['roll_rate_dps'])*.05/lag
                    d['pitch_rate_dps'] += (23*gain*pitch_input-d['pitch_rate_dps'])*.05/lag
                    d['roll_deg'] += d['roll_rate_dps']*.05
                    d['pitch_deg'] += d['pitch_rate_dps']*.05
                    d['heading_rate_dps'] = .18*d['roll_deg']+9*gain*out['rudder']
                    d['heading_deg'] = (d['heading_deg']+d['heading_rate_dps']*.05) % 360
                    d['speed_kmh'] = max(140, min(255, d['speed_kmh']+(18*a['accelerate']-25*a['brake_reverse'])*.05))
                    gamma, h = math.radians(d['pitch_deg']+1.4), math.radians(d['heading_deg'])
                    speed = d['speed_kmh']/3.6
                    d['climb_mps'] = speed*math.sin(gamma)
                    d['horizontal_speed_kmh'] = d['speed_kmh']*math.cos(gamma)
                    d['position_m'] = [d['position_m'][0]+speed*math.cos(gamma)*math.sin(h)*.05,
                        d['position_m'][1]+speed*math.cos(gamma)*math.cos(h)*.05,
                        d['position_m'][2]+d['climb_mps']*.05]
                    d['agl_terrain_m'] = d['position_m'][2]-27.72
                self.assertEqual(reached, len(targets), (closest,self.c.status))


if __name__ == '__main__':
    unittest.main()
