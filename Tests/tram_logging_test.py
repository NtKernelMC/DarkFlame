"""Disk diagnostics must work while chat debug is disabled."""
import unittest
from pilot_telemetry_test import ROOT,LuaRuntime

class TramLoggingTests(unittest.TestCase):
    def test_disk_logging_independent_of_chat_and_rate_limited(self):
        lua=LuaRuntime(unpack_returned_tuples=True)
        source=(ROOT/'bin/Release/x86/TramBot.lua').read_text(encoding='utf8')
        lua.globals().script=source
        lua.execute('assert(loadstring(script))')
        helpers=source[source.index('local function debugOutput'):source.index('local function pointText')]
        lua.execute('''
            local debugEnabled=false
            local debugMemory,debugTicks={},{}
            local now=1000
            local logs,chats={},{}
            local api={log=function(s) logs[#logs+1]=s end}
            local function getTickCount() return now end
            local function outputChatBox(s) chats[#chats+1]=s end
        '''+helpers+'''
            debugChange('state','moving','start')
            debugChange('state','moving','duplicate')
            debugRate('sample',1000,'first')
            debugRate('sample',1000,'duplicate')
            assert(#logs==2 and #chats==0)
            now=2000; debugRate('sample',1000,'second'); assert(#logs==3)
            debugEnabled=true; debugOutput('chat and file'); assert(#logs==4 and #chats==1)
        ''')

if __name__=='__main__': unittest.main()
