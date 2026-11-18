#!/usr/bin/env python3
"""Packs Kenney Input Prompts 1.5A (CC0) into the controller prompt atlases.

Usage: python3 -I tools/pack_prompts.py --zip kenney_input-prompts_1.5.zip
       [--out assets/prompts] [--licenses licenses/kenney-input-prompts] [--check]
       python3 -I tools/pack_prompts.py --vr-devices [--check]   (only the VR controller pictures; no zip needed)

Reads the icons straight from the verified zip (no extraction) and writes one
RGBA atlas per controller family, a keyboard/mouse atlas for launcher hints,
prompt_atlas_rects.inc (the C++ rect table used by platform/button_prompts.h)
and the Kenney licence. The output is byte-for-byte reproducible. --check
compares instead of writing and exits 1 when a committed file is stale.
Needs Pillow; run with -P instead of -I where Pillow is a user-site install.
"""
import argparse, hashlib, io, pathlib, sys, zipfile
from collections import deque
from PIL import Image

ZIP_SHA256='ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1'
CELL=128          # Kenney "Double" icon size
PAD=8             # transparent ring around each cell, filled by edge extrusion
STRIDE=CELL+2*PAD # 144: 7 x 3 cells fit 1024 x 512, 7 x 7 fit 1024 x 1024
COLUMNS=7
MARGIN=8          # every icon used stays 8 px inside its 128 px frame (most keep 16)

# Order == cane::input::Button (platform/controller_family.h).
BUTTONS=['South','East','West','North','LB','RB','LT','RT','Back','Start','LS','RS','LSPress','RSPress',
 'DpadUp','DpadDown','DpadLeft','DpadRight','Dpad','Guide']
# Order == cane::input::Family. Keyboard has no button icons; it uses KEYS.
FAMILIES=['keyboard','playstation','xbox','nintendo','steamdeck','steamcontroller','ps4','ps5','steamcontroller2026',
 'gamecube','switch2','wii','wiiu','metaquest','valveindex','steamframe','vivewand','pico']

def series(folder,prefix,names):
 return [None if n is None else n if '/' in n else f'{folder}/Double/{prefix}{n}.png' for n in names]
