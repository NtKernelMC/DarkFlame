"""Exercise JBK's real control helper when game input clears a held control."""
import unittest

from pilot_telemetry_test import ROOT, LuaRuntime


class ControlTests(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute('''
            localPlayer, actual, calls, fail = {}, {}, {}, false
            function setPedControlState(ped, name, pressed)
                assert(ped == localPlayer)
                calls[#calls+1] = {name, pressed}
                if fail then return false end
                actual[name] = pressed
                return true
            end
        ''')
        source = (ROOT / 'bin/Release/x86/JBKBot.lua').read_text(encoding='utf-8')
        self.set_control, self.release = self.lua.execute(
            source[source.index('local BOT_CONTROLS ='):source.index('local Settings =')]
            + '\nreturn setBotControl, releaseBotControls')

    def test_held_sprint_and_movement_are_reasserted_after_input_reset(self):
        for name in ('sprint', 'forwards', 'left', 'right'):
            self.assertTrue(self.set_control(name, True))
            self.lua.globals().actual[name] = False
            self.assertTrue(self.set_control(name, True))
            self.assertTrue(self.lua.globals().actual[name])

    def test_stop_releases_all_controls_and_does_not_reassert_them(self):
        self.set_control('sprint', True)
        self.set_control('forwards', True)
        self.release()
        count = len(self.lua.globals().calls)
        for name in ('sprint', 'forwards'):
            self.assertFalse(self.lua.globals().actual[name])
            self.assertTrue(self.set_control(name, False))
        self.assertEqual(len(self.lua.globals().calls), count)

    def test_failed_write_is_retried_and_unowned_controls_are_rejected(self):
        self.lua.globals().fail = True
        self.assertFalse(self.set_control('sprint', True))
        self.lua.globals().fail = False
        self.assertTrue(self.set_control('sprint', True))
        count = len(self.lua.globals().calls)
        self.assertFalse(self.set_control('enter_exit', True))
        self.assertEqual(len(self.lua.globals().calls), count)


if __name__ == '__main__':
    unittest.main()
