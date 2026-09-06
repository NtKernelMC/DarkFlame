"""Compile the real bridge dispatch against a hidden test window, never the game."""
import os
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class InputCompatibilityTests(unittest.TestCase):
    def test_native_keyboard_and_mouse_backends(self):
        source = (ROOT/'Client/lua_bridge.cpp').read_text(encoding='utf8')
        begin = source.index('int __cdecl DirectEmulateKey(void* lua)\n{')
        end = source.index('int __cdecl DirectPlayAlertSignal(void* lua)\n{', begin)
        directory = ROOT/'.codex-temp-dia2dump'
        directory.mkdir(exist_ok=True)
        (directory/'pilot-input-dispatch.h').write_text(source[begin:end], encoding='utf8')
        result = subprocess.run([os.environ['COMSPEC'], '/c', str(ROOT/'Tests/run_pilot_input_test.cmd')],
            capture_output=True, text=True, errors='replace', timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

if __name__ == '__main__':
    unittest.main()