# (the Quest's Select: the left grip held + the left stick clicked)
QUEST_COMBO_L='combo:Meta Quest/Double/quest_grip_left.png|Meta Quest/Double/quest_stick_l_press.png'
DPAD=['dpad_up','dpad_down','dpad_left','dpad_right','dpad']
ICONS={
 # The Switch column is positional: GLFW A (south) is the button Nintendo labels B.
 # PlayStation Select/Start use the PS3-style "SELECT"/"START" art, which
 # matches the PS2 game's own glyphs and stays legible at footer size.
 'playstation':series('PlayStation Series','',['playstation_button_color_cross','playstation_button_color_circle',
  'playstation_button_color_square','playstation_button_color_triangle','playstation_trigger_l1','playstation_trigger_r1',
  'playstation_trigger_l2','playstation_trigger_r2','playstation3_button_select','playstation3_button_start',
  'playstation_stick_l','playstation_stick_r','playstation_button_l3','playstation_button_r3']+['playstation_'+d for d in DPAD]+[None]),
 # DualShock 4 / DualSense: the modern white-symbol face buttons and their own Share / Create and Options.
 'ps4':series('PlayStation Series','',['playstation_button_cross','playstation_button_circle',
  'playstation_button_square','playstation_button_triangle','playstation_trigger_l1','playstation_trigger_r1',
  'playstation_trigger_l2','playstation_trigger_r2','playstation4_button_share','playstation4_button_options',
  'playstation_stick_l','playstation_stick_r','playstation_button_l3','playstation_button_r3']+['playstation_'+d for d in DPAD]+[None]),
 'ps5':series('PlayStation Series','',['playstation_button_cross','playstation_button_circle',
  'playstation_button_square','playstation_button_triangle','playstation_trigger_l1','playstation_trigger_r1',
  'playstation_trigger_l2','playstation_trigger_r2','playstation5_button_create','playstation5_button_options',
  'playstation_stick_l','playstation_stick_r','playstation_button_l3','playstation_button_r3']+['playstation_'+d for d in DPAD]+[None]),
 'xbox':series('Xbox Series','xbox_',['button_color_a','button_color_b','button_color_x','button_color_y','lb','rb','lt','rt',
  'button_view','button_menu','stick_l','stick_r','stick_l_press','stick_r_press']+DPAD+['guide']),
 'nintendo':series('Nintendo Switch','switch_',['button_b','button_a','button_y','button_x','button_l','button_r','button_zl',
  'button_zr','button_minus','button_plus','stick_l','stick_r','stick_l_press','stick_r_press']+DPAD+['button_home']),
 # Steam Deck. Its View / Menu come from the 2026 controller set: the same symbols, larger.
 'steamdeck':series('Steam Deck','steamdeck_',['button_a','button_b','button_x','button_y','button_l1','button_r1','button_l2',
  'button_r2','Steam Controller/Double/controller_button_view.png','Steam Controller/Double/controller_button_options.png','stick_l','stick_r','stick_l_press','stick_r_press']+DPAD+['button_guide']),
 # The 2026 Steam Controller ("Triton"): Kenney's 2026 set (controller_button_*: L1 / R1 / L2 / R2, View, Menu) with
 # the Steam face buttons and d-pad; its two sticks use the Steam Deck's stick art (the 2026 set has none).
 'steamcontroller2026':series('Steam Controller','steam_',['button_a','button_b','button_x','button_y',
  'Steam Controller/Double/controller_button_l1.png','Steam Controller/Double/controller_button_r1.png',
  'Steam Controller/Double/controller_button_l2.png','Steam Controller/Double/controller_button_r2.png',
  'Steam Controller/Double/controller_button_view.png','Steam Controller/Double/controller_button_options.png',
  'Steam Deck/Double/steamdeck_stick_l.png','Steam Deck/Double/steamdeck_stick_r.png',
  'Steam Deck/Double/steamdeck_stick_l_press.png','Steam Deck/Double/steamdeck_stick_r_press.png']+DPAD+
  ['Steam Deck/Double/steamdeck_button_guide.png']),
 # GameCube, positional as SDL maps it: A south, X east, B west, Y north, Z the right bumper, L / R the triggers, the
 # yellow C-stick as the right stick. It has no left bumper (the Switch Online pad's ZL), Select or Home, and no stick
 # clicks: the stick art stands in for L3 / R3 (as the 2015 Steam Controller's pad does), the rest stays text.
 'gamecube':series('Nintendo Gamecube','gamecube_',['button_color_a','button_x','button_color_b','button_y',None,'button_z',
  'trigger_l','trigger_r',None,'button_start','stick','stick_color_c','stick','stick_color_c']+DPAD+[None]),
 # Switch 2 (Pro Controller 2, Joy-Con 2): Kenney's Switch 2 set, positional like the Switch's.
 'switch2':series('Nintendo Switch 2','switch_',['button_b','button_a','button_y','button_x','button_l','button_r','button_zl',
  'button_zr','button_minus','button_plus','stick_l','stick_r','stick_l_press','stick_r_press']+DPAD+['button_home']),
 # Wii Classic Controller (Pro), positional. Kenney's Wii "B" is the Remote's trigger, so the round Wii U B stands in;
 # the Classic has no stick clicks (the stick art stands in).
 'wii':series('Nintendo Wii','wii_',['Nintendo WiiU/Double/wiiu_button_b.png','button_a','button_y','button_x','button_l',
  'button_r','button_zl','button_zr','button_minus','button_plus','stick_l','stick_r','stick_l','stick_r']+DPAD+['button_home']),
 # Wii U Pro Controller, positional (Kenney has no stick-click art for it: the stick art stands in).
 'wiiu':series('Nintendo WiiU','wiiu_',['button_b','button_a','button_y','button_x','button_l','button_r','button_zl',
  'button_zr','button_minus','button_plus','stick_l','stick_r','stick_l','stick_r']+DPAD+['button_home']),
 # VR controllers, the two hands as one pad, in the VR pad layout (platform/vr_pad_map.h, the user's 2026-11-11):
 # the right hand's lower face button south (Cross), its upper one west (Square), the left grip east (Circle), the
 # right grip north (Triangle), the left hand's lower / upper face buttons the triggers' places (L2 / R2), the triggers
 # the bumpers (L1 / R1), the sticks; no d-pad (the left stick's directions stand in). Controllers without real Start /
 # Select buttons hold a grip and click that hand's stick: their cell is the two icons side by side ("combo:A|B":
 # compose_combo; the in-game text draws the two full-size icons, promptglyph.cpp).
 # Meta Quest Touch (also Touch Plus / Touch Pro): A B X Y, grips, triggers, the left Menu as Start, Meta as Guide.
 'metaquest':series('Meta Quest','quest_',['button_a','grip_left','button_b','grip_right','trigger_left','trigger_right',
  'button_x','button_y',QUEST_COMBO_L,'button_menu','stick_l','stick_r','stick_l_press','stick_r_press',
  'stick_l_up','stick_l_down','stick_l_left','stick_l_right','stick_l','button_meta']),
 # Valve Index ("Knuckles"): A / B on each hand (Kenney has one A / B pair: the left hand's are labelled by text), the
 # Quest's grip art (Kenney's Index set has none), the triggers; no Start / Select buttons: the grip + stick-click
 # combos (the Quest's grip art with the Index stick clicks).
 'valveindex':series('Valve Index','index_',['button_a','Meta Quest/Double/quest_grip_left.png','button_b',
  'Meta Quest/Double/quest_grip_right.png','lt','rt','button_a','button_b',
  'combo:Meta Quest/Double/quest_grip_left.png|Valve Index/Double/index_stick_l_press.png',
  'combo:Meta Quest/Double/quest_grip_right.png|Valve Index/Double/index_stick_r_press.png',
  'stick_l','stick_r','stick_l_press','stick_r_press','stick_l_up','stick_l_down','stick_l_left','stick_l_right',
  'stick_l','button']),
 # Steam Frame controllers: the Steam Controller's layout split in two hands (Kenney's Steam Frame set, complete).
 'steamframe':series('Steam Frame','frame_',['button_a','button_b','button_x','button_y','button_l1','button_r1',
  'button_l2','button_r2','button_view','button_options','stick_l','stick_r','stick_l_press','stick_r_press']+
  ['dpad_up','dpad_down','dpad_left','dpad_right','dpad','Steam Deck/Double/steamdeck_button_guide.png']),
 # HTC Vive wands (Kenney has none: its Index trackpad, trigger and the Quest's grip / menu art, all CC0): each wand's
 # trackpad press as its face button (right south, left west), the grips east / north, the triggers the bumpers, no
 # L2 / R2, the menu buttons Back / Start, the trackpads as the sticks and the left one's edges as the d-pad.
 'vivewand':series('Valve Index','index_',['track_press','Meta Quest/Double/quest_grip_left.png','track_press',
  'Meta Quest/Double/quest_grip_right.png','lt','rt',None,None,
  'Meta Quest/Double/quest_button_menu.png','Meta Quest/Double/quest_button_menu.png',
  'track','track','track_press','track_press','track_up','track_down',None,None,'track','button']),
 # Pico (4 / 4 Ultra / Neo 3) controllers: the Quest's layout and art (Kenney has no Pico set).
 'pico':series('Meta Quest','quest_',['button_a','grip_left','button_b','grip_right','trigger_left','trigger_right',
  'button_x','button_y',QUEST_COMBO_L,'button_menu','stick_l','stick_r','stick_l_press','stick_r_press',
  'stick_l_up','stick_l_down','stick_l_left','stick_l_right','stick_l','button_oculus']),
 # The 2015 Steam Controller: one stick plus the right trackpad as "RS".
 'steamcontroller':series('Steam Controller','steam_',['button_a','button_b','button_x','button_y','lb','rb','lt','rt',
  'button_back_icon','button_start_icon','stick','pad','stick_l_press','pad']+DPAD+['Steam Deck/Double/steamdeck_button_guide.png']),
}
# (GLFW key code, Kenney name). -100 == cane::input::kMouseLeftKey.
KEYS=[(257,'keyboard_enter'),(256,'keyboard_escape'),(258,'keyboard_tab'),(32,'keyboard_space'),(259,'keyboard_backspace'),
 (-100,'mouse_left'),(265,'keyboard_arrow_up'),(264,'keyboard_arrow_down'),(263,'keyboard_arrow_left'),(262,'keyboard_arrow_right'),
 (340,'keyboard_shift'),(341,'keyboard_ctrl'),(342,'keyboard_alt')]+[(65+i,'keyboard_'+chr(97+i)) for i in range(26)]+\
 [(48+i,f'keyboard_{i}') for i in range(10)]
# More keycaps and the mouse (Sly 2's pause Controls page lists every binding): ids below -100 are the mouse
# (cane::input::kMouse*Key), the rest GLFW key codes.
KEYS+=[(-101,'mouse_right'),(-102,'mouse_scroll'),(-103,'mouse_scroll_up'),(-104,'mouse_scroll_down'),
 (-105,'mouse_side_back'),(-106,'mouse_side_forward'),(-110,'mouse_move'),(-111,'keyboard_arrows')]+\
 [(290+i,f'keyboard_f{i+1}') for i in range(12)]+\
 [(45,'keyboard_minus'),(61,'keyboard_equals'),(91,'keyboard_bracket_open'),(93,'keyboard_bracket_close'),
 (59,'keyboard_semicolon'),(39,'keyboard_apostrophe'),(44,'keyboard_comma'),(46,'keyboard_period'),
 (47,'keyboard_slash_forward'),(92,'keyboard_slash_back'),(96,'keyboard_tilde'),(260,'keyboard_insert'),
 (261,'keyboard_delete'),(266,'keyboard_page_up'),(267,'keyboard_page_down'),(268,'keyboard_home'),(269,'keyboard_end'),
 (280,'keyboard_capslock'),(334,'keyboard_numpad_plus'),(332,'keyboard_asterisk')]
