"""Recorded missed turn, coupled body-axis response and frozen parking release."""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller
import pilot_flight_test as flight

FIXTURE = json.loads(Path(__file__).with_name('pilot_turn_fixture.json').read_text(encoding='utf-8'))


class GuidanceTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def test_recorded_turn_banks_early_instead_of_waiting_for_close_range(self):
        d = copy.deepcopy(FIXTURE['cases']['entry']['sample'])
        self.start(d)
        out = self.update(d)
        self.assertLess(self.c.detail['goal_roll_deg'], -23)
        self.assertGreater(abs(self.c.detail['goal_roll_deg']), 2.5*abs(FIXTURE['cases']['entry']['decision']['goal_roll_deg']))
        self.assertLess(out['aileron'], 0)
        self.assertLess(self.c.detail['goal_turn_rate_dps'], -3)
        self.assertGreater(self.c.detail['predicted_miss_m'], 140)
        self.assertEqual(self.c.detail['guidance'], 'curvature_intercept')

    def test_same_heading_error_needs_more_bank_at_higher_speed_or_shorter_range(self):
        banks = []
        for speed, distance in ((200,600),(255,600),(255,300)):
            self.setUp()
            d = controller.observation(on_ground=False, agl_terrain_m=400, speed_kmh=speed, horizontal_speed_kmh=speed)
            d['navigation'].update(marker_type='ring', heading_error_deg=15, track_error_deg=15, distance_2d_m=distance, distance_3d_m=distance)
            self.start(d)
            self.update(d)
            banks.append(self.c.detail['goal_roll_deg'])
        self.assertLess(banks[0], banks[1])
        self.assertLess(banks[1], banks[2])

    def test_both_turn_directions_use_pull_with_actual_bank(self):
        for direction in (-1,1):
            self.setUp()
            d = controller.observation(on_ground=False, agl_terrain_m=400, speed_kmh=250,
                horizontal_speed_kmh=250, pitch_deg=-1.4, roll_deg=direction*35)
            d['navigation'].update(marker_type='ring', heading_error_deg=direction*25, track_error_deg=direction*25,
                distance_2d_m=400, distance_3d_m=400)
            self.start(d)
            out = self.update(d)
            self.assertGreater(self.c.detail['turn_pull_dps'], 1)
            self.assertGreater(out['elevator'], 0)
            self.assertGreater(out['rudder']*direction, 0)
            self.assertFalse(self.c.detail['bank_recovery'])

    def test_bank_and_pull_limits_near_ground_landing_and_recovery(self):
        for agl, landing, roll, limit in ((4,False,0,12),(25,False,0,28),(100,True,0,25),(200,False,50,50),(200,False,67,50)):
            self.setUp()
            d = controller.observation(on_ground=False, agl_terrain_m=agl, speed_kmh=250, horizontal_speed_kmh=250, roll_deg=roll)
            d['navigation'].update(marker_type='ring', heading_error_deg=60, track_error_deg=60)
            self.start(d)
            self.c.airborne, self.c.flightSeen, self.c.landing = True, True, landing
            out = self.update(d)
            self.assertLessEqual(abs(self.c.detail['goal_roll_deg']), limit)
            self.assertLessEqual(abs(out['elevator']), .7)
            self.assertLessEqual(abs(out['rudder']), .35)
            self.assertEqual(self.c.detail['bank_recovery'], roll>65)
            if roll>65:
                self.assertEqual(self.c.detail['goal_roll_deg'], 0)
                self.assertEqual(out['rudder'], 0)

    def test_unchanged_target_remains_stable_across_north_heading_wrap(self):
        results=[]
        for error, track in ((179,-179),(-179,179)):
            self.setUp()
            d=controller.observation(on_ground=False, agl_terrain_m=400, speed_kmh=250,horizontal_speed_kmh=250)
            d['navigation'].update(marker_type='ring',heading_error_deg=error,track_error_deg=track)
            self.start(d)
            self.update(d)
            results.append(self.c.detail['goal_roll_deg'])
        self.assertGreater(abs(results[0]),40)
        self.assertAlmostEqual(results[0],-results[1])


class FrozenParkingTests(unittest.TestCase):
    setUp = controller.AdapterTests.setUp
    run_lua, arm, records = controller.AdapterTests.run_lua, controller.AdapterTests.arm, controller.AdapterTests.records
    assert_released = controller.AdapterTests.assert_released

    def test_frozen_job_releases_controls_once_and_resumes_without_stopping_recording(self):
        self.arm()
        self.run_lua("frozen=true; function isElementFrozen() return frozen end; event('province:sendNotification',pilotRoot,'Ожидайте, пока все пассажиры займут свои места в салоне'); step(3)")
        self.assert_released()
        calls = self.lua.eval('#actuatorCalls')
        self.run_lua('step(30)')
        self.assertEqual(calls,self.lua.eval('#actuatorCalls'))
        self.assertEqual(self.lua.globals().ui['autopilot'],'1')
        self.assertEqual(self.lua.globals().ui['recording'],'1')
        self.assertEqual(self.records('autopilot_stop'),[])
        suspended = self.records('autopilot_controls_suspended')
        self.assertEqual(len(suspended),1)
        self.assertTrue(suspended[0]['data']['suspended'])
        self.run_lua("frozen=false; m1.position={0,-150,6.2}; event('province:sendNotification',pilotRoot,'Двигайтесь аккуратно назад для начала маршрута'); step(12)")
        self.assertGreater(self.lua.eval('analog.brake_reverse'),0)
        self.assertFalse(self.records('autopilot_controls_suspended')[-1]['data']['suspended'])


