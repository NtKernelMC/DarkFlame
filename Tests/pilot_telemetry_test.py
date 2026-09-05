"""Flight-recorder contract tests in Lua 5.1 (requires lupa.lua51)."""
import json
import math
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.codex-temp-dia2dump/pilot-test-deps'))
from lupa.lua51 import LuaRuntime

MOCK = r'''
now, commands, writes, ui, events, traces, serverEvents = 1000, {}, {}, {}, {}, {}, {}
alertCalls, alertMonitorCalls, chatMessages = 0, {}, {}
soundRequests, nativeSoundAvailable = {}, true
collectorGeneration=0
root = {kind='root', valid=true}
resourceRoot = {kind='resource', valid=true, name='carrier', parent=root}
pilotRoot = {kind='resource', valid=true, name='province_pilot', parent=root}
localPlayer = {kind='player', valid=true, parent=root}
pilotResource = {name='province_pilot', root=pilotRoot}
notifyResource = {name='province_notifications', root=resourceRoot}
plane = {kind='vehicle', valid=true, parent=root, position={0,0,100}, velocity={0,1,0}, dimension=0, interior=0}
players = {}
occupied = plane
markers, blips, notifications = {}, {}, {}
pressed, analog, gear, console, failLog = {}, {}, false, false, false
matrix = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,100,1}}
function dfPilotLog(text, force, lease)
    if lease~=tostring(collectorGeneration) then return false, 'collector_replaced' end
    if failLog then return false, 'disk full' end
    assert(#text <= 65536)
    writes[#writes+1] = text
    return true, ''
end
function dfPilotUpdate(key,value,lease)
    if key=='attach' then collectorGeneration=collectorGeneration+1; return tostring(collectorGeneration) end
    if lease~=tostring(collectorGeneration) then return false end
    if key=='play_sound' then
        if not nativeSoundAvailable then return false end
        soundRequests[#soundRequests+1]=value
        return true
    end
    ui[key]=value
    return true
end
function dfPilotTakeCommand(lease)
    if lease==tostring(collectorGeneration) then return table.remove(commands,1) end
end
function dfPlayAlertSignal()
    alertCalls=alertCalls+1
    now=(now+(alertDelayMs or 0))%4294967296
    return true
end
function dfSetAlertMonitorEnabled(enabled) alertMonitorCalls[#alertMonitorCalls+1]=enabled; return true end
function getTickCount() return now end
function getRealTime() return {timestamp=1700000000} end
function getVersion() return {number='1.6.0-test'} end
function isElement(e) return type(e)=='table' and e.valid == true end
function getPedOccupiedVehicle() return occupied end
function getPedOccupiedVehicleSeat() return 0 end
function getVehicleController() return localPlayer end
function getElementPosition(e) return unpack(e.position or {0,0,0}) end
function getElementVelocity(e) return unpack(e.velocity or {0,0,0}) end
function getElementRotation() return 0,0,0 end
function getElementAngularVelocity() return 0,0,0 end
function getElementMatrix() return matrix end
function getElementDimension(e) return e.dimension or 0 end
function getElementInterior(e) return e.interior or 0 end
function getElementType(e) return e.kind end
function getElementID(e) return e.name or '' end
function getElementParent(e) return e.parent end
function getElementModel() return 577 end
function getVehicleType() return 'Plane' end
function getElementHealth() return 987 end
function getVehicleEngineState() return true end
function getVehicleLandingGearDown() return gear end
function isVehicleOnGround() return false end
function isElementInWater() return false end
function isElementFrozen() return false end
function getElementCollisionsEnabled() return true end
function isVehicleBlown() return false end
function getVehicleCurrentGear() return 1 end
function getVehicleGravity() return 0,0,-1 end
function getGroundPosition() return 5 end
function getWaterLevel() return false end
function getGameSpeed() return 1 end
function getGravity() return .008 end
function getWindVelocity() return 0,0,0 end
function getWeather() return 0 end
function getFPSLimit() return 60 end
function getVehicleHandling() return {mass=10000,turnMass=100000,centerOfMass={0,0,0}} end
function getControlState(name) return pressed[name] or false end
function isControlEnabled() return true end
function getAnalogControlState(name) return analog[name] or 0 end
function getKeyState(key) return pressed[key] or false end
function isChatBoxInputActive() return false end
function isConsoleActive() return console end
function isCursorShowing() return false end
function dfMenuOpen() return false end
function isMTAWindowActive() return true end
function getBoundKeys(name) return {w='down'} end
function getResourceFromName(name) if name=='province_pilot' then return pilotResource end return false end
function getResourceRootElement(r) return r.root end
function getResourceName(r) return r.name end
function getResourceState(r) return r.state or 'running' end
function getServerIp() return '185.71.66.70:22003' end
function triggerServerEvent(name,source,...)
    serverEvents[#serverEvents+1]={name=name,source=source,args={...}}
    return true
end
function getElementsByType(kind, scope)
    local output = {}
    local list = kind=='marker' and markers or kind=='blip' and blips
        or kind=='notifications:Static' and notifications or kind=='player' and players or {}
    for _,e in ipairs(list) do
        local parent=e
        while parent and parent~=scope do parent=parent.parent end
        if e.valid and (not scope or parent==scope) then output[#output+1]=e end
    end
    return output
end
function isElementStreamedIn() return true end
function getPlayerNametagText(e) return e.name or 'Player' end
function getPlayerName(e) return e.name or 'Player' end
function getMarkerType(e) return e.markerType or 'ring' end
function getMarkerSize() return 20 end
function isElementWithinMarker(e,m)
    if (e.dimension or 0)~=(m.dimension or 0) or (e.interior or 0)~=(m.interior or 0) then return false end
    local p,t,d=e.position or {0,0,0},m.position,0
    for i=1,3 do d=d+(p[i]-t[i])^2 end
    return d<=getMarkerSize(m)^2
end
function getMarkerColor(e) return 255,0,0,e.alpha or 255 end
function getElementAlpha(e) return e.alpha or 255 end
function getMarkerTarget() return false end
function getBlipIcon() return 0 end
function getBlipSize() return 2 end
function getBlipVisibleDistance() return 4000 end
function getBlipOrdering() return 0 end
function getBlipColor() return 255,0,0,255 end
function getElementAttachedTo() return false end
function getElementData(e,key) return e[key] or false end
function outputChatBox(text) chatMessages[#chatMessages+1]=text end
function addEventHandler(name,element,callback)
    events[name] = events[name] or {}
    table.insert(events[name],{element,callback})
    return true
end
function removeEventHandler(name,element,callback)
    for i,v in ipairs(events[name] or {}) do if v[2]==callback then table.remove(events[name],i); return true end end
end
function addDebugHook() error('debug hooks are forbidden') end
function removeDebugHook() error('debug hooks are forbidden') end
function event(name,element,...)
    source=element
    local copy={}
    for _,entry in ipairs(events[name] or {}) do copy[#copy+1]=entry end
    for _,entry in ipairs(copy) do if entry[1]==root or entry[1]==element then entry[2](...) end end
    source=nil
end
function frame(ms)
    now=(now+ms)%4294967296
    event('onClientPreRender',root,ms)
end
function step(count,ms) for i=1,count do frame(ms or 50) end end
function marker(x,y,z,owner)
    local e={kind='marker',valid=true,position={x,y,z},parent=owner or pilotRoot}
    markers[#markers+1]=e
    return e
end
function forbidden() error('recorder attempted to mutate gameplay') end
setControlState, setPedControlState, setAnalogControlState = forbidden, forbidden, forbidden
setElementPosition, setElementVelocity, setVehicleLandingGearDown = forbidden, forbidden, forbidden
triggerEvent, call = forbidden, forbidden
'''