# Keys sharing another key's cap: right modifiers, keypad Enter, keypad digits / minus / period.
KEY_ALIASES=[(344,340),(345,341),(346,342),(335,257),(333,45),(330,46)]+[(320+i,48+i) for i in range(10)]

# Controller pictures for Sly 2's pause Controls page: Kenney's device silhouettes (Vector, 64 x 64 units)
# rasterised here (even-odd fill of the flattened path, 4x supersampled), PX_PER_UNIT pixels per unit. Per device
# two aligned cells: the body (white, its button holes cut out) and a filled, dilated underlay (the outline and the
# holes' backing). Order == the Family values that have a picture (keyboard and playstation have none).
# The Steam Deck and the 2026 Steam Controller are composed here (compose_*): Kenney's silhouettes leave out their
# thumbsticks (the Deck's has only the d-pad / face buttons and the trackpads), so the Controls page could not point
# "Move Sly" / "Move Camera" at them.
DEVICES=[('xbox','Xbox Series/Vector/controller_xboxseries.svg'),('nintendo','Nintendo Switch/Vector/controller_switch_pro.svg'),
 ('steamdeck','compose_steamdeck'),('steamcontroller','Steam Controller/Vector/controller_steam.svg'),
 ('ps4','PlayStation Series/Vector/controller_playstation4.svg'),('ps5','PlayStation Series/Vector/controller_playstation5.svg'),
 ('steamcontroller2026','compose_steamcontroller2026'),
 ('gamecube','Nintendo Gamecube/Vector/gamecube_controller.svg'),('switch2','Nintendo Switch 2/Vector/controller_switch_pro.svg'),
 ('wii','Nintendo Wii/Vector/controller_wii_classic_pro.svg'),('wiiu','Nintendo WiiU/Vector/controller_wiiu_pro.svg')]
PX_PER_UNIT=8
SUPER=4
OUTLINE_UNITS=0.75

def svg_subpaths(svg):
 import re
 d=re.search(r' d="([^"]*)"',svg.decode()).group(1)
 toks=re.findall(r'[MLQZmlqz]|-?\d*\.?\d+(?:e-?\d+)?',d)
 subs=[];cur=[];cmd=None;i=0;pos=(0.0,0.0)
 def num():
  nonlocal i;v=float(toks[i]);i+=1;return v
 while i<len(toks):
  t=toks[i]
  if t in 'MLQZmlqz':
   cmd=t;i+=1
   if cmd in 'Zz':
    if cur:subs.append(cur);cur=[]
    continue
  if cmd=='M':
   if cur:subs.append(cur)
   pos=(num(),num());cur=[pos];cmd='L'
  elif cmd=='L':
   pos=(num(),num());cur.append(pos)
  elif cmd=='Q':
   c=(num(),num());e=(num(),num());p0=pos
   for k in range(1,17):
    u=k/16;a=(1-u)*(1-u);b=2*(1-u)*u;cc=u*u
    cur.append((a*p0[0]+b*c[0]+cc*e[0],a*p0[1]+b*c[1]+cc*e[1]))
   pos=e
  else:sys.exit(f'unsupported path command {cmd}')
 if cur:subs.append(cur)
 return subs

# ---- composed pictures: shapes in the SVG's 64 x 64 units, applied in order ('add' fills, 'cut' clears)
def arc(cx,cy,r,a0,a1,n=12):
 import math
 return [(cx+r*math.cos(math.radians(a0+(a1-a0)*i/n)),cy+r*math.sin(math.radians(a0+(a1-a0)*i/n))) for i in range(n+1)]
def rrect(x0,y0,x1,y1,rt,rb=None):
 rb=rt if rb is None else rb
 return arc(x1-rt,y0+rt,rt,-90,0)+arc(x1-rb,y1-rb,rb,0,90)+arc(x0+rb,y1-rb,rb,90,180)+arc(x0+rt,y0+rt,rt,180,270)
def rsquare(cx,cy,h,r):return rrect(cx-h,cy-h,cx+h,cy+h,r)
def circle(cx,cy,r):return arc(cx,cy,r,0,360,32)[:-1]
def cross(cx,cy,h,w):
 return [(cx-w,cy-h),(cx+w,cy-h),(cx+w,cy-w),(cx+h,cy-w),(cx+h,cy+w),(cx+w,cy+w),(cx+w,cy+h),(cx-w,cy+h),(cx-w,cy+w),
  (cx-h,cy+w),(cx-h,cy-w),(cx-w,cy-w)]
def moved(poly,dx,dy,k=1.0,c=None):
 c=c or (0.0,0.0)
 return [(c[0]+(x-c[0])*k+dx,c[1]+(y-c[1])*k+dy) for x,y in poly]
def stick(cx,cy,r):
 # a hole with the stick's cap in it
 return [('cut',circle(cx,cy,r)),('add',circle(cx,cy,r*0.72))]
def trackpad(cx,cy,h,r):
 return [('cut',rsquare(cx,cy,h,r)),('add',rsquare(cx,cy,h*0.8,r*0.7))]

def compose_steamdeck(z):
 # After the real Deck (298 x 117 mm): on each side of the screen the d-pad (left, outer) / face buttons (right,
 # outer) and the thumbstick (inner, a little lower) on top, the square trackpad under them, View / Menu above the
 # sticks by the screen's top corners; L1 / R1 on the top edge at the ends, L2 / R2 behind them.
 shapes=[]
 for m in (1,-1):   # (left, then the mirror)
  X=lambda x:x if m==1 else 64-x
  shapes.append(('add',rrect(min(X(7.1),X(12.9)),17.6,max(X(7.1),X(12.9)),20.5,0.9)))    # L2 / R2 behind
  shapes.append(('add',rrect(min(X(5.4),X(14.6)),18.8,max(X(5.4),X(14.6)),21.5,1.2)))    # L1 / R1
 shapes.append(('add',rrect(4,20,60,44,4.5,7.5)))
 shapes.append(('cut',rrect(19,23,45,41,2)))                                              # the screen
 shapes.append(('cut',cross(8.8,26.3,3.0,1.0)))                                           # d-pad
 for x,y in ((55.2,24.3),(57.2,26.3),(53.2,26.3),(55.2,28.3)):shapes.append(('cut',circle(x,y,1.0)))  # Y B X A
 shapes+=stick(15.2,27.6,2.7)+stick(48.8,27.6,2.7)
 shapes+=trackpad(11.6,36.0,3.3,1.0)+trackpad(52.4,36.0,3.3,1.0)
 shapes.append(('cut',rrect(15.3,41.3,17.1,42.4,0.5)))                                    # Steam
 shapes.append(('cut',rrect(46.9,41.3,48.7,42.4,0.5)))                                    # Quick Access
 return shapes

