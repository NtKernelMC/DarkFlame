"""Ground yaw is a held key with predictive release, never a 180 ms pulse train."""
import math
import unittest

import pilot_controller_test as controller


class GroundHoldTests(unittest.TestCase):
    setUp, start, update = controller.MathTests.setUp, controller.MathTests.start, controller.MathTests.update

    def test_lagged_turn_holds_then_releases_without_repeated_left_right_reversal(self):
        for side in (-1,1):
            for gain,lag in ((5.5,.3),(8.8,.25),(12,.6)):
                for reverse in (False,True):
                    with self.subTest(side=side,gain=gain,lag=lag,reverse=reverse):
                        self.setUp()
                        sign=-1 if reverse else 1
                        d=controller.observation(speed_kmh=8,velocity_body_rfu_mps=[0,sign*8/3.6,0])
                        if reverse: self.c.notify(self.c,'Двигайтесь аккуратно назад',self.now)
                        target=side*35
                        d['navigation']['heading_error_deg']=(target+(180 if reverse else 0)+180)%360-180
                        self.start(d)
                        rate=0; outputs=[]; errors=[]
                        for _ in range(500):
                            heading_error=(target-d['heading_deg']+180)%360-180
                            d['navigation']['heading_error_deg']=(heading_error+(180 if reverse else 0)+180)%360-180
                            out=self.update(d)
                            self.assertTrue(self.c.enabled,self.c.status)
                            self.assertIn(out['rudder'],(-1,0,1))
                            outputs.append(out['rudder']); errors.append(heading_error)
                            rate+=(sign*gain*out['rudder']-rate)*(1-math.exp(-.05/lag))
                            d['heading_deg']=(d['heading_deg']+rate*.05)%360
                            d['heading_rate_dps']=rate
                        changes=sum(a!=b for a,b in zip(outputs,outputs[1:]))
                        self.assertLessEqual(changes,6,(changes,outputs))
                        self.assertTrue(all(x==outputs[0] for x in outputs[:20]))
                        self.assertTrue(all(x==0 for x in outputs[-100:]))
                        self.assertLess(abs(errors[-1]),2.5)
                        self.assertGreater(min(side*e for e in errors),-2.5)

    def test_predicted_stop_releases_before_reaching_target_and_ignores_small_noise(self):
        d=controller.observation()
        d['navigation']['heading_error_deg']=15
        self.start(d)
        self.assertEqual(self.update(d)['rudder'],1)
        d.update(heading_rate_dps=8.8)
        for _ in range(10): self.update(d)
        d['navigation']['heading_error_deg']=3
        self.assertEqual(self.update(d)['rudder'],0)
        d.update(heading_rate_dps=0)
        for error in ([.2,-.3,.5,-.7,1,-1]*10):
            d['navigation']['heading_error_deg']=error
            self.assertEqual(self.update(d)['rudder'],0)


class GroundHoldAdapterTests(unittest.TestCase):
    setUp = controller.AdapterTests.setUp
    arm = controller.AdapterTests.arm
    run_lua,records=controller.AdapterTests.run_lua,controller.AdapterTests.records

    def test_each_game_frame_keeps_same_key_down_across_pwm_boundaries(self):
        self.run_lua('m1.position={20,150,6.2}; plane.velocity={0,.03,0}')
        self.arm()
        self.run_lua('''
            for i=1,150 do
                frame(10)
                assert(pressed.vehicle_look_right==true)
                assert(pressed.vehicle_look_left==false)
            end
        ''')
        applied=self.records('sample')[-1]['data']['autopilot']['applied']
        self.assertEqual(applied['rudder_control'],'hold')
        self.assertEqual(applied['rudder_pwm_period_ms'],0)
        self.run_lua('m1.position={0,150,6.2}; step(10)')
        self.assertEqual(self.lua.eval('pressed.vehicle_look_left,pressed.vehicle_look_right'),(False,False))


if __name__=='__main__':
    unittest.main()
