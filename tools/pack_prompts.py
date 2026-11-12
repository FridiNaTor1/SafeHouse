#!/usr/bin/env python3
"""Packs Kenney Input Prompts 1.5A (CC0) into the controller prompt atlases.

Usage: python3 -I tools/pack_prompts.py --zip kenney_input-prompts_1.5.zip
       [--out assets/prompts] [--licenses licenses/kenney-input-prompts] [--check]

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

def rasterise(shapes,k,size):
 from PIL import ImageDraw
 mask=Image.new('1',(size,size),0);draw=ImageDraw.Draw(mask)
 for op,poly in shapes:draw.polygon([(x*k,y*k) for x,y in poly],fill=1 if op=='add' else 0)
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
 small=lambda im:im.resize((64*PX_PER_UNIT,64*PX_PER_UNIT),Image.BOX)
 body=small(body);filled=small(filled)
 r=int(round(OUTLINE_UNITS*PX_PER_UNIT))
 for _ in range(r):filled=filled.filter(ImageFilter.MaxFilter(3))
 return body,filled

def pack_devices(z):
 cells=[]
 for fam,name in DEVICES:
  body,under=render_device(z,name)
  box=under.getbbox()
  x0,y0,x1,y1=max(box[0]-2,0),max(box[1]-2,0),min(box[2]+2,64*PX_PER_UNIT),min(box[3]+2,64*PX_PER_UNIT)
  cells.append((fam,body.crop((x0,y0,x1,y1)),under.crop((x0,y0,x1,y1)),x0/PX_PER_UNIT,y0/PX_PER_UNIT))
 w=max(c[1].size[0] for c in cells);H=sum(c[1].size[1]+4 for c in cells)
 atlas=Image.new('RGBA',(2*w+8,H),(255,255,255,0));y=0;rects=[]
 for fam,body,under,ux,uy in cells:
  cw,ch=body.size
  white=Image.new('RGBA',(cw,ch),(255,255,255,255))
  b=white.copy();b.putalpha(body);u=white.copy();u.putalpha(under)
  atlas.paste(b,(0,y));atlas.paste(u,(w+8,y))
  rects.append((fam,(0,y,cw,ch),(w+8,y,cw,ch),ux,uy));y+=ch+4
 return atlas,rects

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
 src.add_argument('--zip',type=pathlib.Path,help='the pinned kenney_input-prompts_1.5.zip')
 src.add_argument('--dir',type=pathlib.Path,help='an extracted copy of the same release (its top folder: "Xbox Series", ...); '
  'used when the zip cannot be downloaded. Not hash-pinned: --check against the committed atlases is the proof')
 p.add_argument('--out',type=pathlib.Path,default=root/'assets/prompts')
 p.add_argument('--licenses',type=pathlib.Path,default=root/'licenses/kenney-input-prompts')
 p.add_argument('--check',action='store_true',help='verify committed outputs instead of writing')
 a=p.parse_args()
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
 lines.append('// Controller pictures (prompts_devices.png, Sly 2\'s pause Controls page): per device the body and its')
 lines.append(f'// underlay cells, and the cell\'s top left in the Kenney SVG\'s 64 x 64 units ({PX_PER_UNIT} px per unit).')
 lines.append(f'inline constexpr int kPromptDevicePxPerUnit = {PX_PER_UNIT};')
 lines.append('inline constexpr PromptRect kPromptDevicesSize = {0, 0, %d, %d};'%datlas.size)
 lines.append(f'inline constexpr PromptDeviceRect kPromptDeviceRects[{len(drects)}] = {{')
 for fam,b,u,ux,uy in drects:
  lines.append('    {%d, {%d, %d, %d, %d}, {%d, %d, %d, %d}, %.3ff, %.3ff}, // %s'%((FAMILIES.index(fam),)+b+u+(ux,uy,fam)))
 lines.append('};')
 outputs[a.out/'prompt_atlas_rects.inc']=('\n'.join(lines)+'\n').encode()
 outputs[a.licenses/'License.txt']=licence
 stale=[]
 for path,blob in outputs.items():
  if a.check:
   if not path.is_file() or path.read_bytes()!=blob:stale.append(path)
  else:
   path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(blob)
 for path in stale:print(f'stale: {path}',file=sys.stderr)
 if a.check and not stale:print('prompt atlases are up to date')
 for path,blob in outputs.items():print(f'{hashlib.sha256(blob).hexdigest()}  {path.name}')
 return 1 if stale else 0

if __name__=='__main__':sys.exit(main())