def compose_steamcontroller2026(z):
 # Kenney's 2026 silhouette (body, the tilted square trackpads) with the d-pad / face buttons moved out to the top
 # corners and a thumbstick above each trackpad: the real controller's layout (the Steam Deck's controls).
 subs=svg_subpaths(z.read('Steam Controller/Vector/controller_steam_new.svg'))
 def box(sub):
  xs=[p[0] for p in sub];ys=[p[1] for p in sub];return min(xs),min(ys),max(xs),max(ys)
 body=max(subs,key=lambda s:(box(s)[2]-box(s)[0])*(box(s)[3]-box(s)[1]))
 shapes=[('add',body)]
 for sub in subs:
  if sub is body:continue
  x0,y0,x1,y1=box(sub);cx=(x0+x1)/2
  if y0>=31:shapes.append(('cut',sub))                                       # the trackpads
  elif cx<32:shapes.append(('cut',moved(sub,-5.0,0.0,0.7,(21.0,25.0))))     # the d-pad
  else:shapes.append(('cut',moved(sub,5.2,0.0,0.75,(42.0,25.0))))            # the face buttons
 shapes+=stick(25.2,26.0,2.7)+stick(38.8,26.0,2.7)
 return shapes

# ---- VR controllers (Sly 2's pause Controls page in VR): both hands side by side in one 64 x 64 picture, the left
# hand left, seen from above as they are held (the front, with the triggers, at the top). Original drawings (simple
# shapes: no Kenney set has VR controller silhouettes): the face with its stick / buttons / trackpad as holes, the
# triggers as tabs at the front edge, the grips as tabs on the handles' inner sides, a tracking ring where the real
# controller has one. Each picture's button places (VR_LAYOUTS) go into prompt_atlas_rects.inc (kPromptVrLayouts) for
# the page's icons: the PS2 buttons in the VR pad layout (platform/vr_pad_map.h): Touch layout A Cross, B Square, the
# left grip Circle, the right grip Triangle, X / Y L2 / R2, the triggers L1 / R1; Pads layout (WMR, Vive) the right /
# left trackpad clicks Cross / Square; Gamepad layout (Steam Frame) by position. The id is 64 + vr_pad_map.h's
# Controllers value.
def ellipse(cx,cy,rx,ry,rot=0.0,n=48):
 import math
 c,s=math.cos(math.radians(rot)),math.sin(math.radians(rot))
 out=[]
 for i in range(n):
  a=2*math.pi*i/n;x,y=rx*math.cos(a),ry*math.sin(a)
  out.append((cx+x*c-y*s,cy+x*s+y*c))
 return out
def hull(pts):
 pts=sorted(set((round(x,4),round(y,4)) for x,y in pts))
 def half(seq):
  h=[]
  for p in seq:
   while len(h)>=2 and (h[-1][0]-h[-2][0])*(p[1]-h[-2][1])-(h[-1][1]-h[-2][1])*(p[0]-h[-2][0])<=0:h.pop()
   h.append(p)
  return h
 lo=half(pts);hi=half(pts[::-1])
 return lo[:-1]+hi[:-1]
def capsule(x0,y0,r0,x1,y1,r1):return hull(circle(x0,y0,r0)+circle(x1,y1,r1))
def band(cx,cy,ro,ri,a0,a1,n=24):
 # a ring's arc (degrees, clockwise on screen from +x), ro / ri its outer / inner radius
 return arc(cx,cy,ro,a0,a1,n)+arc(cx,cy,ri,a1,a0,n)
def button(cx,cy,r):return [('cut',circle(cx,cy,r))]
def pad(cx,cy,r):return [('cut',circle(cx,cy,r)),('add',circle(cx,cy,r*0.8))]
def mirrored(shapes):return [(op,[(64-x,y) for x,y in poly]) for op,poly in shapes]
def mx(p):return (64-p[0],p[1])

# The hands' shapes: (left, right) lists of ('add' | 'cut', polygon); the right hand is drawn as the left one mirrored
# unless a model has its own.
def vr_touch_hand(right):
 # Meta Quest Touch (Quest 2 / 3 / 3S / Pro, Touch Plus / Pro): the round face with the stick outside and the two
 # buttons inside, the menu button (right: Meta) under the stick, the handle down and in
 s=[('add',rrect(10.5,11.0,20.5,15.6,1.8)),                 # the trigger
    ('add',rrect(23.6,28.5,28.4,38.0,1.8)),                 # the grip
    ('add',ellipse(17.0,21.5,10.4,8.6,-8)),
    ('add',capsule(19.0,27.0,5.6,22.4,41.0,4.2))]
 s+=stick(13.6,20.8,3.3)+button(21.0,25.4,1.9)+button(22.4,19.4,1.9)+button(12.4,27.4,1.1)
 return s
def vr_touch(z):return vr_touch_hand(False)+mirrored(vr_touch_hand(True))
def vr_pico_hand(right):
 # Pico 4 / Neo 3: the Touch layout, the tracking ring an open arc around the face's outer side
 s=[('add',rrect(11.5,11.0,21.0,15.4,1.8)),('add',rrect(23.4,28.5,28.2,38.0,1.8)),
    ('add',band(17.0,21.5,12.4,10.4,120,265)),
    ('add',ellipse(17.6,21.8,9.2,8.0,-4)),
    ('add',capsule(19.2,27.0,5.4,22.2,41.0,4.2))]
 s+=stick(14.4,20.6,3.2)+button(21.4,25.2,1.85)+button(22.6,19.4,1.85)+button(14.0,27.4,1.0)
 return s
def vr_pico(z):return vr_pico_hand(False)+mirrored(vr_pico_hand(True))
def vr_vivefocus_hand(right):
 # HTC Vive Focus 3 / XR Elite: a rounder face, no ring, the menu button (right: system) below the buttons
 s=[('add',rrect(11.0,11.6,20.4,15.8,1.8)),('add',rrect(23.2,28.0,28.0,37.4,1.8)),
    ('add',ellipse(17.4,21.6,9.8,9.0,0)),
    ('add',capsule(18.6,28.0,5.8,21.6,41.0,4.4))]
 s+=stick(13.8,20.4,3.3)+button(21.4,24.6,1.9)+button(21.6,18.8,1.9)+button(17.0,28.6,1.0)
 return s
def vr_vivefocus(z):return vr_vivefocus_hand(False)+mirrored(vr_vivefocus_hand(True))
def vr_vivecosmos_hand(right):
 # HTC Vive Cosmos: the Touch layout under a wide ring over the face, a bumper in front of the trigger
 s=[('add',rrect(10.0,8.6,19.0,11.6,1.2)),                  # the bumper (unused)
    ('add',rrect(11.0,11.0,20.0,15.4,1.8)),('add',rrect(23.6,29.0,28.4,38.2,1.8)),
    ('add',band(17.0,19.0,11.6,9.4,195,345)),
    ('add',ellipse(17.0,22.0,9.4,8.2,-6)),
    ('add',capsule(18.8,27.4,5.6,22.2,41.0,4.2))]
 s+=stick(13.8,21.4,3.2)+button(21.0,25.6,1.85)+button(21.8,19.8,1.85)+button(12.8,27.8,1.0)
 return s
