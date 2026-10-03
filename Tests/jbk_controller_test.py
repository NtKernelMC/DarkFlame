"""Run the entire production JBK Lua chunk, its timers and native menu commands."""
import unittest
from pilot_telemetry_test import ROOT,LuaRuntime

BOT=(ROOT/'bin/Release/x86/JBKBot.lua').read_text(encoding='utf-8')

MOCK=r'''
now=10000
localPlayer={kind='player',valid=true,pos={50,-240,391.2578125},interior=0,dimension=0}
function getTickCount() return now end
function isElement(e) return type(e)=='table' and e.valid==true end
function getElementType(e) return e.kind end
function isPedOnGround() return true end
function Vector3(x,y,z) return {x=x,y=y,z=z} end
timers,handlers,commands,logs,ui,controls,keys={},{},{},{},{},{},{}
root={valid=true,kind='root'}; resourceRoot=root
job={valid=true,kind='marker',pos={14.654296875,-227.111328125,390.20001220703},color={0,155,0,170},interior=0,dimension=0}
elements={job}; camera,alerts=270,0; cameraWrites={}
function getRealTime() return {timestamp=now/1000} end
function getElementPosition(e)
    if e.pos then return unpack(e.pos) end
    return (e[1]+e[2])/2,(e[3]+e[4])/2,(e[5]+e[6])/2
end
function getElementVelocity(e) return unpack(e.velocity or {0,0,0}) end
function getElementInterior(e) return e.interior or 0 end
function getElementDimension(e) return e.dimension or 0 end
function getElementsByType(kind) local r={}; for _,e in ipairs(elements) do if e.valid and e.kind==kind then r[#r+1]=e end end; return r end
function getMarkerColor(e) return unpack(e.color) end
function setMarkerColor(e,...) e.color={...}; return true end
function getDistanceBetweenPoints3D(x,y,z,a,b,c)
    if type(x)=='table' then x,y,z,a,b,c=x.x,x.y,x.z,y.x,y.y,y.z end
    return ((x-a)^2+(y-b)^2+(z-c)^2)^0.5
end
function getPedCameraRotation() return -camera end
function setPedCameraRotation(_,angle) camera=angle; cameraWrites[#cameraWrites+1]=angle; return true end
function getPedControlState(_,key) return controls[key] or false end
function setPedControlState(_,key,down) controls[key]=down; return true end
function isPedDead() return false end
function getPedOccupiedVehicle() return false end
function isChatBoxInputActive() return chat==true end
function dfMenuOpen() return menu==true end
function dfPlayAlertSignal() alerts=alerts+1; return true end
function dfSetAlertMonitorEnabled() return true end
function dfEmulateKey(key,down) assert(key=='r'); keys[#keys+1]={key,down}; return true end
function dfJbkTakeCommand() return table.remove(commands,1) end
function dfJbkUpdate(key,value) ui[key]=value; return true end
function setTimer(fn,interval,count,...)
    local timer={fn=fn,interval=interval,count=count,args={...},valid=true}
    timers[#timers+1]=timer; return timer
end
function killTimer(t) t.valid=false end
function isTimer(t) return t and t.valid end
function addEventHandler(name,element,fn) handlers[#handlers+1]={name,element,fn}; return true end
function removeEventHandler(name,element,fn)
    for i=#handlers,1,-1 do
        local h=handlers[i]
        if h[1]==name and h[2]==element and h[3]==fn then table.remove(handlers,i); return true end
    end
    return false
end
function dfTriggerEvent(name,element,...)
    source=element
    for _,handler in ipairs(handlers) do if handler[1]==name and (handler[2]==root or handler[2]==element) then handler[3](...) end end
    source=nil; return true
end
function outputChatBox() end
function createMarker(x,y,z,shape,size,r,g,b,a)
    local e={valid=true,kind='marker',pos={x,y,z},color={r,g,b,a}}
    elements[#elements+1]=e; return e
end
function destroyElement(e) e.valid=false end
function setElementInterior(e,v) e.interior=v end
function setElementDimension(e,v) e.dimension=v end
function setCameraTarget() end
function getPlayerNametagText() return 'tester' end
function getPlayerName() return 'tester' end
function getElementData() return nil end
function onUnload(fn) unload=fn end
function dxDrawLine3D() end
function tocolor() return 0 end
function frame(ms)
    for _,handler in ipairs(handlers) do if handler[1]=='onClientPreRender' then handler[3](ms or 1000/60) end end
end
function step(ms)
    now=now+(ms or 50)
    for _,timer in ipairs(timers) do if timer.valid and timer.interval==50 then timer.fn(unpack(timer.args)) end end
    frame(ms)
end
function command(text) commands[#commands+1]=text; step() end
'''

class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.lua=LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(MOCK)
        self.lua.execute(BOT)

    def start(self):
        self.lua.execute('command("bot:1"); step(); assert(ui.bot=="1" and controls.forwards)')

    def test_legacy_route_starts_and_releases_controls_on_stop(self):
        self.start()
        self.lua.execute('command("bot:0"); assert(ui.bot=="0"); for _,v in pairs(controls) do assert(not v) end')
        self.lua.execute('for i=1,80 do step() end; assert(ui.bot=="0" and not controls.forwards)')

    def test_r_replaces_mouse_and_is_released_on_unload(self):
        self.start()
        self.lua.execute('step(); assert(#keys==1 and keys[1][1]=="r" and keys[1][2]); unload()')
        self.lua.execute('assert(#keys==2 and not keys[2][2]); for i=1,80 do step() end; assert(#keys==2)')

    def test_camera_and_cleared_controls_update_on_every_frame(self):
        self.start()
        self.lua.execute('''
            cameraWrites={}
            for i=1,120 do
                controls.forwards=false; controls.sprint=false
                now=now+1000/120
                frame(1000/120)
                assert(#cameraWrites==i and controls.forwards and controls.sprint)
            end
        ''')

    def test_timers_do_not_write_camera_and_unload_removes_movement_handler(self):
        self.start()
        self.lua.execute('''
            cameraWrites={}
            for _,timer in ipairs(timers) do
                if timer.valid and timer.interval==50 then timer.fn(unpack(timer.args)) end
            end
            assert(#cameraWrites==0)
            frame(); assert(#cameraWrites==1)
            command('bot:0'); local count=#cameraWrites
            for i=1,30 do frame() end
            assert(#cameraWrites==count)
            command('bot:1'); frame(); count=#cameraWrites
            local before=#handlers
            unload()
            assert(#handlers==before-1)
            for i=1,30 do frame() end
            assert(#cameraWrites==count and not controls.forwards)
        ''')

    def test_missing_job_marker_does_not_start_or_press_r(self):
        self.lua.execute('job.valid=false; command("bot:1"); for i=1,80 do step() end; assert(ui.bot=="0" and #keys==0)')

if __name__=='__main__': unittest.main()
