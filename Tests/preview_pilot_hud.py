"""Render the real HUD draw commands over a synthetic backdrop; no game required."""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from pilot_hud_test import HudTests


def preview(destination, width=1280, height=800, bank=18, pitch=7, taxi=False):
    harness = HudTests()
    harness.setUp()
    harness.arm()
    lua = harness.lua
    lua.globals().screenW, lua.globals().screenH = width, height
    lua.globals().projectedX, lua.globals().projectedY = width * .56, height * .38
    item, ap = harness.access['state']['latest'], harness.access['autopilot']
    if not taxi:
        item['heading_deg'], item['pitch_deg'], item['roll_deg'] = 128, pitch, bank
        item['speed_kmh'], item['climb_mps'], item['agl_terrain_m'] = 284, 3.6, 244
        item['position_m'] = lua.table_from([0, 0, 260])
        item['on_ground'], item['landing_gear_state'] = False, 'up'
        nav = item['navigation']
        nav['heading_error_deg'], nav['distance_3d_m'], nav['altitude_error_m'] = 9.4, 1150, 80
        nav['waypoint_generation'], nav['marker_type'] = 17, 'ring'
        ap['phase'] = 'flight'
        ap['detail'] = lua.table_from(dict(goal_speed_kmh=290, goal_pitch_deg=8.5, goal_roll_deg=20))
        ap['output'] = lua.table_from(dict(throttle=.74, brake=0, rudder=.1, aileron=.15, elevator=.08))
    else:
        item['speed_kmh'], item['heading_deg'] = 12, 351
        item['navigation']['heading_error_deg'] = -28.4
        item['navigation']['distance_3d_m'] = 42
        ap['output'] = lua.table_from(dict(throttle=0, brake=.22, rudder=-.35, aileron=0, elevator=0))
    harness.render()
    canvas = Image.new('RGB', (width, height))
    background = ImageDraw.Draw(canvas)
    for y in range(height):
        f = y / height
        color = (int(27 + f*40), int(55 + f*55), int(72 + f*60)) if f < .46 else (44, 54, 52)
        background.line((0,y,width,y), fill=color)
    background.polygon([(0,height*.68),(width*.48,height*.46),(width*.52,height*.46),(width,height*.82)], fill=(49,58,59))
    overlay = Image.new('RGBA', (width,height))
    draw = ImageDraw.Draw(overlay)
    fonts = {}
    for entry in harness.commands():
        args = entry['args']
        color = int(args[4 if entry['kind']=='line' else 5])
        rgba = ((color >> 16)&255, (color >> 8)&255, color&255, (color >> 24)&255)
        if entry['kind']=='line':
            draw.line(args[:4], fill=rgba, width=max(1,round(args[5])))
        else:
            text,x,y,right,bottom,color,size,font,align = args[:9]
            pixels = max(1, round(14*size))
            if pixels not in fonts:
                fonts[pixels] = ImageFont.truetype('C:/Windows/Fonts/tahomabd.ttf', pixels)
            font = fonts[pixels]
            length = draw.textlength(text, font=font)
            if align=='center': x = (x+right-length)*.5
            elif align=='right': x = right-length
            draw.text((x,y), text, font=font, fill=rgba, anchor='lt')
    canvas = Image.alpha_composite(canvas.convert('RGBA'), overlay)
    label = ImageDraw.Draw(canvas)
    label.text((24,height-32), 'SIMULATED DATA / actual Lua HUD draw commands', fill='#95a5af', font=ImageFont.truetype('C:/Windows/Fonts/tahoma.ttf',14))
    destination.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert('RGB').save(destination)
    print(f'{destination}: {len(harness.commands())} draw calls')


if __name__=='__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    preview(args.output / 'flight-hud.png', 1920,1080)
    preview(args.output / 'taxi-hud.png', taxi=True)
    preview(args.output / 'small-hud.png', 800,600, bank=-28, pitch=-9)