def vr_vivecosmos(z):return vr_vivecosmos_hand(False)+mirrored(vr_vivecosmos_hand(True))
def vr_index_hand(right):
 # Valve Index ("Knuckles"): a narrow upright body, the stick on top, A / B on the inside, the long trackpad under the
 # stick, the system button below, the force-sensing grip along the handle's inside
 s=[('add',rrect(12.6,10.0,21.2,14.0,1.6)),                 # the trigger
    ('add',rrect(23.0,24.0,26.6,40.5,1.6)),                 # the grip
    ('add',ellipse(17.6,19.0,7.0,7.8,0)),
    ('add',capsule(18.0,22.0,6.0,19.8,41.5,4.6))]
 s+=stick(18.6,16.8,2.8)+button(13.4,15.4,1.45)+button(13.6,19.8,1.45)
 s+=[('cut',rrect(14.8,21.6,19.8,31.0,2.4)),('add',rrect(15.6,22.4,19.0,30.2,1.8))]   # the trackpad
 s+=button(17.6,34.0,0.9)
 return s
def vr_index(z):return vr_index_hand(False)+mirrored(vr_index_hand(True))
def vr_vive_hand(right):
 # HTC Vive wand: the open tracking ring in front, the round trackpad, the menu button above it, the system button
 # below, the trigger under the neck (shown out at the side), the grips on the handle
 s=[('add',rrect(8.4,22.4,13.0,27.2,1.4)),                  # the trigger (under the neck: shown at the side)
    ('add',rrect(21.2,34.0,24.8,42.0,1.4)),                 # the grip
    ('add',circle(17.0,14.6,9.6)),('cut',circle(17.0,14.6,6.6)),('hole',circle(17.0,14.6,6.6)),
    ('add',capsule(17.0,22.0,4.2,17.0,45.0,3.8)),
    ('add',ellipse(17.0,30.0,6.0,7.6,0))]
 s+=pad(17.0,30.0,4.0)+button(17.0,23.4,1.0)+button(17.0,37.6,0.8)
 return s
def vr_vive(z):return vr_vive_hand(False)+mirrored(vr_vive_hand(True))
def vr_wmr_hand(right,hp=False):
 # Windows Mixed Reality: the ring in front, the stick outside, the trackpad inside, the menu button under the stick;
 # HP Reverb G2 (hp=True): X / Y (right: A / B) in the trackpad's place
 s=[('add',rrect(5.0,16.8,10.4,22.2,1.4)),                  # the trigger (shown at the side)
    ('add',rrect(23.2,32.0,27.4,40.6,1.6)),                 # the grip
    ('add',circle(17.4,14.0,8.8)),('cut',circle(17.4,14.0,6.2)),('hole',circle(17.4,14.0,6.2)),
    ('add',ellipse(17.4,26.0,8.4,7.0,-6)),
    ('add',capsule(18.6,30.0,5.2,21.0,44.0,4.0))]
 if hp:s+=stick(13.4,24.6,2.8)+button(20.6,28.6,1.8)+button(21.4,23.6,1.8)+button(13.8,30.6,0.95)
 else:s+=stick(12.6,24.6,2.6)+pad(19.8,26.4,3.4)+button(13.6,30.6,0.95)
 return s
def vr_wmr(z):return vr_wmr_hand(False)+mirrored(vr_wmr_hand(True))
def vr_hp(z):return vr_wmr_hand(False,True)+mirrored(vr_wmr_hand(True,True))
def vr_frame_hand(right):
 # the Steam Frame's controllers: a gamepad in two hands, the stick on top, the d-pad (left) / A B X Y (right) under it,
 # View (left) / Menu (right) outside, the bumper on the front edge and the trigger behind it, the grips (no action)
 s=[('add',rrect(11.2,8.6,20.2,12.6,1.4)),                  # the trigger (L2 / R2)
    ('add',rrect(10.4,11.4,21.0,15.4,1.6)),                 # the bumper (L1 / R1)
    ('add',ellipse(17.0,22.0,10.2,8.4,-6)),
    ('add',capsule(19.0,27.4,5.6,22.2,41.0,4.2))]
 # (the stick inside, the d-pad / face buttons under it outside, View / Menu at the outer edge: every control reachable
 # from the outside without passing another)
 s+=stick(17.8,19.0,3.2)+button(8.6,20.4,0.95)
 if right:
  for x,y in ((14.0,23.8),(16.4,26.2),(14.0,28.6),(11.6,26.2)):s+=button(64-x,y,1.2)
 else:s.append(('cut',cross(14.0,26.2,2.9,1.0)))
 return s
def vr_frame(z):
 r=vr_frame_hand(True)
 return vr_frame_hand(False)+mirrored(r)

# Each picture's button places (SVG units; a button left out has no icon) and icon sizes, in BUTTONS order.
VR_ID=64
VR_DEVICES=[('vr_touch',VR_ID+1,vr_touch),('vr_index',VR_ID+2,vr_index),('vr_frame',VR_ID+3,vr_frame),
 ('vr_wmr',VR_ID+4,vr_wmr),('vr_vive',VR_ID+5,vr_vive),('vr_pico',VR_ID+7,vr_pico),('vr_vivefocus',VR_ID+8,vr_vivefocus),
 ('vr_vivecosmos',VR_ID+9,vr_vivecosmos),('vr_hp',VR_ID+10,vr_hp)]
def touch_layout(stick,lo,hi,menu,trig,grip,back=None,start=None,guide=None):
 """The Touch layout from the left hand's places: stick, lower / upper face button, menu, trigger, grip."""
 back=back or (stick[0]-5.0,stick[1]+13.4)
 return {'South':mx(lo),'East':grip,'West':mx(hi),'North':mx(grip),'LB':trig,'RB':mx(trig),'LT':lo,'RT':hi,
  'Back':back,'Start':start or menu,'LS':stick,'RS':mx(stick),'LSPress':stick,'RSPress':mx(stick),
  'Guide':guide or mx(menu)}
