"""JBK's native game controls and PostMessage R pulse lifecycle."""
import unittest
from pilot_telemetry_test import ROOT,LuaRuntime

class ControlTests(unittest.TestCase):
    def setUp(self):
        self.lua=LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute('''
            localPlayer={}; actual,calls,keyCalls={},{},{}
            now,fail,menu,chat=0,false,false,false
            function getTickCount() return now end
            function isChatBoxInputActive() return chat end
            function setPedControlState(player,key,down)
                assert(player==localPlayer)
                calls[#calls+1]={key,down}
                if fail then return false end
                actual[key]=down; return true
            end
            api={menuOpen=function() return menu end,key=function(key,down)
                assert(key=='r'); keyCalls[#keyCalls+1]={key,down}
                if fail then return false end
                keyDown=down; return true
            end}
            function setTimer(fn,ms,repeats) tick=fn; assert(ms==50) end
        ''')
        source=(ROOT/'bin/Release/x86/JBKBot.lua').read_text(encoding='utf8')
        self.set_control,self.release,self.enabled=self.lua.execute(
            'local _STATE=false\n'+source[source.index('local BOT_CONTROLS ='):source.index('local Settings =')]
            +'\nreturn setBotControl,releaseBotControls,function(v) _STATE=v; if not v then releasePulseKey() end end')

    def test_native_controls_are_reasserted(self):
        for key in ('forwards','sprint','left','right','jump','fire'):
            self.assertTrue(self.set_control(key,True))
            self.lua.globals().actual[key]=False
            self.assertTrue(self.set_control(key,True))
            self.assertTrue(self.lua.globals().actual[key])
        self.assertEqual(len(self.lua.globals().keyCalls),0)

    def test_failed_release_is_retried(self):
        self.set_control('jump',True)
        self.lua.globals().fail=True
        self.release()
        self.assertTrue(self.lua.globals().actual['jump'])
        self.lua.globals().fail=False
        self.release()
        self.assertFalse(self.lua.globals().actual['jump'])

    def test_r_pulses_every_two_seconds_only_while_working(self):
        self.lua.execute('now=2000; tick(); assert(#keyCalls==0)')
        self.enabled(True)
        self.lua.execute('tick(); assert(keyDown); now=2100; tick(); assert(not keyDown)')
        self.lua.execute('now=3999; tick(); assert(#keyCalls==2); now=4000; tick(); assert(keyDown)')
        self.enabled(False)
        self.lua.execute('assert(not keyDown); now=8000; tick(); assert(#keyCalls==4)')

    def test_menu_blocks_r_and_failed_release_retries(self):
        self.enabled(True)
        self.lua.execute('now=2000; menu=true; tick(); assert(#keyCalls==0); menu=false; tick(); assert(keyDown)')
        self.lua.execute('fail=true; now=2100; tick(); assert(keyDown)')
        self.lua.execute('fail=false; now=2150; tick(); assert(not keyDown)')

    def test_chat_prevents_new_presses_and_releases_held_r(self):
        self.enabled(True)
        self.lua.execute('chat=true; now=2000; tick(); assert(#keyCalls==0)')
        self.lua.execute('chat=false; tick(); assert(keyDown); chat=true; now=2050; tick(); assert(not keyDown)')

if __name__=='__main__': unittest.main()
