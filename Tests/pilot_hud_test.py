"""HUD geometry, lifecycle and isolation from flight control, using real Lua 5.1."""
import math
import unittest

import pilot_controller_test as controller_tests
import pilot_telemetry_test as recorder_tests


HUD_MOCK = '''
draws, screenW, screenH, menuOpen = {}, 1280, 800, false
projectedX, projectedY = 700, 280
projectionCalls, scanCalls = 0, 0
local scan = getElementsByType
function getElementsByType(...) scanCalls=scanCalls+1; return scan(...) end
function guiGetScreenSize() return screenW,screenH end
function dfMenuOpen() return menuOpen end
function getScreenFromWorldPosition(...)
    projectionCalls=projectionCalls+1
    return projectedX, projectedY
end
function dxDrawLine(...) draws[#draws+1]={kind='line',args={...}}; return true end
function dxDrawText(...) draws[#draws+1]={kind='text',args={...}}; return true end
function render() draws={}; event('onClientRender',root) end
'''


class HudTests(unittest.TestCase):
    run_lua, records = recorder_tests.RecorderTests.run_lua, recorder_tests.RecorderTests.records
    def arm(self, telemetry=True):
        self.run_lua("commands[#commands+1]='autopilot_hud:1'")
        controller_tests.AdapterTests.arm(self, telemetry)

    def setUp(self):
        self.lua = recorder_tests.LuaRuntime(unpack_returned_tuples=True)
        self.run_lua(recorder_tests.MOCK)
        self.run_lua(controller_tests.ADAPTER_MOCK)
        self.run_lua(HUD_MOCK)
        self.source = (recorder_tests.ROOT / 'bin/Release/x86/PilotTelemetry.lua').read_text(encoding='utf-8')
        # Test-only access to locals; production exports no diagnostic/control globals.
        self.access = self.run_lua(self.source + '\nreturn {state=state, autopilot=autopilot}')

    def commands(self):
        return [dict(kind=d['kind'], args=list(d['args'].values())) for d in self.lua.globals().draws.values()]

    def texts(self):
        return [d['args'][0] for d in self.commands() if d['kind'] == 'text']

    def render(self):
        self.run_lua('render()')
        self.assertIsNone(self.access['state']['hudError'])

    def reset_smoothing(self):
        self.access['state']['hudSmooth'] = None

    def horizon(self):
        # Two long green level-zero segments; omit their dark outlines.
        return [d['args'] for d in self.commands() if d['kind'] == 'line'
            and d['args'][4] == 0xED70FF92
            and math.hypot(d['args'][2]-d['args'][0], d['args'][3]-d['args'][1]) > 110
            and abs(d['args'][1]-368) < 105 and abs(d['args'][3]-368) < 105]

    def test_default_off_and_enabled_only_draws_during_autopilot(self):
        self.run_lua('step(6)')
        self.assertEqual(self.lua.globals().ui['autopilot_hud'], '0')
        self.render()
        self.assertEqual(self.commands(), [])
        self.arm()
        self.render()
        self.assertIn('AP  /  TAXI', self.texts())
        self.assertIn('REC  /  TELEMETRY', self.texts())
        self.run_lua("commands[#commands+1]='autopilot_stop'; step(6)")
        self.render()
        self.assertEqual(self.commands(), [])

    def test_dashboard_bridge_contains_current_values_and_fits_state_limit(self):
        self.arm()
        fields = self.lua.globals().ui['dashboard'].split('|')
        self.assertEqual(len(fields), 16)
        self.assertEqual(fields[:3], ['0', '0', '6'])
        self.assertEqual(fields[8:12], ['150', 'ВЫПУЩЕНЫ', 'ЗЕМЛЯ', 'checkpoint'])
        self.assertLess(len(list(self.lua.globals().ui.keys())), 32)
        self.run_lua('occupied=nil; step(12)')
        fields = self.lua.globals().ui['dashboard'].split('|')
        self.assertEqual(fields[:3], ['--', '--', '--'])

    def test_checkbox_is_independent_and_survives_ap_restart(self):
        self.arm()
        self.run_lua("commands[#commands+1]='autopilot_hud:0'; step(6)")
        self.render()
        self.assertEqual(self.commands(), [])
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.run_lua("commands[#commands+1]='autopilot_stop'; commands[#commands+1]='stop'; step(6)")
        controller_tests.AdapterTests.arm(self, telemetry=False)
        self.render()
        self.assertEqual(self.commands(), [])
        self.run_lua("commands[#commands+1]='autopilot_hud:1'; step(6)")
        self.render()
        self.assertIn('REC OFF', self.texts())

    def test_render_does_not_scan_write_or_control(self):
        self.arm()
        before = self.lua.eval('#writes, #actuatorCalls, scanCalls, #serverEvents')
        for _ in range(120):
            self.render()
        self.assertEqual(self.lua.eval('#writes, #actuatorCalls, scanCalls, #serverEvents'), before)
        self.assertLess(len(self.commands()), 220)

    def test_heading_wrap_uses_shortest_direction(self):
        self.arm()
        self.access['state']['latest']['heading_deg'] = 359
        self.render()
        self.assertIn('HDG 359', self.texts())
        self.access['state']['latest']['heading_deg'] = 1
        self.run_lua('now=now+50')
        self.render()
        self.assertIn('HDG 000', self.texts())
        self.assertIn('000', self.texts())
        self.assertNotIn('360', self.texts())

    def test_pitch_and_roll_horizon_direction(self):
        self.arm()
        self.render()
        level = self.horizon()
        self.assertEqual(len(level), 2)
        self.assertTrue(all(abs(line[1]-368) < .01 for line in level))
        self.access['state']['latest']['pitch_deg'] = 10
        self.reset_smoothing()
        self.render()
        self.assertTrue(all(abs(line[1]-428) < .01 for line in self.horizon()))
        self.access['state']['latest']['pitch_deg'] = 0
        self.access['state']['latest']['roll_deg'] = 20
        self.reset_smoothing()
        self.render()
        self.assertEqual(len(self.horizon()), 2)
        self.assertTrue(all((line[3]-line[1])/(line[2]-line[0]) < 0 for line in self.horizon()))

    def test_target_projection_and_missing_optional_values(self):
        self.arm()
        self.render()
        diamond = [d['args'] for d in self.commands() if d['kind']=='line'
            and d['args'][:4] == [700, 269, 711, 280]]
        self.assertEqual(len(diamond), 1)
        self.run_lua('projectedX=false; projectedY=false')
        self.render()
        self.assertIn('TARGET OFFSCREEN', self.texts())
        item = self.access['state']['latest']
        for name in ('heading_deg', 'pitch_deg', 'roll_deg', 'speed_kmh', 'agl_terrain_m', 'climb_mps'):
            item[name] = self.lua.table()
        item['navigation'], item['position_m'] = self.lua.table(), self.lua.table()
        item['landing_gear_state'], item['on_ground'] = 'unknown', None
        self.render()
        self.assertIn('ATTITUDE --', self.texts())
        self.assertIn('GEAR  --', self.texts())
        self.assertIn('NO TARGET', self.texts())
        self.assertIn('GROUND --', self.texts())
        self.assertIn('--', self.texts())

    def test_unreliable_window_flag_does_not_hide_hud_but_menu_does(self):
        self.run_lua('function isMTAWindowActive() return false end')
        self.arm()
        self.render()
        self.assertTrue(self.commands())
        self.run_lua('menuOpen=true')
        self.render()
        self.assertEqual(self.commands(), [])
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.run_lua('menuOpen=false')
        self.render()
        self.assertTrue(self.commands())
        self.run_lua("event('onClientMinimize',root)")
        self.render()
        self.assertEqual(self.commands(), [])

    def test_stale_data_and_vehicle_loss_hide_hud(self):
        self.arm()
        self.run_lua('now=now+301')
        self.render()
        self.assertEqual(self.commands(), [])
        self.access['state']['lastSample'] = self.lua.globals().now
        self.run_lua('plane.valid=false')
        self.render()
        self.assertEqual(self.commands(), [])

    def test_render_fault_does_not_stop_flight_or_recording(self):
        self.arm()
        self.run_lua("originalDraw=dxDrawLine; dxDrawLine=function() error('renderer unavailable') end; render(); render()")
        self.assertIn('renderer unavailable', self.lua.globals().ui['autopilot_hud_error'])
        self.assertEqual(self.lua.globals().ui['autopilot'], '1')
        self.assertEqual(self.lua.globals().ui['recording'], '1')
        self.assertEqual(len(self.records('autopilot_hud_error')), 1)
        self.assertEqual(self.commands(), [])
        self.run_lua("dxDrawLine=originalDraw; commands[#commands+1]='autopilot_hud:0'; step(6); commands[#commands+1]='autopilot_hud:1'; step(6)")
        self.render()
        self.assertTrue(self.commands())

    def test_geometry_is_finite_and_fits_multiple_screen_shapes(self):
        self.arm()
        self.access['state']['latest']['pitch_deg'] = -18
        self.access['state']['latest']['roll_deg'] = 58
        for width, height in ((800,600), (1280,720), (1920,1080), (2560,1080)):
            self.run_lua(f'screenW={width}; screenH={height}; projectedX=false; projectedY=false')
            self.reset_smoothing()
            self.render()
            for draw in self.commands():
                start = 0 if draw['kind']=='line' else 1
                coords = draw['args'][start:start+4]
                self.assertTrue(all(math.isfinite(c) for c in coords))
                self.assertTrue(all(0 <= c <= width for c in coords[::2]), (width,draw))
                self.assertTrue(all(0 <= c <= height for c in coords[1::2]), (height,draw))

    def test_gear_commands_and_render_cost_are_reported(self):
        self.arm()
        self.render()
        self.assertIn('GEAR  DOWN', self.texts())
        self.assertIn('GROUND', self.texts())
        self.assertIn('RUDDER', self.texts())
        self.assertIn('AILERON', self.texts())
        self.assertIn('ELEVATOR', self.texts())
        self.access['state']['latest']['landing_gear_state'] = 'up'
        self.access['state']['latest']['on_ground'] = False
        self.render()
        self.assertIn('GEAR  UP', self.texts())
        self.assertIn('AIR', self.texts())
        self.run_lua("step(24); commands[#commands+1]='stop'; step(6)")
        costs = self.records('collector_performance')
        self.assertTrue(any(r['data']['hud_frames'] > 0 for r in costs))
        self.assertTrue(all('hud_mean_ms' in r['data'] and 'hud_max_ms' in r['data'] for r in costs))

    def test_cleanup_removes_render_handler(self):
        self.arm()
        self.assertEqual(self.lua.eval('#events.onClientRender'), 1)
        self.run_lua('__DarkFlamePilotCleanup()')
        self.assertEqual(self.lua.eval('#events.onClientRender'), 0)
        self.run_lua('render()')
        self.assertEqual(self.commands(), [])


if __name__ == '__main__':
    unittest.main()