VR_LAYOUTS={
 'vr_touch':touch_layout((13.6,20.8),(21.0,25.4),(22.4,19.4),(12.4,27.4),(15.5,13.3),(26.0,33.2)),
 'vr_pico':touch_layout((14.4,20.6),(21.4,25.2),(22.6,19.4),(14.0,27.4),(16.2,13.2),(25.8,33.2)),
 'vr_vivefocus':touch_layout((13.8,20.4),(21.4,24.6),(21.6,18.8),(17.0,28.6),(15.7,13.7),(25.6,32.7),back=(11.2,30.6)),
 'vr_vivecosmos':touch_layout((13.8,21.4),(21.0,25.6),(21.8,19.8),(12.8,27.8),(15.5,13.2),(26.0,33.6)),
 # (Index: no menu buttons: Select / Start are the grip + stick-click combos, by each stick)
 'vr_index':dict(touch_layout((18.6,16.8),(13.6,19.8),(13.4,15.4),(17.6,34.0),(16.9,12.0),(24.8,32.0),
  back=(10.0,23.6),start=(54.0,23.6),guide=(17.6,34.0)),Guide=None),
 # (the HP Reverb G2: real menu buttons, Select left, Start right)
 'vr_hp':dict(touch_layout((13.4,24.6),(20.6,28.6),(21.4,23.6),(13.8,30.6),(7.6,19.4),(25.3,36.3)),
  Back=(13.8,30.6),Start=mx((13.8,30.6)),Guide=None),
 # (Pads layout: each trackpad's click its face button, the menu buttons Select / Start; no L2 / R2)
 'vr_wmr':{'South':mx((19.8,26.4)),'East':(25.3,36.3),'West':(19.8,26.4),'North':mx((25.3,36.3)),'LB':(7.6,19.4),
  'RB':mx((7.6,19.4)),'Back':(13.6,30.6),'Start':mx((13.6,30.6)),'LS':(12.6,24.6),'RS':mx((12.6,24.6)),
  'LSPress':(12.6,24.6),'RSPress':mx((12.6,24.6))},
 # (the Vive's trackpads are its sticks: their click icons sit under the pads)
 'vr_vive':{'South':mx((17.0,34.9)),'East':(23.0,38.0),'West':(17.0,34.9),'North':mx((23.0,38.0)),'LB':(10.7,24.8),
  'RB':mx((10.7,24.8)),'Back':(17.0,23.4),'Start':mx((17.0,23.4)),'LS':(17.0,30.0),'RS':mx((17.0,30.0)),
  'LSPress':(17.0,34.9),'RSPress':mx((17.0,34.9))},
 # (a standard pad by position: A B X Y on the right hand, the d-pad on the left)
 'vr_frame':{'South':mx((14.0,28.6)),'East':mx((11.6,26.2)),'West':mx((16.4,26.2)),'North':mx((14.0,23.8)),
  'LB':(15.7,13.4),'RB':mx((15.7,13.4)),'LT':(15.7,10.4),'RT':mx((15.7,10.4)),'Back':(8.6,20.4),
  'Start':mx((8.6,20.4)),'LS':(17.8,19.0),'RS':mx((17.8,19.0)),'LSPress':(17.8,19.0),'RSPress':mx((17.8,19.0)),
  'DpadUp':(14.0,24.1),'DpadDown':(14.0,28.3),'DpadLeft':(11.9,26.2),'DpadRight':(16.1,26.2),'Dpad':(14.0,26.2)},
}
# icon sizes (SVG units) by kind: face buttons, sticks, shoulders (triggers / grips / bumpers), menu buttons, the d-pad;
# a combo (Index / Touch Select: grip + stick click in one cell) a little larger
VR_SIZES={'vr_frame':(3.0,5.2,3.6,3.6,5.2)}
VR_SIZE_DEFAULT=(3.4,5.4,4.2,3.0,5.0)
VR_COMBO_SIZE=5.4
def vr_layout_rows():
 rows=[]
 for name,ident,_ in VR_DEVICES:
  lay=VR_LAYOUTS[name];face,stk,sh,menu,dpad=VR_SIZES.get(name,VR_SIZE_DEFAULT)
  pos=[];du=[]
  for b in BUTTONS:
   p=lay.get(b)
   kind=({'LS':stk,'RS':stk,'LSPress':stk,'RSPress':stk,'LB':sh,'RB':sh,'Dpad':dpad,'Back':menu,'Start':menu,
    'Guide':menu,'DpadUp':0.55*dpad,'DpadDown':0.55*dpad,'DpadLeft':0.55*dpad,'DpadRight':0.55*dpad}).get(b,face)
   # (the triggers / grips of the Touch and Pads layouts are shoulders too; L2 / R2 are the X / Y buttons there)
   if name!='vr_frame' and b in ('East','North'):kind=sh
   if name=='vr_frame' and b in ('LT','RT'):kind=sh
   if b=='Back' and name in ('vr_touch','vr_pico','vr_vivefocus','vr_vivecosmos','vr_index'):kind=VR_COMBO_SIZE
   if b=='Start' and name=='vr_index':kind=VR_COMBO_SIZE
   pos.append(p);du.append(kind if p else 0.0)
  rows.append((name,ident,pos,du))
 return rows

def rasterise(shapes,k,size):
 from PIL import ImageDraw
 mask=Image.new('1',(size,size),0);draw=ImageDraw.Draw(mask)
 for op,poly in shapes:
  if op!='hole':draw.polygon([(x*k,y*k) for x,y in poly],fill=1 if op=='add' else 0)
 return mask

def render_device(z,source):
 from PIL import ImageChops,ImageDraw,ImageFilter
 k=PX_PER_UNIT*SUPER;size=64*k
 if source.startswith('compose_'):
  mask=rasterise(globals()[source](z),k,size)
 else:
  mask=Image.new('1',(size,size),0)
  for sub in svg_subpaths(z.read(source)):
   layer=Image.new('1',(size,size),0)
   ImageDraw.Draw(layer).polygon([(x*k,y*k) for x,y in sub],fill=1)
   mask=ImageChops.logical_xor(mask,layer)
 body=mask.convert('L')
 # filled: everything not reachable from the border
 outside=body.copy();ImageDraw.floodfill(outside,(0,0),128)
 filled=outside.point(lambda v:0 if v==128 else 255)
 # (composed: 'hole' shapes are open to the background (a tracking ring's middle): out of the underlay too, its outline
 # still drawn by the dilation below)
 if source.startswith('compose_'):
  draw=ImageDraw.Draw(filled)
  for op,poly in globals()[source](z):
   if op=='hole':draw.polygon([(x*k,y*k) for x,y in poly],fill=0)
 small=lambda im:im.resize((64*PX_PER_UNIT,64*PX_PER_UNIT),Image.BOX)
 body=small(body);filled=small(filled)
 r=int(round(OUTLINE_UNITS*PX_PER_UNIT))
 for _ in range(r):filled=filled.filter(ImageFilter.MaxFilter(3))
 return body,filled

def device_cells(z,devices):
 cells=[]
 for fam,name in devices:
  body,under=render_device(z,name)
  box=under.getbbox()
  x0,y0,x1,y1=max(box[0]-2,0),max(box[1]-2,0),min(box[2]+2,64*PX_PER_UNIT),min(box[3]+2,64*PX_PER_UNIT)
  cells.append((fam,body.crop((x0,y0,x1,y1)),under.crop((x0,y0,x1,y1)),x0/PX_PER_UNIT,y0/PX_PER_UNIT))
 return cells
def layout_devices(cells):
 w=max(c[1].size[0] for c in cells);H=sum(c[1].size[1]+4 for c in cells)
 atlas=Image.new('RGBA',(2*w+8,H),(255,255,255,0));y=0;rects=[]
 for fam,body,under,ux,uy in cells:
  cw,ch=body.size
  white=Image.new('RGBA',(cw,ch),(255,255,255,255))
  b=white.copy();b.putalpha(body);u=white.copy();u.putalpha(under)
  atlas.paste(b,(0,y));atlas.paste(u,(w+8,y))
  rects.append((fam,(0,y,cw,ch),(w+8,y,cw,ch),ux,uy));y+=ch+4
 return atlas,rects
for _name,_id,_fn in VR_DEVICES:globals()['compose_'+_name]=_fn
VR_DEVICE_SOURCES=[(name,'compose_'+name) for name,_,_ in VR_DEVICES]
def pack_devices(z):
 return layout_devices(device_cells(z,DEVICES+VR_DEVICE_SOURCES))
def device_id(fam):
 for name,ident,_ in VR_DEVICES:
  if name==fam:return ident
 return FAMILIES.index(fam)
