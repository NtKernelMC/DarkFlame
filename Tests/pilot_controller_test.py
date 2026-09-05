"""Controller math and public-client-API integration, running in real Lua 5.1."""
import math
import hashlib
import unittest

from pilot_telemetry_test import ROOT, LuaRuntime, RecorderTests

BEGIN, END = '-- BEGIN EMBEDDED PILOT CONTROLLER', '-- END EMBEDDED PILOT CONTROLLER'


def controller_source():
    source = (ROOT / 'bin/Release/x86/PilotTelemetry.lua').read_text(encoding='utf-8')
    return source[source.index(BEGIN):source.index(END)] + '\nreturn PilotController'


def observation(**changes):
    data = dict(vehicle='plane', driver=True, model=519, dimension=0, interior=0,
        heading_deg=0, pitch_deg=0, roll_deg=0, speed_kmh=20, climb_mps=0, heading_rate_dps=0,
        pitch_rate_dps=0, roll_rate_dps=0, on_ground=True, agl_terrain_m=1.2, health=1000,
        velocity_body_rfu_mps=[0, 20/3.6, 0], position_m=[0, 0, 1.2], horizontal_speed_kmh=20,
        navigation=dict(id='marker', position=[0, 150, 1.2], marker_type='checkpoint',
            heading_error_deg=0, bearing_deg=0, track_error_deg=0, altitude_error_m=0,
            distance_2d_m=150, distance_3d_m=150, color_rgba=[255, 0, 0, 255], marker_size_m=30))
    data.update(changes)
    return data