class RecordedTurnTests(unittest.TestCase):
    setUp = flight.FlightDynamicsTests.setUp

    def test_recorded_turn_and_preceding_rings_with_coupled_pitch_yaw(self):
        scenarios=[(start,side,gain,lag,passive) for start in ('entry','chain_entry') for side in (-1,1)
            for gain,lag,passive in ((.7,.3,.08),(1,.2,.12),(1.3,.15,.18),(1,.6,.12))]
        for start, side, gain, lag, passive in scenarios:
            with self.subTest(start=start,side=side,gain=gain,lag=lag,passive=passive):
                self.setUp()
                d=copy.deepcopy(FIXTURE['cases'][start]['sample'])
                targets=copy.deepcopy(FIXTURE['targets'] if start=='chain_entry' else FIXTURE['targets'][-1:])
                d['vehicle']='plane'
                d['position_m'][0]*=side
                for target in targets: target[0]*=side
                d['heading_deg']=(d['heading_deg']*side)%360
                for key in ('roll_deg','heading_rate_dps','roll_rate_dps'): d[key]*=side
                self.assertIs(self.c.start(self.c,self.lua.table_from(d,recursive=True),1000),True)
                now,reached,closest,peak_roll=1000,0,1e9,0
                bank,pitch=math.radians(d['roll_deg']),math.radians(d['pitch_deg'])
                q=d['pitch_rate_dps']*math.cos(bank)+d['heading_rate_dps']*math.cos(pitch)*math.sin(bank)
                r=d['heading_rate_dps']*math.cos(pitch)*math.cos(bank)-d['pitch_rate_dps']*math.sin(bank)
                p=d['roll_rate_dps']
                speed=d['speed_kmh']/3.6
                terrain=d['position_m'][2]-d['agl_terrain_m']
                for _ in range(850):
                    target=targets[reached]
                    delta=[target[i]-d['position_m'][i] for i in range(3)]
                    horizontal=math.hypot(*delta[:2]); distance=math.sqrt(sum(v*v for v in delta))
                    closest=min(closest,distance)
                    if distance<28:
                        reached+=1
                        if reached==len(targets):break
                        closest=1e9
                        continue
                    bearing=math.degrees(math.atan2(delta[0],delta[1]))%360
                    error=(bearing-d['heading_deg']+180)%360-180
                    d['navigation'].update(id='ring'+str(reached),position=target,distance_2d_m=horizontal,
                        distance_3d_m=distance,altitude_error_m=delta[2],heading_error_deg=error,
                        track_error_deg=error,bearing_deg=bearing)
                    now+=50
                    out=self.c.update(self.c,self.lua.table_from(d,recursive=True),now,True)
                    if not self.c.enabled:break
                    self.apply(now)
                    a=self.lua.globals().analog
                    roll_input,pitch_input=a['vehicle_right']-a['vehicle_left'],a['steer_back']-a['steer_forward']
                    p+=(47*gain*roll_input-p)*.05/lag
                    q+=((30.5 if pitch_input>=0 else 21)*gain*pitch_input-q)*.05/lag
                    r+=(12.4*gain*out['rudder']-r)*.05/lag
                    bank,pitch=math.radians(d['roll_deg']),math.radians(d['pitch_deg'])
                    d['roll_rate_dps']=p
                    d['pitch_rate_dps']=q*math.cos(bank)-r*math.sin(bank)
                    d['heading_rate_dps']=(q*math.sin(bank)+r*math.cos(bank))/max(.5,math.cos(pitch))+passive*d['roll_deg']
                    d['roll_deg']+=d['roll_rate_dps']*.05
                    d['pitch_deg']+=d['pitch_rate_dps']*.05
                    d['heading_deg']=(d['heading_deg']+d['heading_rate_dps']*.05)%360
                    gamma,h=math.radians(d['pitch_deg']+1.4),math.radians(d['heading_deg'])
                    d['climb_mps']=speed*math.sin(gamma)
                    d['horizontal_speed_kmh']=d['speed_kmh']*math.cos(gamma)
                    d['position_m']=[d['position_m'][0]+speed*math.cos(gamma)*math.sin(h)*.05,
                        d['position_m'][1]+speed*math.cos(gamma)*math.cos(h)*.05,
                        d['position_m'][2]+d['climb_mps']*.05]
                    d['agl_terrain_m']=d['position_m'][2]-terrain
                    peak_roll=max(peak_roll,abs(d['roll_deg']))
                self.assertEqual(reached,len(targets),(reached,closest,peak_roll,self.c.status))
                self.assertLess(peak_roll,65)


if __name__=='__main__':
    unittest.main()
