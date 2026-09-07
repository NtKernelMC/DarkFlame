"""Route observation and dimension safety for the production tram bot."""
import unittest

from pilot_telemetry_test import ROOT, LuaRuntime


BOT = (ROOT / 'bin/Release/x86/TramBot.lua').read_text(encoding='utf-8')

MOCK = r'''
now,commands,logs,alerts=1000,{},{},0
ui,handlers,timers,keys={},{},{},{}
root={kind='root',valid=true}
resourceRoot={kind='resource',valid=true,parent=root}
localPlayer={kind='player',valid=true,dimension=0,parent=root}
tram={kind='vehicle',valid=true,vehicleType='Train',model=604,TrainTrack=12,dimension=0,parent=root}
occupied=tram
function dfTriggerServerEvent() return true end
function dfTriggerEvent() return true end
function dfAddEvent() return true end
function dfAddEventHandler(name,element,fn) handlers[#handlers+1]={name,element,fn}; return true end
function dfRemoveEventHandler() return true end
function dfCatchServerEvent() return 1 end
function dfRemoveEventCatcher() return true end
function dfEmulateKey(key,down) keys[key]=down; return true end
function dfPlayAlertSignal() alerts=alerts+1; return true end
function dfTramTakeCommand() return table.remove(commands,1) end
function dfTramUpdate(key,value) ui[key]=value; return true end
function dfTramLog(text) logs[#logs+1]=text end
function dfMenuOpen() return false end
function getTickCount() return now end
function setTimer(fn,interval,count,...)
    local timer={fn=fn,interval=interval,count=count,args={...},valid=true}
    timers[#timers+1]=timer
    return timer
end
function isTimer(timer) return timer and timer.valid end
function killTimer(timer) timer.valid=false end
function isElement(element) return type(element)=='table' and element.valid end
function getPedOccupiedVehicle() return occupied end
function getVehicleType(vehicle) return vehicle.vehicleType end
function getElementModel(vehicle) return vehicle.model end
function getElementData(element,key) return element[key] end
function getElementDimension(element) return element.dimension or 0 end
function getElementInterior(element) return element.interior or 0 end
function getVehicleTowedByVehicle() return nil end
function getTrainSpeed() return 0 end
function getTrainPosition() return 0 end
function outputChatBox() end
function bindKey() return true end
function unbindKey() return true end
function isChatBoxInputActive() return false end
function onUnload(fn) unload=fn end
function event(name,element,...)
    source=element
    for _,handler in ipairs(handlers) do
        if handler[1]==name and (handler[2]==root or handler[2]==element) then handler[3](...) end
    end
    source=nil
end
function command(value)
    commands[#commands+1]=value
    for _,timer in ipairs(timers) do if timer.valid and timer.interval==100 then timer.fn(unpack(timer.args)) end end
end
function fireTimer(interval)
    for _,timer in ipairs(timers) do if timer.valid and timer.interval==interval then timer.fn(unpack(timer.args)) end end
end
'''


class TramControllerTests(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(MOCK)
        self.lua.execute(BOT)

    def test_normal_job_lap_event_selects_route_from_train_track(self):
        routes = {
            2: 'ТП Мирка-3', 8: 'ТП Мирка-8', 12: 'ТП Прива-2',
            22: 'ТП Нева-100', 31: 'ТП Прива-8', 32: 'ТП Прива-7',
        }
        for track, route in routes.items():
            with self.subTest(track=track):
                self.lua.execute(f"tram.TrainTrack={track}; event('tram:onClientSetCurrentLap',resourceRoot,1)")
                self.assertEqual(self.lua.globals().ui['route'], route)
        self.assertTrue(any('TrainTrack=12' in line for line in self.lua.globals().logs.values()))

    def test_dimension_change_sounds_and_stops_bot(self):
        self.lua.execute("event('tram:onClientSetCurrentLap',resourceRoot,1); command('bot:1')")
        self.assertEqual(self.lua.globals().ui['bot'], '1')
        self.lua.execute('localPlayer.dimension=5; fireTimer(2000)')
        self.assertEqual(self.lua.globals().alerts, 1)
        self.assertEqual(self.lua.globals().ui['bot'], '0')

    def test_repeated_lap_event_does_not_reset_running_route(self):
        self.lua.execute("event('tram:onClientSetCurrentLap',resourceRoot,1); command('bot:1')")
        self.lua.execute("event('tram:onClientSetCurrentLap',resourceRoot,2)")
        self.assertTrue(any('без сброса прогресса' in line for line in self.lua.globals().logs.values()))


if __name__ == '__main__':
    unittest.main()