class MathTests(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.cls = self.lua.execute(controller_source())
        self.c = self.cls.new()
        self.now = 1000

    def start(self, data):
        result = self.c.start(self.c, self.lua.table_from(data, recursive=True), self.now)
        self.assertIs(result, True)

    def update(self, data, ms=50, clear=True):
        self.now = (self.now + ms) % 4294967296
        out = self.c.update(self.c, self.lua.table_from(data, recursive=True), self.now, clear)
        for name in ('throttle', 'brake', 'rudder', 'aileron', 'elevator'):
            self.assertTrue(math.isfinite(out[name]))
            self.assertLessEqual(abs(out[name]), 1)
        self.assertFalse(out['throttle'] > 0 and out['brake'] > 0)
        return out

    def test_same_marker_id_moving_resets_closest(self):
        d = observation(on_ground=False, agl_terrain_m=100, speed_kmh=230)
        self.start(d)
        self.update(d)
        d['navigation'].update(position=[0, 400, 100], distance_2d_m=400, distance_3d_m=400)
        self.update(d)
        self.assertEqual(self.c.waypoint, 2)
        self.assertEqual(self.c.closest, 400)

    def test_ground_speed_and_signs_and_braking(self):
        d = observation(speed_kmh=50, velocity_body_rfu_mps=[0, 50/3.6, 0])
        d['navigation'].update(heading_error_deg=45, bearing_deg=45)
        self.start(d)
        out = self.update(d)
        self.assertGreater(out['brake'], 0)
        self.assertGreater(out['rudder'], 0)
        self.assertEqual(out['aileron'], 0)
        d['navigation'].update(heading_error_deg=-45, bearing_deg=315)
        self.assertEqual(self.update(d)['rudder'], 0)
        for _ in range(3): self.update(d)
        self.assertLess(self.update(d)['rudder'], 0)

    def test_reverse_has_correct_body_axis_and_steering(self):
        d = observation(speed_kmh=3, velocity_body_rfu_mps=[0, -3/3.6, 0])
        d['navigation'].update(heading_error_deg=-150, bearing_deg=210)
        self.c.notify(self.c, 'Двигайтесь аккуратно назад для начала маршрута')
        self.start(d)
        out = self.update(d)
        self.assertGreater(out['brake'], 0)
        self.assertLess(out['rudder'], 0)
        d.update(speed_kmh=20, velocity_body_rfu_mps=[0, -20/3.6, 0])
        self.assertGreater(self.update(d)['throttle'], 0)

    def test_no_takeoff_before_notice_or_during_passengers(self):
        d = observation(speed_kmh=0)
        d['navigation']['marker_type'] = 'ring'
        self.start(d)
        self.assertEqual(self.update(d)['throttle'], 0)
        self.assertEqual(self.c.phase, 'wait_clearance')
        self.c.notify(self.c, 'Взлет разрешен!')
        self.assertEqual(self.update(d)['throttle'], 1)
        self.c.notify(self.c, 'Ожидайте, пока все пассажиры займут свои места в салоне')
        self.assertEqual(self.update(d)['throttle'], 0)

    def test_short_false_ground_signal_is_not_a_flight(self):
        d = observation()
        self.start(d)
        self.update(d)
        d['on_ground'] = False
        self.update(d, 64)
        d['on_ground'] = True
        self.update(d)
        self.assertFalse(self.c.airborne)
        self.assertEqual(self.c.phase, 'taxi')

    def test_old_takeoff_notice_cannot_launch_after_landing_or_on_taxi_checkpoint(self):
        d = observation(speed_kmh=0)
        self.c.notify(self.c, 'Взлет разрешен!', self.now)
        self.start(d)
        self.assertLess(self.update(d)['throttle'], .5)
        d['navigation']['marker_type'] = 'ring'
        self.c.takeoffPermit = self.now - 61000
        self.assertEqual(self.update(d)['throttle'], 0)
        self.assertEqual(self.c.phase, 'wait_clearance')

    def test_obstacle_stops_reverse_motion_and_requires_clear_time(self):
        d = observation(speed_kmh=10, velocity_body_rfu_mps=[0, -10/3.6, 0])
        self.start(d)
        out = self.update(d, clear=False)
        self.assertGreater(out['throttle'], 0)
        self.assertEqual(out['brake'], 0)
        for _ in range(10):
            self.update(d)
        self.assertEqual(self.c.phase, 'obstacle_hold')
        d.update(speed_kmh=0, velocity_body_rfu_mps=[0, 0, 0])
        for _ in range(12):
            self.update(d)
        self.assertEqual(self.c.phase, 'taxi')

    def test_air_roll_pitch_and_circular_heading(self):
        d = observation(on_ground=False, agl_terrain_m=400, speed_kmh=250, horizontal_speed_kmh=250)
        d['navigation'].update(marker_type='ring', altitude_error_m=40, heading_error_deg=25, track_error_deg=25)
        self.start(d)
        out = self.update(d)
        self.assertGreater(out['aileron'], 0)
        self.assertGreater(out['elevator'], 0)
        self.assertLessEqual(self.c.detail['goal_roll_deg'], self.c.detail['bank_limit_deg'])
        d['navigation'].update(heading_error_deg=179, track_error_deg=-179)
        self.update(d)
        self.assertGreater(abs(self.c.detail['goal_roll_deg']), 20)

    def test_descent_can_follow_recorded_steep_rings(self):
        d = observation(on_ground=False, agl_terrain_m=300, speed_kmh=250, climb_mps=-10)
        d['navigation'].update(marker_type='ring', altitude_error_m=-80, distance_2d_m=270)
        self.start(d)
        self.update(d)
        self.assertLess(self.c.detail['goal_pitch_deg'], -15)

    def test_overshooting_one_climb_ring_is_not_landing(self):
        d = observation(on_ground=False, agl_terrain_m=45, speed_kmh=230, surface={'ground_z_m': 0})
        d['navigation'].update(marker_type='ring', position=[0, 150, 40], altitude_error_m=-5)
        self.start(d)
        self.update(d)
        self.assertEqual(self.c.phase, 'flight')
        for height in (32, 24, 16):
            d['navigation']['position'][2] = height
            self.update(d)
        self.assertEqual(self.c.phase, 'approach')

    def test_marker_passed_too_far_disables(self):
        d = observation(on_ground=False, agl_terrain_m=400)
        self.start(d)
        self.update(d)
        d['navigation'].update(heading_error_deg=120, distance_3d_m=200)
        for _ in range(10):
            self.update(d)
        self.assertFalse(self.c.enabled)

    def test_invalid_nav_pause_world_vehicle_and_tick_wrap(self):
        for change in ({'vehicle': None}, {'dimension': 3}, {'pitch_deg': float('nan')}, {'position_m': [9999, 0, 0]}):
            self.setUp()
            d = observation()
            self.start(d)
            d.update(change)
            self.update(d)
            self.assertFalse(self.c.enabled)
        self.setUp()
        self.now = 4294967270
        d = observation()
        self.start(d)
        self.update(d)
        self.assertTrue(self.c.enabled)
        self.update(d, ms=400)
        self.assertFalse(self.c.enabled)
        self.setUp()
        d = observation(navigation={'position': []})
        self.start(d)
        for _ in range(24):
            self.update(d)
        self.assertFalse(self.c.enabled)

    def test_yellow_stop_zone_does_not_hold_accelerator(self):
        d = observation(speed_kmh=0)
        d['navigation'].update(distance_2d_m=5, distance_3d_m=5, color_rgba=[255, 255, 0, 255])
        self.start(d)
        out = self.update(d)
        self.assertEqual(out['throttle'], 0)
        self.assertTrue(out['handbrake'])

    def test_closed_loop_turn_and_altitude_on_simplified_response_model(self):
        # Measured rate scale, uncertain response lag/gain: a stability check, not GTA physics.
        for gain, lag in ((0.7, 0.15), (1.0, 0.2), (1.3, 0.3)):
            self.setUp()
            d = observation(on_ground=False, agl_terrain_m=400, speed_kmh=240,
                horizontal_speed_kmh=240, position_m=[0, 0, 400], pitch_deg=-1.4)
            target, dt = [180, 900, 430], .05
            d['navigation'].update(position=target, marker_type='ring')
            self.start(d)
            closest, max_roll = 1e9, 0
            for _ in range(450):
                delta = [target[i]-d['position_m'][i] for i in range(3)]
                horizontal = math.hypot(*delta[:2])
                bearing = math.degrees(math.atan2(delta[0], delta[1])) % 360
                error = (bearing-d['heading_deg']+180) % 360-180
                distance = math.hypot(horizontal, delta[2])
                closest = min(closest, distance)
                if distance < 20:
                    break
                d['navigation'].update(distance_2d_m=horizontal, distance_3d_m=distance,
                    altitude_error_m=delta[2], bearing_deg=bearing, heading_error_deg=error, track_error_deg=error)
                out = self.update(d)
                if not self.c.enabled:
                    break
                d['roll_rate_dps'] += (47*gain*out['aileron']-d['roll_rate_dps'])*dt/lag
                d['pitch_rate_dps'] += (23*gain*out['elevator']-d['pitch_rate_dps'])*dt/lag
                d['roll_deg'] += d['roll_rate_dps']*dt
                d['pitch_deg'] += d['pitch_rate_dps']*dt
                d['heading_rate_dps'] = .18*d['roll_deg']+9*gain*out['rudder']
                d['heading_deg'] = (d['heading_deg']+d['heading_rate_dps']*dt) % 360
                gamma = math.radians(d['pitch_deg']+1.4)
                heading = math.radians(d['heading_deg'])
                speed = d['speed_kmh']/3.6
                d['climb_mps'] = speed*math.sin(gamma)
                d['position_m'] = [d['position_m'][0]+speed*math.cos(gamma)*math.sin(heading)*dt,
                    d['position_m'][1]+speed*math.cos(gamma)*math.cos(heading)*dt,
                    d['position_m'][2]+d['climb_mps']*dt]
                d['agl_terrain_m'] = d['position_m'][2]
                max_roll = max(max_roll, abs(d['roll_deg']))
            self.assertLess(closest, 20, (gain, lag, closest, self.c.status))
            self.assertLess(max_roll, 30)


ADAPTER_MOCK = '''
physicalKeys, actuatorCalls, losCalls, ground, autoGear, clearPath = {}, {}, {}, true, true, true
plane.position, plane.velocity, gear = {0,0,6.2}, {0,0,0}, true
function getElementModel() return 519 end
function isVehicleOnGround() return ground end
function getKeyState(name) return physicalKeys[name] or false end
function setAnalogControlState(name,value,force)
    actuatorCalls[#actuatorCalls+1] = {name=name, value=value, analog=true, force=force}
    analog[name]=value
    local opposite = {vehicle_left='vehicle_right', vehicle_right='vehicle_left', steer_forward='steer_back', steer_back='steer_forward'}
    if value~=nil and opposite[name] then analog[opposite[name]]=0 end
    return true
end
function setPedControlState(ped,name,value)
    assert(ped==localPlayer)
    actuatorCalls[#actuatorCalls+1] = {name=name, value=value, analog=false}
    if autoGear and name=='sub_mission' and value and not pressed[name] then gear=not gear end
    pressed[name]=value
    return true
end
function getElementBoundingBox() return -10,-9,-1,10,10,3 end
function isLineOfSightClear(...)
    losCalls[#losCalls+1]={...}
    return clearPath
end
m1=marker(0,150,6.2); m1.markerType='checkpoint'
'''


class AdapterTests(unittest.TestCase):
    # Reuse helpers without inheriting the manual-only test methods.
    run_lua, records = RecorderTests.run_lua, RecorderTests.records

    def setUp(self):
        RecorderTests.setUp(self)
        self.run_lua(ADAPTER_MOCK)

    def arm(self, telemetry=True):
        self.run_lua("commands[#commands+1]='autopilot_telemetry:%d'" % telemetry)
        self.run_lua("commands[#commands+1]='autopilot_start'; step(12)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')

    def assert_released(self):
        self.run_lua('''
        for _,name in ipairs({'accelerate','brake_reverse','vehicle_left','vehicle_right','steer_forward','steer_back'}) do
            assert(analog[name]==nil, 'latched '..name)
        end
        for _,name in ipairs({'vehicle_look_left','vehicle_look_right','handbrake','sub_mission'}) do
            assert(pressed[name]~=true, 'latched '..name)
        end
        ''')

    def test_controller_is_in_the_single_release_script(self):
        source = (ROOT / 'bin/Release/x86/PilotTelemetry.lua').read_text(encoding='utf-8')
        self.assertEqual(source.count(BEGIN), 1)
        self.assertEqual(source.count(END), 1)
        self.assertNotIn('dofile(', source)
        self.assertNotIn('require(', source)

    def test_original_audio_files_are_external_and_requests_follow_transitions(self):
        source = (ROOT / 'bin/Release/x86/PilotTelemetry.lua').read_text(encoding='utf-8')
        self.assertNotIn('SOUND_BASE64', source)
        for filename, expected in (
            ('AirbusOff.mp3', '64d72444b4546052e3e34117946302cbde360eeefd45e0745cbe1ba1031bad0e'),
            ('AutoPilotON.mp3', '26cab695fbed203d224e520399c0d165589de8b422576fa2022378170e9484d6')):
            self.assertEqual(hashlib.sha256((ROOT / 'bin/Release/x86' / filename).read_bytes()).hexdigest(), expected)
        self.arm(False)
        self.assertEqual(self.lua.eval('#soundRequests'), 1)
        self.assertEqual(self.lua.eval('soundRequests[1]'), 'on')
        self.run_lua("commands[#commands+1]='autopilot_start'; step(6)")
        self.assertEqual(self.lua.eval('#soundRequests'), 1)
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(6); commands[#commands+1]='autopilot_stop'; step(6)")
        self.assertEqual(self.lua.eval('#soundRequests'), 2)
        self.assertEqual(self.lua.eval('soundRequests[2]'), 'off')
        self.assert_released()

    def test_counter_admin_monitor_follows_autopilot_lifecycle(self):
        self.assertFalse(self.lua.eval('alertMonitorCalls[#alertMonitorCalls]'))
        self.arm(False)
        self.assertTrue(self.lua.eval('alertMonitorCalls[#alertMonitorCalls]'))
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(6)")
        self.assertFalse(self.lua.eval('alertMonitorCalls[#alertMonitorCalls]'))

    def test_collision_raises_safety_siren(self):
        self.arm(False)
        self.run_lua('''
            hit={kind='vehicle',valid=true,parent=root,position={0,0,6.2}}
            event('onClientVehicleCollision',plane,hit,1,0,0,0,0,0,1,0,577)
        ''')
        self.assertEqual(self.lua.eval('alertCalls'), 1)

    def test_nearby_player_raises_safety_siren(self):
        self.arm(False)
        self.run_lua('''
            nearby={kind='player',valid=true,parent=root,name='Nearby_Player',id=42,
                position={10,0,6.2},dimension=0,interior=0}
            players[1]=nearby
            step(45)
        ''')
        self.assertEqual(self.lua.eval('alertCalls'), 1)
        self.assertIn('Человек рядом', self.lua.eval('chatMessages[#chatMessages]'))

    def test_chat_from_streamed_player_or_known_admin_raises_siren(self):
        self.arm(False)
        self.run_lua('''
            nearby={kind='player',valid=true,parent=root,name='Nearby_Player',id=42,
                position={10,0,6.2},dimension=0,interior=0}
            players[1]=nearby
            event('onClientChatMessage',root,'Nearby_Player[42]: привет',255,255,255,0)
        ''')
        self.assertEqual(self.lua.eval('alertCalls'), 1)
        self.run_lua("now=now+5001; event('onClientChatMessage',root,'Maria_Alekseeva[7]: проверка',255,255,255,0)")
        self.assertEqual(self.lua.eval('alertCalls'), 2)
        self.assertIn('сообщение администратора', self.lua.eval('chatMessages[#chatMessages]'))

    def test_rejected_start_does_not_play_on_sound(self):
        self.run_lua("occupied=nil; commands[#commands+1]='autopilot_start'; step(6)")
        self.assertEqual(self.lua.eval('#soundRequests'), 0)

    def test_sound_failure_cannot_prevent_control_release(self):
        self.arm()
        self.run_lua("nativeSoundAvailable=false; event('onClientKey',root,'q',true)")
        self.assert_released()
        self.assertTrue(self.lua.globals().ui['autopilot_sound_error'])

    def test_button_arms_and_stop_releases_and_stops_automatic_log(self):
        self.arm()
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)
        self.assertEqual(self.lua.eval('#serverEvents'), 0)
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(6)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '0')
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assert_released()
        self.assertEqual(len(self.records('autopilot_start')), 1)
        self.assertEqual(len(self.records('autopilot_stop')), 1)
        self.assertEqual(self.records('recording_start')[0]['data']['owner'], 'autopilot')
        self.assertEqual(self.records('recording_stop')[-1]['data']['reason'], 'autopilot_stopped')

    def test_startup_cost_does_not_underflow_the_first_control_tick(self):
        for initial in (1000, 4294967240):
            self.setUp()
            self.run_lua(f'now={initial}')
            self.run_lua('''
            function getRealTime()
                now=(now+11)%4294967296
                return {timestamp=1700000000}
            end
            commands[#commands+1]='autopilot_telemetry:1'
            commands[#commands+1]='autopilot_start'; frame(50)
            ''')
            self.assertEqual(self.lua.globals().ui['autopilot'], '1')
            self.run_lua('step(6)')
            self.assertGreater(self.lua.eval('analog.accelerate'), 0)
            self.assertEqual(len(self.records('autopilot_start')), 1)
            self.assertEqual(self.records('autopilot_stop'), [])

    def test_failure_before_first_control_apply_is_logged_and_stops_owned_recording(self):
        self.run_lua('''
        function getBoundKeys() now=now+40; return {w='down'} end
        commands[#commands+1]='autopilot_telemetry:1'
        commands[#commands+1]='autopilot_start'; frame(50)
        ''')
        self.assertEqual(self.lua.globals().ui['autopilot'], '0')
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assertEqual(self.lua.eval('#actuatorCalls'), 0)
        self.assertEqual(len(self.records('autopilot_start')), 1)
        self.assertEqual(len(self.records('autopilot_stop')), 1)
        self.assertIn('300', self.records('autopilot_stop')[0]['data']['reason'])
        self.assertEqual(self.lua.eval('#soundRequests'), 2)
        self.assertEqual(len(self.records('recording_stop')), 1)

    def test_existing_manual_recording_is_not_claimed_by_autopilot(self):
        self.run_lua("commands[#commands+1]='start'; step(6)")
        self.arm()
        self.run_lua("commands[#commands+1]='autopilot_telemetry:0'; step(6)")
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(6)")
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.assertEqual(self.records('recording_stop'), [])
        self.assertEqual(self.records('recording_start')[0]['data']['owner'], 'manual')

    def test_checkbox_started_recording_stops_with_autopilot(self):
        self.arm(False)
        self.run_lua("commands[#commands+1]='autopilot_telemetry:1'; step(6)")
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(6)")
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assertEqual(self.records('recording_stop')[-1]['data']['owner'], 'autopilot')

    def test_manual_restart_of_recording_during_autopilot_survives_stop(self):
        self.arm()
        self.run_lua("commands[#commands+1]='stop'; step(6); commands[#commands+1]='start'; step(6)")
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(6)")
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.assertEqual(self.records('recording_start')[-1]['data']['owner'], 'manual')

    def test_rejected_start_only_closes_the_recording_it_started(self):
        self.run_lua("commands[#commands+1]='autopilot_telemetry:1'")
        self.run_lua("occupied=nil; commands[#commands+1]='autopilot_start'; step(6)")
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assertEqual(self.records('recording_stop')[-1]['data']['reason'], 'autopilot_start_rejected')
        self.run_lua("commands[#commands+1]='start'; step(6); commands[#commands+1]='autopilot_start'; step(6)")
        self.assertEqual(self.lua.globals().ui['recording'], '1')

    def test_terminal_notification_is_recorded_before_automatic_recording_stops(self):
        self.arm()
        self.run_lua("event('province:sendNotification',pilotRoot,'Вы выполнили рейс! Заработано: 18808 р.')")
        self.assertEqual(self.lua.globals().ui['autopilot'], '0')
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        types = [r['type'] for r in self.records()]
        last_notice = max(i for i,t in enumerate(types) if t=='job_notification')
        self.assertLess(last_notice, types.index('autopilot_stop'))
        self.assertEqual(types[-1], 'recording_stop')

    def test_checkbox_controls_recording_independently_and_control_rate_stays_fast(self):
        self.arm(False)
        self.assertEqual(self.records(), [])
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)
        self.run_lua("commands[#commands+1]='autopilot_telemetry:1'; step(10)")
        self.assertTrue(self.records('sample'))
        self.run_lua("commands[#commands+1]='autopilot_telemetry:0'; step(10)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertEqual(self.lua.globals().ui['recording'], '0')
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)

    def test_completed_notice_stops_even_without_telemetry(self):
        self.arm(False)
        self.run_lua("event('province:sendNotification',pilotRoot,'Вы выполнили рейс! Заработано: 18808 р.'); step(6)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '0')
        self.assert_released()

    def test_chat_and_old_destroyed_notifications_cannot_authorize_takeoff(self):
        self.run_lua("m1.markerType='ring'")
        self.arm()
        self.run_lua("event('onClientChatMessage',root,'Взлет разрешен!',255,255,255,0); step(6)")
        self.assertEqual(self.lua.eval('analog.accelerate'), 0)
        self.run_lua("event('province:sendNotification',pilotRoot,'Взлет разрешен!'); step(6)")
        self.assertEqual(self.lua.eval('analog.accelerate'), 1)

    def test_physical_manual_override_releases_but_minimize_keeps_flying(self):
        self.arm()
        self.run_lua("event('onClientKey',root,'q',true)")
        self.assert_released()
        self.arm()
        self.run_lua("event('onClientMinimize',root); step(12)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertGreater(self.lua.eval('analog.accelerate'), 0)

    def test_collision_contains_world_model_fresh_motion_controls_and_marker(self):
        self.run_lua("commands[#commands+1]='start'; step(6)")
        self.arm()
        self.run_lua("plane.velocity={0,.2,0}; event('onClientVehicleCollision',plane,false,125,5,10,20,30,1,0,0,0,1234)")
        event = self.records('collision')[-1]['data']
        self.assertEqual(event['hit_kind'], 'world')
        self.assertEqual(event['hit_model'], 1234)
        self.assertEqual(event['context']['speed_kmh'], 36)
        self.assertTrue(event['context']['autopilot']['enabled'])
        self.assertTrue(event['context']['landing_gear_down'])
        self.assertEqual(event['context']['navigation']['position'], [0, 150, 6.2])
        self.assert_released()
        self.run_lua("event('onClientVehicleDamage',plane,false,0,12,10,20,30,false)")
        self.assertEqual(self.records('vehicle_damage')[-1]['data']['loss'], 12)

    def test_collision_burst_is_bounded_and_tail_is_flushed(self):
        self.arm()
        self.run_lua("for i=1,100 do event('onClientVehicleCollision',plane,false,i,5,i,20,30,0,0,1,0,1234) end; frame(100)")
        self.assertEqual(len(self.records('collision')), 1)
        burst = self.records('collision_burst')[-1]['data']
        self.assertEqual(burst['count'], 99)
        self.assertEqual(burst['peak_force_raw'], 100)
        self.assertEqual(burst['peak_contact']['position'][0], 100)

    def test_obstacles_probe_wings_with_own_vehicle_ignored(self):
        self.run_lua('clearPath=false; plane.velocity={0,.1,0}')
        self.arm()
        self.assertGreater(self.lua.eval('analog.brake_reverse'), 0)
        self.assertEqual(self.lua.eval('analog.accelerate'), 0)
        self.assertEqual(self.lua.eval('losCalls[1][14]==plane'), True)
        self.assertLessEqual(self.lua.eval('#losCalls'), 13 * 5)
        self.run_lua('clearPath=true; losCalls={}; step(15)')
        self.assertTrue(self.lua.eval('(function() for _,v in ipairs(losCalls) do if math.abs(v[1])>10 and v[14]==plane then return true end end return false end)()'))

    def test_liftoff_contact_debounce_does_not_brake_on_first_air_frame(self):
        self.run_lua("m1.markerType='ring'; event('province:sendNotification',pilotRoot,'Взлет разрешен!')")
        self.arm()
        self.run_lua("plane.velocity={0,1,0}; ground=false; plane.position={0,0,8}; frame(50)")
        self.assertEqual(self.lua.eval('analog.brake_reverse'), 0)
        self.assertEqual(self.lua.eval('analog.accelerate'), 1)

    def test_gear_uses_pulse_then_ack_and_releases(self):
        self.run_lua("ground=false; plane.position={0,0,100}; plane.velocity={0,1.3,0}; m1.position={0,800,110}; m1.markerType='ring'")
        self.arm()
        self.run_lua('step(35)')
        self.assertIs(self.lua.globals().gear, False)
        self.assertIs(self.lua.eval('pressed.sub_mission'), False)
        self.assertEqual(len(self.records('autopilot_gear_request')), 1)

    def test_failure_during_partial_apply_releases_other_controls(self):
        self.arm()
        self.run_lua("function setPedControlState(ped,name,value) pressed[name]=value; if name=='handbrake' and value then return false end; return true end; clearPath=false; plane.velocity={0,0,0}; step(10)")
        self.assertEqual(self.lua.globals().ui['autopilot'], '0')
        self.assert_released()

    def test_vehicle_loss_cleanup_and_frame_gap_release(self):
        for failure in ('occupied=nil; step(6)', '__DarkFlamePilotCleanup()', 'frame(400)'):
            self.setUp()
            self.arm()
            self.run_lua(failure)
            self.assert_released()

    def test_marker_reuse_resets_navigation_derivative(self):
        self.arm()
        self.run_lua("m1.position={150,150,6.2}; frame(50); commands[#commands+1]='stop'; step(6)")
        changes = self.records('waypoint_change')
        self.assertGreaterEqual(len(changes), 2)
        samples = [r['data'] for r in self.records('sample')]
        first = next(d for d in samples if d.get('navigation', {}).get('position') == [150, 150, 6.2])
        self.assertNotIn('heading_error_rate_dps', first['navigation'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