class RecorderTests(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(MOCK)
        self.source = (ROOT / 'bin/Release/x86/PilotTelemetry.lua').read_text(encoding='utf-8')
        self.lua.execute(self.source)

    def run_lua(self, source):
        return self.lua.execute(source)

    def start(self):
        self.run_lua("commands[#commands+1]='start'; step(6)")

    def records(self, kind=None):
        data = self.lua.eval("table.concat(writes)")
        records = [json.loads(line) for line in data.splitlines() if line]
        return [v for v in records if not kind or v['type'] == kind]

    def stop(self):
        self.run_lua("commands[#commands+1]='stop'; step(6)")

    def test_accept_job_command_uses_pilot_resource_root(self):
        self.run_lua("commands[#commands+1]='accept_job'; step(6)")
        self.assertEqual(self.lua.eval("#serverEvents"), 1)
        self.assertEqual(self.lua.eval("serverEvents[1].name"), 'pilot:onJobAccepted')
        self.assertTrue(self.lua.eval("serverEvents[1].source == pilotRoot"))
        self.assertEqual(self.lua.globals().ui['status'], 'Запрос на трудоустройство отправлен.')

    def test_accept_job_rejects_stopped_pilot_resource(self):
        self.run_lua("pilotResource.state='stopped'; commands[#commands+1]='accept_job'; step(6)")
        self.assertEqual(self.lua.eval("#serverEvents"), 0)
        self.assertEqual(self.lua.globals().ui['resource_ready'], '0')

    def test_motion_units_false_values_and_basis(self):
        self.start()
        self.stop()
        sample = self.records('sample')[-1]['data']
        self.assertEqual(sample['velocity_world_mps'], [0, 50, 0])
        self.assertEqual(sample['speed_kmh'], 180)
        self.assertEqual(sample['velocity_body_rfu_mps'], [0, 50, 0])
        self.assertEqual(sample['heading_deg'], 0)
        self.assertEqual(sample['agl_terrain_m'], 95)
        self.assertIs(sample['landing_gear_down'], False)
        self.assertIs(sample['controls']['digital']['accelerate'], False)
        self.assertIsNone(sample['surface']['water_z_m'])
        self.assertEqual(self.records('collector_error'), [])

    def test_wrapped_heading_and_acceleration(self):
        self.run_lua("local a=math.rad(359); matrix={{math.cos(a),-math.sin(a),0},{math.sin(a),math.cos(a),0},{0,0,1}}")
        self.start()
        self.run_lua("local a=math.rad(1); matrix={{math.cos(a),-math.sin(a),0},{math.sin(a),math.cos(a),0},{0,0,1}}; plane.velocity={0,1.1,0}; frame(50)")
        self.stop()
        samples = [r['data'] for r in self.records('sample')]
        self.assertTrue(any(abs(s.get('heading_rate_dps', 0) - 40) < 1e-5 for s in samples))
        self.assertTrue(any(abs(s.get('acceleration_world_mps2', [0,0,0])[1]-100) < 1e-5 for s in samples))

    def test_dynamic_markers_and_dimension_filter(self):
        self.run_lua("m1=marker(0,500,100); m2=marker(200,800,130); unrelated=marker(0,1,100); unrelated.dimension=2")
        self.start()
        self.run_lua("event('onClientMarkerHit',m1,localPlayer,true); event('onClientElementDestroy',m1); m1.valid=false; step(7)")
        self.stop()
        changes = self.records('target_change')
        self.assertEqual(changes[0]['data']['new']['position'], [0,500,100])
        self.assertEqual(changes[-1]['data']['new']['position'], [200,800,130])
        self.assertEqual(len(self.records('onClientMarkerHit')), 1)
        self.assertTrue(self.records('navigation_destroy'))
        self.assertEqual(self.records('sample')[-1]['data']['navigation']['altitude_error_m'], 30)

    def test_short_input_edges_and_disabled_controls(self):
        self.start()
        self.run_lua("pressed.vehicle_look_left=true; analog.vehicle_look_left=0.4; event('onClientKey',root,'q',true); frame(10); pressed.vehicle_look_left=false; analog.vehicle_look_left=0; event('onClientKey',root,'q',false); frame(10)")
        self.stop()
        edges = [r['data']['pressed'] for r in self.records('key')]
        self.assertEqual(edges, [True, False])
        self.assertTrue(any(r['data']['input']['raw_analog']['vehicle_look_left'] == .4 for r in self.records('input')))

    def test_world_marker_storm_does_not_scan_or_rescan_other_jobs(self):
        self.run_lua('''
            mine=marker(0,500,100)
            for i=1,2200 do marker(0,i,100,resourceRoot) end
            markerReads=0
            local position=getElementPosition
            getElementPosition=function(e)
                if e.kind=='marker' then
                    markerReads=markerReads+1
                    assert(e==mine, 'unrelated world marker was inspected')
                end
                return position(e)
            end
            local enumerate=getElementsByType
            getElementsByType=function(kind, scope)
                if kind=='marker' or kind=='blip' then assert(scope==pilotRoot) end
                return enumerate(kind, scope)
            end
        ''')
        self.start()
        self.run_lua('''
            frame(50)
            scansBefore=markerReads
            for i=2,#markers do
                event('onClientElementDestroy',markers[i])
                markers[i].valid=false
            end
            frame(50)
            assert(markerReads==scansBefore+1, 'destroy storm caused a rescan')
            step(20)
        ''')
        self.stop()
        navigation = self.records('navigation')
        self.assertTrue(navigation)
        self.assertTrue(all(n['data']['inspected'] == 1 for n in navigation))
        self.assertTrue(all(len(n['data']['candidates']) == 1 for n in navigation))
        self.assertEqual(self.records('navigation_destroy'), [])
        self.assertTrue(self.records('collector_performance'))

    def test_notifications_use_event_handlers_without_debug_hooks(self):
        self.assertNotIn('DebugHook', self.source)
        self.start()
        self.run_lua('''
            assert(#events['province:sendNotification']==1)
            event('province:sendNotification',pilotRoot,'Уберите шасси')
        ''')
        self.stop()
        self.assertIsNone(self.lua.eval('traces.preFunction'))
        self.assertEqual(len(self.records('job_notification')), 1)

    def test_unchanged_input_deduplicates_but_captures_enabled_and_chat_changes(self):
        self.start()
        self.run_lua('''
            step(20)
            isControlEnabled=function(name) return name~='steer_forward' end
            frame(10)
            console=true
            frame(10)
            console=false
            frame(10)
        ''')
        self.stop()
        inputs = [r['data']['input'] for r in self.records('input')]
        self.assertEqual(len(inputs), 4)
        self.assertFalse(inputs[1]['enabled']['steer_forward'])
        self.assertTrue(inputs[2]['console'])
        self.assertFalse(inputs[3]['console'])

    def test_ground_transitions_and_live_marker_color(self):
        self.run_lua("m1=marker(0,500,100); grounded=true; isVehicleOnGround=function() return grounded end")
        self.start()
        self.run_lua("grounded=false; plane.velocity={0,1,.1}; frame(50); getMarkerColor=function() return 255,180,20,90 end; frame(50); grounded=true; gear=true; frame(50)")
        self.stop()
        transitions = [r['data'] for r in self.records('ground_contact_change')]
        self.assertEqual([r['on_ground'] for r in transitions], [True, False, True])
        self.assertEqual(transitions[1]['climb_mps'], 5)
        self.assertEqual(transitions[2]['previous_duration_ms'], 100)
        self.assertIs(transitions[2]['landing_gear_down'], True)
        self.assertEqual(self.records('sample')[-1]['data']['navigation']['color_rgba'], [255,180,20,90])

    def test_heading_marker_bearing_and_signed_remaining_turn(self):
        self.run_lua('m=marker(0,500,100)')
        self.start()
        ticks = self.run_lua('''
            local ticks={}
            for _, pair in ipairs({{0,90},{90,0},{359,1},{1,359},{90,270}}) do
                local h,b=math.rad(pair[1]),math.rad(pair[2])
                matrix={{math.cos(h),-math.sin(h),0},{math.sin(h),math.cos(h),0},{0,0,1}}
                m.position={500*math.sin(b),500*math.cos(b),100}
                frame(50)
                ticks[#ticks+1]=now
            end
            return ticks
        ''')
        self.stop()
        samples = {r['tick_ms']: r['data'] for r in self.records('sample')}
        cases = [(0,90,90,'right'), (90,0,-90,'left'), (359,1,2,'right'),
                 (1,359,-2,'left'), (90,270,-180,'either')]
        for i, (heading,bearing,error,direction) in enumerate(cases,1):
            nav = samples[ticks[i]]['navigation']
            self.assertAlmostEqual(nav['current_heading_deg'], heading)
            self.assertAlmostEqual(nav['bearing_deg'], bearing)
            self.assertAlmostEqual(nav['heading_error_deg'], error)
            self.assertAlmostEqual(nav['turn_remaining_deg'], abs(error))
            self.assertEqual(nav['turn_direction'], direction)

    def test_body_angles_vertical_target_and_coincident_center(self):
        self.run_lua('m=marker(100,100,200)')
        self.start()
        ticks = self.run_lua('''
            frame(50); local angled=now
            m.position={0,0,200}; frame(50); local above=now
            m.position={0,0,100}; frame(50)
            return {angled,above,now}
        ''')
        self.stop()
        samples = {r['tick_ms']: r['data']['navigation'] for r in self.records('sample')}
        self.assertAlmostEqual(samples[ticks[1]]['body_yaw_to_center_deg'], 45)
        self.assertAlmostEqual(samples[ticks[1]]['body_pitch_to_center_deg'], 35.26438968)
        self.assertIsNone(samples[ticks[2]]['heading_error_deg'])
        self.assertEqual(samples[ticks[2]]['turn_direction'], 'unknown')
        self.assertIsNone(samples[ticks[2]]['body_yaw_to_center_deg'])
        self.assertAlmostEqual(samples[ticks[2]]['body_pitch_to_center_deg'], 90)
        self.assertIsNone(samples[ticks[3]]['body_pitch_to_center_deg'])

    def test_gear_changes_false_unknown_and_vehicle_reset(self):
        self.start()
        self.run_lua('''
            gear=true; frame(50)
            gear=false; frame(50)
            getVehicleLandingGearDown=nil; frame(50)
            getVehicleLandingGearDown=function() return gear end; frame(50)
            occupied={kind='vehicle',valid=true,parent=root,position={0,0,100},velocity={0,0,0}}
            frame(50)
        ''')
        self.stop()
        changes = [r['data'] for r in self.records('landing_gear_change')]
        self.assertEqual([r['state'] for r in changes], ['up','down','up','unknown','up','up'])
        self.assertEqual([r['landing_gear_down'] for r in changes], [False,True,False,None,False,False])
        self.assertTrue(changes[0]['initial_observation'])
        self.assertTrue(changes[-1]['initial_observation'])
        self.assertIsNone(changes[-1]['previous'])
        self.assertEqual(changes[2]['previous_duration_ms'], 50)
        self.assertTrue(all('landing_gear_state_age_ms' in r['data'] for r in self.records('sample')))

    def test_taxi_braking_reverse_yaw_radius_and_input_duration(self):
        self.run_lua('isVehicleOnGround=function() return grounded end; grounded=true; plane.velocity={0,.2,0}')
        self.start()
        ticks = self.run_lua('''
            pressed.brake_reverse=true; plane.velocity={0,.16,0}; frame(50); local braking=now
            frame(100); local held=now
            plane.velocity={0,-.1,0}; frame(50); local reverse=now
            plane.velocity={0,-.08,0}; frame(50); local reverseBrake=now
            local a=math.rad(1)
            matrix={{math.cos(a),-math.sin(a),0},{math.sin(a),math.cos(a),0},{0,0,1}}
            frame(100); local turning=now
            pressed.brake_reverse=false; grounded=false; frame(50)
            return {braking,held,reverse,reverseBrake,turning,now}
        ''')
        self.stop()
        samples = {r['tick_ms']:r['data'] for r in self.records('sample')}
        braking, held, reverse, reverse_brake, turning, airborne = [samples[ticks[i]] for i in range(1,7)]
        self.assertAlmostEqual(braking['taxi']['speed_change_mps2'], -40)
        self.assertAlmostEqual(braking['taxi']['forward_accel_mps2'], -40)
        self.assertEqual(braking['control_held_ms']['brake_reverse'], 0)
        self.assertEqual(held['control_held_ms']['brake_reverse'], 100)
        self.assertEqual(reverse['taxi']['direction'], 'reverse')
        self.assertAlmostEqual(reverse['taxi']['forward_speed_mps'], -5)
        self.assertAlmostEqual(reverse_brake['taxi']['speed_change_mps2'], -20)
        self.assertAlmostEqual(turning['taxi']['yaw_rate_dps'], 10)
        self.assertAlmostEqual(turning['taxi']['yaw_radius_estimate_m'], 4 / math.radians(10))
        self.assertNotIn('brake_reverse', airborne['control_held_ms'])
        self.assertIsNone(airborne['taxi'])

    def test_remaining_turn_rate_resets_on_target_change_and_gaps(self):
        self.run_lua('m=marker(100,500,100)')
        self.start()
        ticks = self.run_lua('''
            local a=math.rad(1)
            matrix={{math.cos(a),-math.sin(a),0},{math.sin(a),math.cos(a),0},{0,0,1}}
            frame(100); local turning=now
            event('onClientElementDestroy',m); m.valid=false; marker(-100,500,100)
            frame(50); local changed=now
            frame(1000)
            return {turning,changed,now}
        ''')
        self.stop()
        samples = {r['tick_ms']:r['data']['navigation'] for r in self.records('sample')}
        self.assertAlmostEqual(samples[ticks[1]]['turn_remaining_rate_dps'], -10)
        self.assertNotIn('turn_remaining_rate_dps', samples[ticks[2]])
        self.assertNotIn('turn_remaining_rate_dps', samples[ticks[3]])

    def test_notifications_full_text_timer_status_static(self):
        self.start()
        self.run_lua(r'''
            event('province:sendNotification',pilotRoot,'#ff0000Уберите шасси!\nСледующий "маркер"',2)
            event('plrTimer:init',pilotRoot,14,'Пилот','Следуйте к маркеру')
            event('notifications.createStatus.client',pilotRoot,'Выпустите шасси',true)
            n={kind='notifications:Static',valid=true,parent=pilotRoot,data={header='Пилот',text='Посадка',sec=14}}
            notifications[1]=n
            event('onClientElementDataChange',n,'data')
            n.data.sec=13
            event('onClientElementDataChange',n,'data')
            event('onClientChatMessage',root,'Уберите шасси',255,255,255,0)
        ''')
        self.stop()
        notifications = self.records('job_notification')
        self.assertEqual(len(notifications), 6)
        self.assertEqual(notifications[0]['data']['text_raw'], '#ff0000Уберите шасси!\nСледующий "маркер"')
        self.assertNotIn('#ff0000', notifications[0]['data']['text_plain'])
        self.assertEqual(notifications[1]['data']['payload']['arguments']['values'][0], 14)
        self.assertEqual(notifications[2]['data']['origin'], 'province_pilot')
        self.assertEqual(notifications[4]['data']['payload']['data']['sec'], 13)
        self.assertTrue(all(n['data']['context']['position'] == [0,0,100] for n in notifications))

    def test_destroying_pilot_notification_keeps_the_flight_recording(self):
        self.start()
        self.run_lua('''
            n={kind='notifications:Static',valid=true,parent=pilotRoot,
                data={header='Пилот',text='Ожидайте, пока все пассажиры займут свои места'}}
            notifications[1]=n
            event('onClientElementDataChange',n,'data')
            event('onClientElementDestroy',n)
            n.valid=false
            step(30)
        ''')
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.stop()
        destroyed = self.records('job_notification')
        destroyed = next(r for r in destroyed if r['data']['channel'] == 'static_destroy')
        self.assertEqual(destroyed['data']['origin'], 'province_pilot')
        self.assertIsInstance(destroyed['data']['text_raw'], str)
        self.assertGreater(self.records('sample')[-1]['tick_ms'], destroyed['tick_ms'] + 1000)
        self.assertEqual(self.records('collector_error'), [])

    def test_bad_notification_is_isolated_and_diagnostics_are_throttled(self):
        self.start()
        self.run_lua('''
            local key=setmetatable({}, {__tostring=function() error('bad notification key') end})
            for i=1,100 do event('province:sendNotification',pilotRoot,{[key]=1}) end
            step(10)
            event('province:sendNotification',pilotRoot,'Взлет разрешен!')
            step(10)
        ''')
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.assertEqual(self.lua.globals().ui['observer_errors'], '100')
        self.stop()
        self.assertEqual(len(self.records('observer_error')), 1)
        self.assertEqual(self.records('collector_error'), [])
        self.assertTrue(any(r['data']['text_plain'] == 'Взлет разрешен!' for r in self.records('job_notification')))

    def test_bad_notification_poll_keeps_sampling(self):
        self.start()
        self.run_lua('''
            local key=setmetatable({}, {__tostring=function() error('bad polled data') end})
            notifications[1]={kind='notifications:Static',valid=true,parent=pilotRoot,data={[key]=1}}
            step(30)
            notifications={}
            step(25)
        ''')
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.stop()
        self.assertTrue(self.records('observer_error'))
        self.assertEqual(self.records('collector_error'), [])
        self.assertGreater(self.records('sample')[-1]['elapsed_ms'], 2500)

    def test_fatal_sample_error_still_stops_and_remains_visible(self):
        self.start()
        self.run_lua('''
            getElementMatrix=function()
                return setmetatable({}, {__index=function() error('matrix access failed') end})
            end
            step(10)
        ''')
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assertIn('matrix access failed', self.lua.globals().ui['collector_error'])
        self.assertEqual(self.records('collector_error')[0]['data']['observer'], 'onClientPreRender')
        self.assertIsNone(self.lua.eval('traces.preFunction'))

    def test_stop_restart_preserves_log_and_resets_derivatives(self):
        self.start()
        self.stop()
        count = len(self.records())
        self.start()
        self.stop()
        self.assertGreater(len(self.records()), count)
        self.assertEqual(len(self.records('recording_start')), 2)
        runs = {r['run'] for r in self.records('recording_start')}
        self.assertEqual(len(runs), 2)
        for run in runs:
            first = next(r['data'] for r in self.records('sample') if r['run'] == run)
            self.assertFalse(first['derivatives_valid'])

    def test_destination_from_notification_event(self):
        self.start()
        self.run_lua("event('province:sendNotification',pilotRoot,'Уведомление: Ваш пункт назначения - Либерти Сити'); step(6)")
        self.stop()
        destination = self.records('job_destination')[0]['data']
        self.assertEqual(destination['destination'], 'Либерти Сити')
        self.assertEqual(self.lua.globals().ui['destination'], 'Либерти Сити')
        self.assertEqual(self.records('job_notification')[0]['data']['text_raw'], 'Уведомление: Ваш пункт назначения - Либерти Сити')

    def test_gap_teleport_and_vehicle_loss(self):
        self.start()
        self.run_lua("plane.position={5000,5000,100}; frame(50); frame(1500); occupied=nil; step(6)")
        self.stop()
        self.assertGreaterEqual(len(self.records('discontinuity')), 2)
        self.assertIsNone(self.records('sample')[-1]['data']['vehicle'])
        self.assertEqual(self.records('collector_error'), [])

    def test_missing_optional_api_and_false_matrix(self):
        self.run_lua("getElementAngularVelocity=nil; getElementMatrix=function() return false end; getWaterLevel=nil")
        self.start()
        self.stop()
        self.assertIsNone(self.records('sample')[-1]['data']['angular_velocity_raw'])
        self.assertNotIn('heading_deg', self.records('sample')[-1]['data'])
        self.assertEqual(self.records('collector_error'), [])

    def test_log_failure_stops_and_is_not_success(self):
        self.start()
        self.run_lua("failLog=true; commands[#commands+1]='stop'; step(8)")
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assertIn('disk full', self.lua.globals().ui['status'])

    def test_tick_wrap_and_cleanup_reload(self):
        self.run_lua("now=4294967200")
        self.start()
        self.run_lua("__DarkFlamePilotCleanup()")
        self.assertEqual(len(self.records('recording_stop')), 1)
        self.assertEqual(self.lua.eval("#events.onClientPreRender"), 0)
        self.lua.execute(self.source)
        self.assertEqual(self.lua.eval("#events.onClientPreRender"), 1)
        self.assertTrue(all(r['elapsed_ms'] >= 0 for r in self.records()))

    def test_pilot_resource_lifecycle_preserves_recording(self):
        self.start()
        self.run_lua("pilotResource.state='stopped'; event('onClientResourceStop',pilotRoot,pilotResource); step(6)")
        self.assertEqual(self.lua.globals().ui['resource_ready'], '0')
        self.assertEqual(self.records('recording_stop')[-1]['data']['reason'], 'pilot_resource_stop')
        count = len(self.records())
        self.run_lua("pilotResource.state='running'; event('onClientResourceStart',pilotRoot,pilotResource)")
        self.start()
        self.stop()
        self.assertGreater(len(self.records()), count)
        self.assertEqual(len(self.records('recording_start')), 2)

    def test_second_lua_environment_replaces_old_collector(self):
        self.start()
        self.lua.execute('''
            local env=setmetatable({_G=false,__DarkFlamePilotCleanup=false},{__index=_G})
            env._G=env
            local chunk=assert(loadstring(...))
            setfenv(chunk,env)
            chunk()
            step(8)
        ''', self.source)
        self.assertEqual(self.lua.eval('#events.onClientPreRender'), 1)
        self.assertEqual(self.lua.globals().ui['loaded'], '1')
        self.start()
        self.stop()
        self.assertEqual(self.records('collector_error'), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
