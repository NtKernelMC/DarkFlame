"""Recorded float/ground-marker regression and approximate touchdown response.

The vertical plant is deliberately limited: pitch-rate response, speed-dependent
flight-path offset and braking, varied across gain/lag cases. It is not GTA physics.
"""
import copy
import json
import math
import unittest
from pathlib import Path

import pilot_controller_test as controller

FIXTURE = json.loads(Path(__file__).with_name('pilot_landing_fixture.json').read_text(encoding='utf8'))


class LandingTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def near(self, leg, agl):
        return copy.deepcopy(min((r for r in FIXTURE[leg] if not r['data']['on_ground']),
            key=lambda r: abs(r['data']['agl_terrain_m']-agl))['data'])

    def test_recorded_low_float_commands_descent_instead_of_constant_positive_pitch(self):
        for leg in ('return', 'outbound'):
            for agl in (4,3,2):
                with self.subTest(leg=leg,agl=agl):
                    self.setUp()
                    d = self.near(leg, agl)
                    self.start(d); self.c.landing = True; self.c.airborne = True; self.c.flightSeen = True
                    out = self.update(d)
                    self.assertEqual(self.c.detail['landing_stage'], 'touchdown')
                    self.assertLessEqual(self.c.detail['goal_climb_mps'], -.9)
                    self.assertLess(self.c.detail['goal_pitch_deg'], 3)
                    self.assertEqual(out['throttle'], 0)
                    self.assertTrue(out['gear_down'])

    def test_ground_checkpoints_do_not_redirect_aircraft_into_taxi_turn(self):
        rows = [r for r in FIXTURE['return'] if r['data']['navigation']['marker_type']=='checkpoint']
        self.start(rows[0]['data']); self.c.landing = True; self.c.flightSeen = True
        heading, ids, previous = None, set(), rows[0]['elapsed_ms']
        for row in rows:
            d = row['data']
            out = self.update(d, row['elapsed_ms']-previous or 50)
            previous = row['elapsed_ms']
            self.assertTrue(self.c.enabled, self.c.status)
            ids.add(tuple(d['navigation']['position']))
            if heading is None: heading = self.c.landingHeading
            self.assertEqual(self.c.landingHeading, heading)
            self.assertEqual(out['throttle'], 0)
            if not d['on_ground']:
                self.assertLessEqual(abs(self.c.detail['goal_roll_deg']), 5)
                self.assertEqual(self.c.detail['landing_stage'], 'touchdown')
            elif d['speed_kmh'] > 35:
                self.assertEqual(self.c.phase, 'landing_rollout')
                self.assertEqual(out['brake'], 1)
                self.assertLessEqual(abs(out['rudder']), .3)
        self.assertGreaterEqual(len(ids), 3)

    def test_first_contact_brakes_immediately_bounce_keeps_final_then_taxi_resumes(self):
        d = self.near('return', 4)
        self.start(d); self.c.landing = True; self.c.flightSeen = True
        self.update(d)
        heading = self.c.landingHeading
        d.update(on_ground=True, agl_terrain_m=1.2, speed_kmh=120, horizontal_speed_kmh=120)
        self.assertEqual(self.update(d)['brake'], 1)
        self.assertTrue(self.c.airborne)  # Contact debounce has not expired.
        d.update(on_ground=False, agl_terrain_m=1.8)
        self.assertTrue(self.update(d)['gear_down'])
        self.assertEqual(self.c.landingHeading, heading)
        self.assertEqual(self.c.detail['landing_stage'], 'touchdown')
        d.update(on_ground=True)
        for _ in range(7): self.update(d)
        d.update(speed_kmh=30, horizontal_speed_kmh=30)
        self.update(d)
        self.assertFalse(self.c.finalLanding)
        self.assertEqual(self.c.phase, 'rollout')

    def test_new_takeoff_clears_landing_lock_and_ordinary_high_ring_can_bank(self):
        d = self.near('return', 4)
        self.start(d); self.c.landing = True; self.c.flightSeen = True
        self.update(d)
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.assertFalse(self.c.finalLanding)
        self.assertIsNone(self.c.landingHeading)
        d.update(agl_terrain_m=200, speed_kmh=235, horizontal_speed_kmh=235)
        d['navigation'].update(marker_type='ring', heading_error_deg=50, track_error_deg=50, distance_2d_m=300)
        self.update(d)
        self.assertGreater(self.c.detail['goal_roll_deg'], 25)

    def test_approximate_descent_lands_before_first_ground_marker_and_brakes_before_turn(self):
        initial = next(r['data'] for r in FIXTURE['return'] if r['data']['navigation']['marker_type']=='checkpoint')
        results = []
        for gain, lag in ((1,.25),(.75,.35),(1.25,.15)):
            for mirrored in (False,True):
                with self.subTest(gain=gain,lag=lag,mirrored=mirrored):
                    self.setUp()
                    d = copy.deepcopy(initial)
                    if mirrored:
                        d['heading_deg'] = (-d['heading_deg']) % 360
                        d['track_deg'] = (-d['track_deg']) % 360
                        for key in ('heading_error_deg','track_error_deg','bearing_deg'):
                            d['navigation'][key] = -d['navigation'][key]
                    ground_z = d['position_m'][2]-d['agl_terrain_m']
                    origin = d['position_m'][:]
                    theta = math.radians(d['heading_deg'])
                    q, vz, travel, contact = d['pitch_rate_dps'], d['climb_mps'], 0, None
                    self.start(d); self.c.landing = True; self.c.flightSeen = True
                    for i in range(600):
                        out = self.update(d)
                        self.assertTrue(self.c.enabled, self.c.status)
                        speed = d['speed_kmh']
                        if not d['on_ground']:
                            speed += (-.2*(speed-127)-16*out['brake']+8*out['throttle'])*.05
                            elevator = round(out['elevator']*128)/128
                            target_q = (30.5 if elevator>=0 else 21)*elevator*gain-1.8
                            q += (target_q-q)*(1-math.exp(-.05/lag))
                            d['pitch_deg'] += q*.05
                            # Offset ~0.6 deg at 145 km/h and ~2.6 deg at 127,
                            # from pitch minus atan2(climb,horizontal speed) in this log.
                            offset = max(.6,2.6-(speed-127)*.11)
                            wanted_vz = speed/3.6*math.sin(math.radians(d['pitch_deg']-offset))
                            vz += (wanted_vz-vz)*(1-math.exp(-.05/.2))
                            d['position_m'][2] += vz*.05
                            if d['position_m'][2]-ground_z <= 1.2:
                                contact = (travel, vz, (i+1)*.05)
                                d['on_ground'] = True
                                d['position_m'][2], vz = ground_z+1.2, 0
                        else:
                            self.assertEqual(out['brake'], 1)
                            speed = max(0,speed-25*out['brake']*.05)
                        travel += speed/3.6*.05
                        d['position_m'][0] = origin[0]+travel*math.sin(theta)
                        d['position_m'][1] = origin[1]+travel*math.cos(theta)
                        d.update(speed_kmh=speed,horizontal_speed_kmh=speed,climb_mps=vz,pitch_rate_dps=q,
                            agl_terrain_m=d['position_m'][2]-ground_z,velocity_body_rfu_mps=[0,speed/3.6,vz])
                        nav = d['navigation']
                        nav['distance_2d_m'] = max(1,initial['navigation']['distance_2d_m']-travel)
                        nav['distance_3d_m'] = math.hypot(nav['distance_2d_m'],nav['position'][2]-d['position_m'][2])
                        nav['altitude_error_m'] = nav['position'][2]-d['position_m'][2]
                        if travel > 155:
                            nav.update(id='next_taxi',heading_error_deg=-60 if mirrored else 60)
                        if d['on_ground'] and speed<35: break
                    self.assertIsNotNone(contact)
                    self.assertLess(contact[0],155,contact)
                    self.assertGreater(contact[1],-2.6,contact)
                    self.assertLess(contact[1],0,contact)
                    self.assertLess(travel,210)
                    results.append((round(contact[0],1),round(contact[1],2),round(travel,1)))
        self.assertEqual(len(results),6)


class TaxiSpeedTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def test_straight_speed_and_marker_boundary_braking(self):
        d = controller.observation(speed_kmh=35, horizontal_speed_kmh=35, velocity_body_rfu_mps=[0,35/3.6,0])
        self.start(d)
        d['navigation']['distance_2d_m'] = 300
        self.update(d)
        self.assertEqual(self.c.detail['goal_speed_kmh'],35)
        limits = []
        for distance in (65,55,45,38,30):
            d['navigation'].update(distance_2d_m=distance,distance_3d_m=distance)
            out = self.update(d)
            limits.append(self.c.detail['goal_speed_kmh'])
            if distance<50: self.assertGreater(out['brake'],0)
        self.assertEqual(limits,sorted(limits,reverse=True))
        self.assertEqual(limits[-1],18)

    def test_turns_and_reverse_retain_reduced_speed(self):
        for angle,limit in ((8,18),(20,12),(40,7),(70,4)):
            self.setUp(); d=controller.observation()
            d['navigation']['heading_error_deg']=angle
            self.start(d); self.update(d)
            self.assertLessEqual(self.c.detail['goal_speed_kmh'],limit)
        self.c.notify(self.c,'Двигайтесь аккуратно назад',self.now)
        d['navigation']['heading_error_deg']=180
        intent=self.c.groundIntent(self.c,self.lua.table_from(d,recursive=True),self.now)
        self.assertEqual(intent['speed'],8)


if __name__ == '__main__':
    unittest.main()
