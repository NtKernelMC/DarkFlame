"""Compile the real bridge dispatch against a hidden test window, never the game."""
import os
import subprocess
import unittest
from pathlib import Path

import pilot_telemetry_test as recorder

ROOT = recorder.ROOT


class InputCompatibilityTests(unittest.TestCase):
    def test_native_keyboard_and_mouse_backends(self):
        source = (ROOT/'Client/lua_bridge.cpp').read_text(encoding='utf8')
        begin = source.index('int __cdecl DirectEmulateKey(void* lua)\n{')
        end = source.index('int __cdecl DirectPlayAlertSignal(void* lua)\n{', begin)
        directory = ROOT/'.codex-temp-dia2dump'
        (directory/'pilot-input-dispatch.h').write_text(source[begin:end], encoding='utf8')
        result = subprocess.run([os.environ['COMSPEC'], '/c', str(ROOT/'Tests/run_pilot_input_test.cmd')],
            capture_output=True, text=True, errors='replace', timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_existing_jbk_timer_still_uses_original_mouse_api(self):
        source = (ROOT/'bin/Release/x86/JBKBot.lua').read_text(encoding='utf8')
        start = source.index('setTimer(function()\n    if not _STATE or not Settings.AntiAFK then')
        end = source.index('end, 2000, 0)', start)+len('end, 2000, 0)')
        lua = recorder.LuaRuntime(unpack_returned_tuples=True)
        lua.execute('''
            calls,timers={},{}
            _STATE=true; Settings={AntiAFK=true}
            function dfEmulateMouseButton(key,down) calls[#calls+1]={key,down}; return true end
            api={mouse=dfEmulateMouseButton}
            function setTimer(fn,ms,repeats) timers[#timers+1]={fn=fn,ms=ms,repeats=repeats} end
        '''+source[start:end])
        self.assertEqual(lua.eval('timers[1].ms,timers[1].repeats'), (2000,0))
        lua.execute('timers[1].fn(); timers[2].fn()')
        self.assertEqual(lua.eval('calls[1][1],calls[1][2],calls[2][1],calls[2][2],timers[2].ms'), ('right',True,'right',False,75))
        lua.execute('_STATE=false; timers[1].fn(); _STATE=true; Settings.AntiAFK=false; timers[1].fn()')
        self.assertEqual(lua.eval('#timers,calls[3][2],calls[4][2]'), (2,False,False))


if __name__ == '__main__':
    unittest.main()