def device_lines(datlas,drects):
 lines=['// Controller pictures (prompts_devices.png, Sly 2\'s pause Controls page): per device the body and its',
  f'// underlay cells, and the cell\'s top left in the Kenney SVG\'s 64 x 64 units ({PX_PER_UNIT} px per unit). The family, or',
  f'// for the VR controllers {VR_ID} + platform/vr_pad_map.h\'s Controllers (both hands in one picture: kPromptVrLayouts).',
  f'inline constexpr int kPromptDevicePxPerUnit = {PX_PER_UNIT};',
  'inline constexpr PromptRect kPromptDevicesSize = {0, 0, %d, %d};'%datlas.size,
  f'inline constexpr PromptDeviceRect kPromptDeviceRects[{len(drects)}] = {{']
 for fam,b,u,ux,uy in drects:
  lines.append('    {%d, {%d, %d, %d, %d}, {%d, %d, %d, %d}, %.3ff, %.3ff}, // %s'%((device_id(fam),)+b+u+(ux,uy,fam)))
 lines.append('};')
 rows=vr_layout_rows()
 lines.append('// The VR pictures\' buttons (Button order): the icon\'s centre in SVG units ({-1, -1}: none) and its size.')
 lines.append(f'inline constexpr PromptVrLayout kPromptVrLayouts[{len(rows)}] = {{')
 for name,ident,pos,du in rows:
  lines.append('    {%d, {%s},'%(ident,', '.join('{%.1ff, %.1ff}'%p if p else '{-1.0f, -1.0f}' for p in pos)))
  lines.append('        {%s}}, // %s'%(', '.join('%.1ff'%d for d in du),name))
 lines.append('};')
 return lines
def vr_devices_only(out):
 """Adds the VR pictures to the committed prompts_devices.png without the Kenney zip: the other pictures' cells are
 taken back from the committed atlas (as the zip makes them), the VR ones drawn here; the result is what a full run
 makes."""
 import re
 inc=(out/'prompt_atlas_rects.inc').read_text()
 head=inc[:inc.index('// Controller pictures')]
 atlas=Image.open(out/'prompts_devices.png').convert('RGBA')
 cells=[]
 vrnames={name for name,_,_ in VR_DEVICES}
 for m in re.finditer(r'\{(\d+), \{(\d+), (\d+), (\d+), (\d+)\}, \{(\d+), (\d+), (\d+), (\d+)\}, ([\d.]+)f, ([\d.]+)f\}, // (\w+)',inc):
  fam=m.group(12)
  if fam in vrnames:continue
  bx,by,bw,bh,ux_,uy_,uw,uh=map(int,m.group(2,3,4,5,6,7,8,9))
  body=atlas.crop((bx,by,bx+bw,by+bh)).getchannel('A');under=atlas.crop((ux_,uy_,ux_+uw,uy_+uh)).getchannel('A')
  cells.append((fam,body,under,float(m.group(10)),float(m.group(11))))
 if [c[0] for c in cells]!=[f for f,_ in DEVICES]:sys.exit('prompt_atlas_rects.inc: unexpected device list')
 cells+=device_cells(None,VR_DEVICE_SOURCES)
 if max(c[1].size[0] for c in cells[len(DEVICES):])>max(c[1].size[0] for c in cells[:len(DEVICES)]):
  sys.exit('a VR picture is wider than the others: the atlas would change width')
 datlas,drects=layout_devices(cells)
 return {out/'prompts_devices.png':png(datlas),out/'prompt_atlas_rects.inc':(head+'\n'.join(device_lines(datlas,drects))+'\n').encode()}

SHRINK={f'Keyboard & Mouse/Double/{n}.png' for n in ('mouse_scroll_up','mouse_scroll_down','mouse_scroll','mouse_side_back','mouse_side_forward','mouse_move','mouse_right','keyboard_arrows')}

# (the VR sets' stick / trackpad direction arrows reach the cell's edge too: scaled the same way)
SHRINK_FOLDERS=('Meta Quest/','Valve Index/','Steam Frame/')

def bleed(icon):
 """Gives every fully transparent pixel the colour of its nearest visible
 pixel (multi-source BFS, deterministic). Kenney's transparent pixels are
 green-grey, which would fringe under straight-alpha GL_LINEAR + mipmaps."""
 w,h=icon.size;px=bytearray(icon.tobytes());seen=bytearray(w*h);queue=deque()
 for i in range(w*h):
  if px[i*4+3]:seen[i]=1;queue.append(i)
 if not queue:return icon
 while queue:
  i=queue.popleft();x,y=i%w,i//w
  for nx,ny in ((x+1,y),(x-1,y),(x,y+1),(x,y-1)):
   if 0<=nx<w and 0<=ny<h and not seen[ny*w+nx]:
    j=ny*w+nx;seen[j]=1;px[j*4:j*4+3]=px[i*4:i*4+3];queue.append(j)
 return Image.frombytes('RGBA',(w,h),bytes(px))

