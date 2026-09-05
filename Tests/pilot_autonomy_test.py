"""Repeat-job lifecycle and input pulses through the actual single Lua script."""
import unittest

import pilot_controller_test as controller
import pilot_liberty_test as liberty


class AutonomyTests(unittest.TestCase):
    setUp = controller.AdapterTests.setUp
    run_lua, records = controller.AdapterTests.run_lua, controller.AdapterTests.records
    assert_released = controller.AdapterTests.assert_released

    def arm(self, telemetry=True):
        self.run_lua("commands[#commands+1]='autopilot_autonomy:1'")
        controller.AdapterTests.arm(self, telemetry)

    def complete(self):
        self.run_lua("event('province:sendNotification',pilotRoot,'Вы выполнили рейс! Заработано: 18808 р.')")

    def test_defaults_enable_ap_telemetry_but_do_not_record_before_a_flight(self):
        self.run_lua('step(12)')
        self.assertEqual(self.lua.eval("ui.autopilot_autonomy,ui.autopilot_hud,ui.autopilot_telemetry"), ('0','0','1'))
        self.complete()
        self.run_lua('occupied=nil; step(60)')
        self.assertEqual(self.lua.eval('#serverEvents'), 0)
        self.assertEqual(self.lua.eval('ui.recording'), '0')

    def test_two_full_cycles_one_request_per_completion_and_defaults_preserved(self):
        self.arm(False)
        for _ in range(2):
            self.complete()
            self.assert_released()
            before = self.lua.eval('#serverEvents')
            self.run_lua('step(30)')  # Still in old aircraft: do not send the event.
            self.assertEqual(self.lua.eval('#serverEvents'), before)
            self.run_lua('occupied=nil; step(8)')
            self.assertEqual(self.lua.eval('#serverEvents'), before+1)
            self.assertTrue(self.lua.eval("serverEvents[#serverEvents].name=='pilot:onJobAccepted' and serverEvents[#serverEvents].source==pilotRoot"))
            self.complete()  # Duplicate server notification after request.
            self.run_lua('step(8)')
            self.assertEqual(self.lua.eval('#serverEvents'), before+1)
            self.run_lua("occupied=plane; event('province:sendNotification',pilotRoot,'Ваш пункт назначения - Либерти Сити'); step(20)")
            self.assertEqual(self.lua.eval('ui.autopilot,ui.autopilot_waiting,ui.recording,ui.destination'), ('1','0','0','Либерти Сити'))
        self.assertEqual(self.lua.eval('#soundRequests'), 5)

    def test_completion_after_ejection_is_accepted_only_during_short_grace(self):
        for delay, requests in ((100,1),(5500,0)):
            with self.subTest(delay=delay):
                self.setUp()
                self.arm(False)
                self.run_lua('occupied=nil; step(%d)' % (delay//50))
                self.complete()
                self.run_lua('step(30)')
                self.assertEqual(self.lua.eval('#serverEvents'), requests)

    def test_stop_button_and_checkbox_cancel_pending_cycle(self):
        for command in ('autopilot_stop', 'autopilot_autonomy:0'):
            for stage in ('wait_exit','wait_plane'):
                with self.subTest(command=command,stage=stage):
                    self.setUp(); self.arm(False); self.complete()
                    if stage == 'wait_plane':
                        self.run_lua('occupied=nil; step(30)')
                    before = self.lua.eval('#serverEvents')
                    self.run_lua("commands[#commands+1]='%s'; step(6); occupied=nil; step(30); occupied=plane; step(30)" % command)
                    self.assertEqual(self.lua.eval('#serverEvents'), before)
                    self.assertEqual(self.lua.eval('ui.autopilot,ui.autopilot_waiting'), ('0','0'))

    def test_disabled_autonomy_completes_once_and_changing_setting_does_not_stop_flight(self):
        self.arm(False)
        self.run_lua("commands[#commands+1]='autopilot_autonomy:0'; step(6)")
        self.assertEqual(self.lua.eval('ui.autopilot'), '1')
        self.complete()
        self.run_lua('occupied=nil; step(30); occupied=plane; step(30)')
        self.assertEqual(self.lua.eval('#serverEvents,ui.autopilot'), (0,'0'))

    def test_failure_manual_stop_and_untrusted_completion_never_rehire(self):
        for trigger in ("event('onClientKey',root,'q',true)",
                        "event('province:sendNotification',pilotRoot,'Вы уволены')",
                        "commands[#commands+1]='autopilot_stop'; step(6)",
                        "frame(500)"):
            with self.subTest(trigger=trigger):
                self.setUp(); self.arm(False)
                self.run_lua(trigger)
                self.complete()
                self.run_lua('occupied=nil; step(30)')
                self.assertEqual(self.lua.eval('#serverEvents'), 0)
        self.setUp(); self.arm(False)
        self.run_lua("event('onClientChatMessage',root,'Вы выполнили рейс!',255,255,255,0); step(30)")
        self.assertEqual(self.lua.eval('#serverEvents,ui.autopilot'), (0,'1'))

    def test_no_server_response_times_out_without_event_spam(self):
        self.arm(False); self.complete()
        self.run_lua('occupied=nil; step(1300)')
        self.assertEqual(self.lua.eval('#serverEvents,ui.autopilot_waiting'), (1,'0'))
        self.assertIn('истекло', self.lua.eval('ui.autopilot_status'))

    def test_repeat_job_continues_while_minimized_without_recording(self):
        liberty.BackgroundTests.setUp(self)
        self.arm(False)
        self.run_lua("event('onClientMinimize',root)")
        self.complete()
        self.run_lua('occupied=nil; for i=1,30 do timerTick(50) end')
        self.assertEqual(self.lua.eval('#serverEvents'), 1)
        self.run_lua('occupied=plane; for i=1,30 do timerTick(50) end')
        self.assertEqual(self.lua.eval('ui.autopilot,ui.recording'), ('1','0'))

    def test_wrong_seat_and_wrong_vehicle_cannot_restart(self):
        self.arm(False); self.complete()
        self.run_lua('occupied=nil; step(30); occupied=plane; function getPedOccupiedVehicleSeat() return 1 end; step(30)')
        self.assertEqual(self.lua.eval('ui.autopilot'), '0')
        self.run_lua('function getPedOccupiedVehicleSeat() return 0 end; function getElementModel() return 577 end; step(30)')
        self.assertEqual(self.lua.eval('ui.autopilot'), '0')
        self.run_lua('function getElementModel() return 519 end; step(30)')
        self.assertEqual(self.lua.eval('ui.autopilot'), '1')

    def test_log_checkbox_restarts_owned_recording_only_for_new_flight(self):
        self.arm(); self.complete()
        self.assertEqual(self.lua.eval('ui.recording'), '0')
        self.run_lua('occupied=nil; step(30); occupied=plane; step(20)')
        self.assertEqual(self.lua.eval('ui.recording'), '1')
        self.assertEqual(len(self.records('recording_start')), 2)
        self.assertEqual(len(self.records('autonomy_restart')), 1)


class RmbTests(unittest.TestCase):
    run_lua, records = controller.AdapterTests.run_lua, controller.AdapterTests.records
    arm = controller.AdapterTests.arm

    def setUp(self):
        liberty.BackgroundTests.setUp(self)
        self.run_lua('''
            rmbCalls, mouseDown, menu, failRelease = {},false,false,false
            function dfMenuOpen() return menu end
            function dfEmulateKey(key,down)
                assert(key=='rmb')
                rmbCalls[#rmbCalls+1]={at=now,down=down}
                if not down and failRelease then return false end
                mouseDown=down
                event('onClientKey',root,'mouse2',down)
                return true
            end
            function getBoundKeys() return {mouse2='down',w='down'} end
        ''')

    def test_period_and_release_width_and_no_self_manual_takeover(self):
        self.arm()
        self.run_lua('step(130)')
        calls = list(self.lua.globals().rmbCalls.values())
        presses = [c['at'] for c in calls if c['down']]
        self.assertEqual(len(presses), 3)
        self.assertEqual([b-a for a,b in zip(presses,presses[1:])], [2000,2000])
        for a,b in zip(calls[::2],calls[1::2]):
            self.assertFalse(b['down'])
            self.assertEqual(b['at']-a['at'], 100)
        self.assertEqual(self.lua.eval('ui.autopilot'), '1')
        self.assertEqual(len(self.records('autopilot_rmb')), 6)

    def test_only_recording_menu_and_physical_rmb_do_not_generate_pulses(self):
        self.run_lua("commands[#commands+1]='start'; step(60)")
        self.assertEqual(self.lua.eval('#rmbCalls'), 0)
        self.arm(False)
        self.run_lua('menu=true; step(60); menu=false; physicalKeys.mouse2=true; step(60)')
        self.assertEqual(self.lua.eval('#rmbCalls'), 0)
        self.run_lua('physicalKeys.mouse2=false; step(3)')
        self.assertEqual(self.lua.eval('#rmbCalls'), 2)

    def test_stop_completion_menu_and_unload_release_held_pulse(self):
        for action in ("commands[#commands+1]='autopilot_stop'; step(6)",
                       "event('province:sendNotification',pilotRoot,'Вы выполнили рейс!')",
                       "menu=true; frame(10)", "__DarkFlamePilotCleanup()"):
            with self.subTest(action=action):
                self.setUp(); self.arm(False)
                self.run_lua('while not mouseDown do frame(10) end')
                self.run_lua(action)
                self.assertFalse(self.lua.eval('mouseDown'))

    def test_background_and_frozen_plane_still_receive_pulses(self):
        self.arm(False)
        self.run_lua("function isElementFrozen() return true end; event('onClientMinimize',root); for i=1,100 do timerTick(50) end")
        self.assertEqual(self.lua.eval('ui.autopilot'), '1')
        self.assertGreaterEqual(self.lua.eval('#rmbCalls'), 4)

    def test_failed_release_is_rate_limited_and_retried_after_stop(self):
        self.arm(False)
        self.run_lua('while not mouseDown do frame(10) end; failRelease=true')
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(200,5)")
        self.assertLess(self.lua.eval('#rmbCalls'), 8)
        self.run_lua('failRelease=false; step(10)')
        self.assertFalse(self.lua.eval('mouseDown'))


if __name__ == '__main__':
    unittest.main()