def place(atlas,icon,index):
 """Pastes the icon into cell `index` and extrudes its edge colours into the
 PAD ring with alpha 0. Returns the icon's content rect (x, y, w, h)."""
 x0,y0=(index%COLUMNS)*STRIDE+PAD,(index//COLUMNS)*STRIDE+PAD
 atlas.paste(icon,(x0,y0))
 src=icon.load();dst=atlas.load()
 for dy in range(-PAD,CELL+PAD):
  for dx in range(-PAD,CELL+PAD):
   if 0<=dx<CELL and 0<=dy<CELL:continue
   r,g,b,_=src[min(max(dx,0),CELL-1),min(max(dy,0),CELL-1)]
   dst[x0+dx,y0+dy]=(r,g,b,0)
 return (x0+MARGIN,y0+MARGIN,CELL-2*MARGIN,CELL-2*MARGIN)

def compose_combo(z,name):
 """'combo:A|B': icons A and B side by side in one cell (a button held + another pressed), each scaled to fit half
 the cell's width inside the margin, centred down; their edges may touch."""
 a,b=name[len('combo:'):].split('|')
 half=(CELL-2*MARGIN)//2+4
 cell=Image.new('RGBA',(CELL,CELL),(0,0,0,0))
 for i,part in enumerate((a,b)):
  icon=load(z,part)
  box=icon.getbbox() or (0,0,CELL,CELL)
  icon=icon.crop(box)
  k=min(half/icon.size[0],(CELL-2*MARGIN)/icon.size[1])
  w,h=max(1,round(icon.size[0]*k)),max(1,round(icon.size[1]*k))
  icon=icon.resize((w,h),Image.LANCZOS)
  x=MARGIN+(half-w)//2 if i==0 else CELL-MARGIN-half+(half-w)//2
  layer=Image.new('RGBA',(CELL,CELL),(0,0,0,0));layer.paste(icon,(x,(CELL-h)//2))
  cell=Image.alpha_composite(cell,layer)
 return bleed(cell)

def load(z,name):
 if name.startswith('combo:'):return compose_combo(z,name)
 icon=Image.open(io.BytesIO(z.read(name))).convert('RGBA')
 if icon.size!=(CELL,CELL):sys.exit(f'{name}: expected {CELL}x{CELL}, got {icon.size}')
 box=icon.getbbox()
 if box and (box[0]<MARGIN or box[1]<MARGIN or box[2]>CELL-MARGIN or box[3]>CELL-MARGIN):
  if name not in SHRINK and not name.startswith(SHRINK_FOLDERS):sys.exit(f'{name}: artwork {box} exceeds the {MARGIN} px margin')
  # (a few mouse icons reach the cell's edge: scaled down about the cell's centre to fit the margin)
  c=CELL/2;k=min((c-MARGIN)/max(c-box[0],box[2]-c,1),(c-MARGIN)/max(c-box[1],box[3]-c,1))
  n=int(CELL*k)//2*2;small=icon.resize((n,n),Image.LANCZOS)
  icon=Image.new('RGBA',(CELL,CELL),(0,0,0,0));icon.paste(small,((CELL-n)//2,(CELL-n)//2))
 return bleed(icon)

def png(image):
 out=io.BytesIO();image.save(out,'PNG',optimize=False,compress_level=9);return out.getvalue()

def fnv1a64(data):
 value=0xcbf29ce484222325
 for byte in data:value=((value^byte)*0x100000001b3)&0xffffffffffffffff
 return value

def pack(z,names,height):
 atlas=Image.new('RGBA',(1024,height),(0,0,0,0));rects={};cells={}
 for name in names:
  if name not in cells:
   cells[name]=len(cells)
   if (cells[name]//COLUMNS+1)*STRIDE>height:sys.exit('atlas overflow')
   rects[name]=place(atlas,load(z,name),cells[name])
 return atlas,rects

class DirSource:
 """zipfile.ZipFile.read() over an extracted folder."""
 def __init__(self,root):self.root=root
 def read(self,name):
  path=self.root/name
  if not path.is_file():sys.exit(f'{path}: missing')
  return path.read_bytes()

def main():
 p=argparse.ArgumentParser(description=__doc__.splitlines()[0])
 root=pathlib.Path(__file__).resolve().parent.parent
 src=p.add_mutually_exclusive_group(required=True)
 src.add_argument('--vr-devices',action='store_true',help='only (re)draw the VR controller pictures into the committed '
  'prompts_devices.png / prompt_atlas_rects.inc (no zip needed; the same result as a full run)')
 src.add_argument('--zip',type=pathlib.Path,help='the pinned kenney_input-prompts_1.5.zip')
 src.add_argument('--dir',type=pathlib.Path,help='an extracted copy of the same release (its top folder: "Xbox Series", ...); '
  'used when the zip cannot be downloaded. Not hash-pinned: --check against the committed atlases is the proof')
 p.add_argument('--out',type=pathlib.Path,default=root/'assets/prompts')
 p.add_argument('--licenses',type=pathlib.Path,default=root/'licenses/kenney-input-prompts')
 p.add_argument('--check',action='store_true',help='verify committed outputs instead of writing')
 a=p.parse_args()
 if a.vr_devices:return write_outputs(vr_devices_only(a.out),a.check)
 if a.zip:
  data=a.zip.read_bytes()
  digest=hashlib.sha256(data).hexdigest()
  if digest!=ZIP_SHA256:sys.exit(f'{a.zip}: sha256 {digest} does not match the pinned Kenney 1.5 release {ZIP_SHA256}')
  z=zipfile.ZipFile(io.BytesIO(data))
  licence=z.read('License.txt')
 else:
  z=DirSource(a.dir)
  # (mirrors usually leave out License.txt: keep the committed one)
  licence=z.read('License.txt') if (a.dir/'License.txt').is_file() else (a.licenses/'License.txt').read_bytes()
 if b'Creative Commons Zero, CC0' not in licence or b'Input Prompts (1.5' not in licence:sys.exit('unexpected License.txt')
 outputs={}
 table=[]
 sizes=[]
 hashes=[]
 for family in FAMILIES:
  if family=='keyboard':
   atlas,rects=pack(z,[f'Keyboard & Mouse/Double/{n}.png' for _,n in KEYS],2048)
   keyrects={code:rects[f'Keyboard & Mouse/Double/{n}.png'] for code,n in KEYS}
   for alias,code in KEY_ALIASES:keyrects[alias]=keyrects[code]
   table.append([None]*len(BUTTONS))
  else:
   names=ICONS[family]
   assert len(names)==len(BUTTONS),family
   atlas,rects=pack(z,[n for n in names if n],512)
   table.append([rects[n] if n else None for n in names])
  blob=png(atlas)
  outputs[a.out/f'prompts_{family}.png']=blob
  sizes.append(atlas.size);hashes.append((len(blob),fnv1a64(blob)))
 lines=['// Generated by tools/pack_prompts.py from Kenney Input Prompts 1.5A (CC0). Do not edit.',
  f'// Source zip sha256 {ZIP_SHA256}.',
  '// Rects are {x, y, w, h} in pixels of the unflipped atlas; w == 0 means no icon.',
  '// Indexed by cane::input::Family and cane::input::Button.',
  f'inline constexpr int kPromptAtlasCount = {len(FAMILIES)};',
  'inline constexpr PromptRect kPromptAtlasSize[kPromptAtlasCount] = {'+', '.join(f'{{0, 0, {w}, {h}}}' for w,h in sizes)+'};',
  '// Embedded PNG size and FNV-1a 64 of its bytes (stale-table check).',
  'inline constexpr PromptBlobId kPromptAtlasBlob[kPromptAtlasCount] = {'+', '.join(f'{{{n}u, 0x{f:016x}ull}}' for n,f in hashes)+'};',
  f'inline constexpr PromptRect kPromptRects[kPromptAtlasCount][{len(BUTTONS)}] = {{']
 for family,row in zip(FAMILIES,table):
  lines.append(f'    {{ // {family}: '+' '.join(BUTTONS))
  lines.append('        '+', '.join('{0, 0, 0, 0}' if r is None else '{%d, %d, %d, %d}'%r for r in row)+' },')
 lines.append('};')
 lines.append('// GLFW key code -> keycap rect in the keyboard atlas (index 0).')
 lines.append(f'inline constexpr PromptKeyRect kPromptKeyRects[{len(keyrects)}] = {{')
 for code in sorted(keyrects):lines.append('    {%d, {%d, %d, %d, %d}},'%((code,)+keyrects[code]))
 lines.append('};')
 datlas,drects=pack_devices(z)
 dblob=png(datlas)
 outputs[a.out/'prompts_devices.png']=dblob
 lines+=device_lines(datlas,drects)
 outputs[a.out/'prompt_atlas_rects.inc']=('\n'.join(lines)+'\n').encode()
 outputs[a.licenses/'License.txt']=licence
 return write_outputs(outputs,a.check)

def write_outputs(outputs,check):
 stale=[]
 for path,blob in outputs.items():
  if check:
   if not path.is_file() or path.read_bytes()!=blob:stale.append(path)
  else:
   path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(blob)
 for path in stale:print(f'stale: {path}',file=sys.stderr)
 if check and not stale:print('prompt atlases are up to date')
 for path,blob in outputs.items():print(f'{hashlib.sha256(blob).hexdigest()}  {path.name}')
 return 1 if stale else 0

if __name__=='__main__':sys.exit(main())
