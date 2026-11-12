"""Native menu mouse acceptance using private window-logical pointer commands.

No desktop input, window activation or direct native-widget activation is used.
The pointer file and real SDL events share the application adapter. Original
asset inventory and runtime-observed coverage are reported separately.
"""
from test_native_frontend import Menus,sha
from native_mouse_inventory import original_inventory
from pathlib import Path
from types import SimpleNamespace
import argparse,json,math,os,time,traceback

NATIVE_STATE='native-menu-mouse-state.json'
INPUT_STATE='native-mouse-input-state.json'
COMMAND_FILE='mouse-command.txt'


def ident(value):
 return int(value,0) if isinstance(value,str) else int(value)


def public_menu(state):
 return {key:state.get(key) for key in ('scene','native_root','native_focus','page','focus','editing','active','host','lobby','pause','results_root','local_slot','wait_release')}


def action_fingerprint(state):
 return tuple(state.get(key) for key in ('scene','native_root','page','editing','active','lobby','pause','results_root'))


class MouseMenus(Menus):
 def __init__(self,*args):super().__init__(*args);self.mouse_cache={};self.ack_cache={};self.carousel_cache={}
 def copied_json(self,role,name,cache):
  try:cache[role]=json.loads((self.folder/role/name).read_text(encoding='utf8'))
  except (OSError,ValueError):pass
  return cache.get(role,{})
 def mouse(self,role='host'):return self.copied_json(role,NATIVE_STATE,self.mouse_cache)
 def mouse_ack(self,role='host'):return self.copied_json(role,INPUT_STATE,self.ack_cache)
 def carousel(self,role='host'):return self.copied_json(role,'native-carousel-mouse-state.json',self.carousel_cache)
 def wait(self,predicate,timeout=45):
  until=time.monotonic()+timeout
  while not predicate():
   self.inspect()
   for role,process in self.processes.items():
    if process.poll() is not None:raise RuntimeError(role+' exited '+str(process.returncode))
    if '[port] Stopped:' in self.log(role):raise RuntimeError(role+' guest stopped')
   if time.monotonic()>until:raise TimeoutError(json.dumps({role:{'menu':public_menu(self.state(role)),'mouse':self.mouse(role),'ack':self.mouse_ack(role)} for role in self.processes}))
   self.delay(.05)


class Pointer:
 def __init__(self,menus,report):
  self.m=menus;self.report=report;self.sequences={};self.seen_groups=set();self.seen_types=set();self.seen_widgets=set();self.exercised_widgets=set()
 def observe(self,role='host'):
  state=self.m.mouse(role)
  for surface in state.get('surfaces',[]):
   root=ident(surface['root']);self.seen_groups.add(root)
   for widget in surface.get('widgets',[]):
    self.seen_types.add(ident(widget['type']));self.seen_widgets.add((root,str(widget.get('id',widget.get('control')))))
  return state
 def ack(self,role='host'):return self.m.mouse_ack(role)
 def command(self,role,action):
  before_native=self.m.mouse(role);before_ack=self.ack(role)
  seq=self.sequences.get(role,0)+1;self.sequences[role]=seq
  path=self.m.folder/role/COMMAND_FILE;pending=path.with_suffix('.pending');pending.write_text(str(seq)+' '+action+'\n',encoding='ascii')
  # Windows can deny replacement during the consumer's brief ifstream read.
  # Retry transport publication only; never repeat a native pointer intent.
  deadline=time.monotonic()+2;retries=0
  while True:
   try:os.replace(pending,path);break
   except PermissionError:
    if time.monotonic()>=deadline:raise
    retries+=1;self.m.delay(.02)
  if retries:self.report.setdefault('command_file_retries',[]).append({'role':role,'sequence':seq,'retries':retries})
  self.m.wait(lambda:self.ack(role).get('accepted_sequence',0)>=seq,10);self.m.delay(.08)
  native=self.observe(role);ack=self.ack(role)
  def numeric_state(value):return {key:value.get(key) for key in ('frame','last_sequence','surface_generation','geometry_generation','active','has_surface','hover','dragging','activations','cancel_events','stale_events','misses','rejected')}
  point=None;tokens=action.split()
  if tokens and tokens[0] in ('move','down','up','wheel'):
   try:point=list(map(float,tokens[-2:]))
   except ValueError:pass
  matches=[];rect=before_ack.get('image_rect',[])
  if point and len(rect)==4 and rect[2]>0 and rect[3]>0:
   uv=[(point[0]-rect[0])/rect[2],(point[1]-rect[1])/rect[3]]
   for surface in before_native.get('surfaces',[]):
    for widget in surface.get('widgets',[]):
     bounds=widget.get('bounds',[])
     if len(bounds)==4 and bounds[0]<=uv[0]<bounds[2] and bounds[1]<=uv[1]<bounds[3]:
      matches.append({'gui':surface['gui'],'root':surface['root'],**{key:widget.get(key) for key in ('node','id','type','option','event','control','bounds','track_bounds','selectable','allowed','value','min','max')}})
  record={'sequence':seq,'action':action,'role':role,'window_point':point,'monotonic_seconds':time.monotonic(),'ack':{key:ack.get(key) for key in ('accepted_sequence','queued_sequence','image_rect','image_sequence','view_generation','surface_generation','geometry_generation','active','presentation','fallback_buttons','fallback_sequence','captured')},'native_before':numeric_state(before_native),'native_after':numeric_state(native),'matching_bounds_before':matches}
  with (self.m.folder/role/'pointer-events.jsonl').open('a',encoding='utf8') as out:out.write(json.dumps(record,separators=(',',':'))+'\n')
  return seq
 def rect(self,role='host'):
  self.m.wait(lambda:len(self.ack(role).get('image_rect',[]))==4 and self.ack(role)['image_rect'][2]>0 and self.ack(role)['image_rect'][3]>0,15)
  return list(map(float,self.ack(role)['image_rect']))
 def point(self,uv,role='host'):
  x,y,w,h=self.rect(role);return x+float(uv[0])*w,y+float(uv[1])*h
 def move(self,uv,role='host'):
  x,y=self.point(uv,role);return self.command(role,'move '+str(x)+' '+str(y))
 def button(self,down,uv,role='host',button='left'):
  x,y=self.point(uv,role);return self.command(role,('down ' if down else 'up ')+button+' '+str(x)+' '+str(y))
 def click_uv(self,uv,role='host',button='left',hold=.06):
  self.move(uv,role);self.button(True,uv,role,button);self.m.delay(hold);self.button(False,uv,role,button);self.m.delay(.25)
 def wheel(self,uv,amount,role='host'):
  x,y=self.point(uv,role);self.command(role,'wheel 0 '+str(amount)+' '+str(x)+' '+str(y));self.m.delay(.2)
 def cancel(self,role='host'):self.command(role,'cancel');self.m.delay(.15)
 def widget(self,wanted,role='host',root=None,allowed=True,port=None):
  matches=[]
  for surface in self.observe(role).get('surfaces',[]):
   if root is not None and ident(surface['root'])!=ident(root):continue
   if port is not None and ident(surface.get('port',-1))!=ident(port):continue
   for widget in surface.get('widgets',[]):
    by_control=isinstance(wanted,str) and not wanted.startswith('0x')
    if not by_control and widget.get('control','') not in ('','selector.value'):continue
    match=widget.get('control')==wanted if by_control else ident(widget['id'])==ident(wanted)
    if not match:continue
    bounds=widget.get('bounds',[])
    if len(bounds)!=4 or not all(math.isfinite(float(v)) for v in bounds) or bounds[2]<=bounds[0] or bounds[3]<=bounds[1]:continue
    if allowed and not (widget.get('selectable') and widget.get('allowed')):continue
    matches.append({**widget,'gui':surface['gui'],'root':surface['root'],'port':surface.get('port')})
  # Enum controls expose one measured label per option. Wheel the current
  # option, rather than confusing their shared descriptor ID with duplicates.
  if len(matches)>1 and len({(v['gui'],v['root'],v['id']) for v in matches})==1 and all(ident(v['type'])==3 for v in matches):
   matches=[v for v in matches if v.get('option')==v.get('value')]
  if len(matches)!=1:raise AssertionError('Expected one hittable widget '+str(wanted)+'; matches='+str(matches)+' state='+json.dumps(self.m.mouse(role)))
  return matches[0]
 @staticmethod
 def center(widget):
  a,b,c,d=map(float,widget['bounds']);left,right=max(0,a),min(1,c);top,bottom=max(0,b),min(1,d)
  if right<=left or bottom<=top:raise AssertionError('Widget is outside the displayed game image')
  return [(left+right)*.5,(top+bottom)*.5]
 def hover(self,wanted,role='host',root=None,port=None):
  widget=self.widget(wanted,role,root,port=port);self.move(self.center(widget),role)
  def matched():
   hover=self.observe(role).get('hover') or {}
   return str(hover.get('id'))==str(widget['id']) and hover.get('gui')==widget['gui']
  self.m.wait(matched,8)
  self.report['steps'].append({'action':'hover','role':role,'id':widget['id'],'root':widget['root'],'port':widget['port'],'menu':public_menu(self.m.state(role))})
  return widget
 def click(self,wanted,role='host',root=None,hold=.06,port=None):
  widget=self.hover(wanted,role,root,port);before=public_menu(self.m.state(role));self.click_uv(self.center(widget),role,hold=hold)
  self.exercised_widgets.add((ident(widget['root']),str(widget['id'])))
  self.report['steps'].append({'action':'click','role':role,'id':widget['id'],'root':widget['root'],'before':before,'after':public_menu(self.m.state(role))})
  return widget
 def back(self,role='host'):
  self.click_uv([.5,.5],role,'right');self.report['steps'].append({'action':'right_click_back','role':role,'menu':public_menu(self.m.state(role))})
 def expect_root(self,root,role='host'):
  self.m.wait(lambda:any(ident(s['root'])==ident(root) for s in self.observe(role).get('surfaces',[])),12)
 def no_action(self,uv,role='host',label='blank'):
  before=action_fingerprint(self.m.state(role));self.click_uv(uv,role);after=action_fingerprint(self.m.state(role));assert after==before,(label,before,after)
  self.report['steps'].append({'action':'no_action','role':role,'label':label,'uv':uv,'menu':public_menu(self.m.state(role))})
 def blank(self,role='host'):
  widgets=[w for s in self.observe(role).get('surfaces',[]) for w in s.get('widgets',[]) if w.get('selectable') and w.get('allowed')]
  for uv in ([.04,.04],[.96,.04],[.04,.96],[.96,.96],[.5,.96],[.96,.5]):
   if not any(w['bounds'][0]<=uv[0]<=w['bounds'][2] and w['bounds'][1]<=uv[1]<=w['bounds'][3] for w in widgets):self.no_action(uv,role);return
  raise AssertionError('No genuinely blank test point available')
 def outside(self,role='host'):
  # Full-image UV outside[0,1] exercises the same adapter rejection as bars.
  self.no_action([-.05,.5],role,'outside_image');self.no_action([1.05,.5],role,'outside_image')
 def disabled(self,role='host'):
  candidates=[w for s in self.observe(role).get('surfaces',[]) for w in s.get('widgets',[]) if not (w.get('selectable') and w.get('allowed')) and len(w.get('bounds',[]))==4]
  for widget in candidates:
   try:uv=self.center(widget)
   except AssertionError:continue
   if widget['bounds'][2]-widget['bounds'][0]<.8 and widget['bounds'][3]-widget['bounds'][1]<.8:
    self.no_action(uv,role,'disabled_'+str(widget.get('id')));return True
  return False
 def slider_value(self,wanted,role='host'):
  widget=self.widget(wanted,role);values=[widget.get(key) for key in ('value','min','max')]
  if not all(isinstance(v,(int,float)) and math.isfinite(v) for v in values) or values[2]<=values[1]:raise AssertionError('Missing actual native slider values '+str(widget))
  return widget,values
 def slider(self,wanted,role='host'):
  widget,values=self.slider_value(wanted,role);a,b,c,d=map(float,widget.get('track_bounds',widget['bounds']));x,y=(a+c)*.5,(b+d)*.5
  horizontal=widget.get('axis','horizontal')=='horizontal'
  low=[a+.25*(c-a),y] if horizontal else [x,b+.25*(d-b)]
  high=[a+.75*(c-a),y] if horizontal else [x,b+.75*(d-b)]
  outside_low=[a-.2,y] if horizontal else [x,b-.2]
  outside_high=[c+.2,y] if horizontal else [x,d+.2]
  self.click_uv(low,role);_,vlow=self.slider_value(wanted,role)
  self.click_uv(high,role);_,vhigh=self.slider_value(wanted,role)
  assert vhigh[0]>vlow[0],('Slider track did not change native value',wanted,vlow,vhigh)
  expected=[values[1]+fraction*(values[2]-values[1]) for fraction in (.25,.75)]
  quantization=None
  integer_bindings={0xf5:('SystemSetup.SFXVolume',[0,10]),0xf8:('SystemSetup.MusicVolume',[0,10]),0xfb:('SystemSetup.SpeechVolume',[0,10]),0x11f:('HorizontalLookSpeed',[1,10]),0x122:('VerticalLookSpeed',[1,10])}
  if ident(wanted) in integer_bindings:
   # These five original controls commit through paired type2 numeric/config
   # kind1 integer bindings. Existing native RAM confirms their exact ranges;
   # the original conversion truncates positive values, without rounding.
   binding,bounds=integer_bindings[ident(wanted)]
   assert values[1:]==bounds,('Unexpected original native integer range',binding,wanted,values)
   expected=[math.floor(value+1e-6) for value in expected];tolerance=1e-4;quantization={'binding':binding,'kind':1,'conversion':'truncate_positive','quantum':1,'range':bounds}
  else:tolerance=max(1e-4,(values[2]-values[1])*.04)
  assert abs(vlow[0]-expected[0])<=tolerance and abs(vhigh[0]-expected[1])<=tolerance,('Native value does not match full drawn track',wanted,widget,expected,vlow,vhigh)
  # Hold capture outside the widget and outside the displayed image, then
  # release. Value must clamp while a release can never activate another row.
  self.button(True,high,role);self.move(outside_low,role);self.m.delay(.15);_,vmin=self.slider_value(wanted,role)
  self.move(outside_high,role);self.m.delay(.15);_,vmax=self.slider_value(wanted,role);self.button(False,outside_high,role)
  epsilon=max(1e-4,(values[2]-values[1])*.04)
  assert abs(vmin[0]-values[1])<=epsilon and abs(vmax[0]-values[2])<=epsilon,('Slider drag failed clamp',wanted,vmin,vmax)
  mouse=self.observe(role);assert not mouse.get('dragging'),('Stuck slider capture',mouse)
  self.exercised_widgets.add((ident(widget['root']),str(widget['id'])))
  self.report['steps'].append({'action':'slider_track_drag_release','role':role,'id':widget['id'],'root':widget['root'],'low':vlow[0],'high':vhigh[0],'outside_min':vmin[0],'outside_max':vmax[0],'expected_low_high':expected,'native_quantization':quantization})
  self.button(True,high,role);self.cancel(role);_,before=self.slider_value(wanted,role);self.move(low,role);self.m.delay(.15);_,after=self.slider_value(wanted,role)
  assert after[0]==before[0],('Canceled drag still changes native value',wanted,before,after)
  self.report['steps'].append({'action':'slider_cancel','id':widget['id'],'role':role})
 def selector(self,wanted,role='host'):
  widget=self.hover(wanted,role);before=widget.get('value');uv=self.center(widget)
  if before is None:raise AssertionError('Missing actual selector value '+str(widget))
  self.wheel(uv,1,role);after=self.widget(wanted,role).get('value')
  if after==before:self.wheel(uv,-1,role);after=self.widget(wanted,role).get('value')
  assert after!=before,('Wheel did not change native selector',wanted,before,after)
  self.exercised_widgets.add((ident(widget['root']),str(widget['id'])))
  self.report['steps'].append({'action':'selector_wheel','role':role,'id':widget['id'],'before':before,'after':after})
 def save_coverage(self,inventory):
  original={g['id'] for g in inventory['groups']}
  self.report['coverage']={'original_groups':len(original),'seen_original_groups':sorted(self.seen_groups&original),'unseen_original_groups':sorted(original-self.seen_groups),'runtime_seen_types':sorted(self.seen_types),'seen_widget_count':len(self.seen_widgets),'exercised_widgets':[{'root':r,'id':i} for r,i in sorted(self.exercised_widgets)],'all_original_groups_observed':original<=self.seen_groups,'scope':'Only recorded native surfaces and successful interaction assertions count. Original inventory is not itself proof of coverage.'}


def main_smoke(p):
 p.expect_root(0x3d)
 # Current native keyboard focus starts at a different row; real coordinates
 # must choose Options rather than blindly selecting that existing focus.
 initial=p.m.state().get('native_focus');assert initial!=0x41,('Need different initial focus for hit proof',initial)
 for item in (0x3e,0x3f,0x40,0x41,0x42):
  widget=p.widget(item,root=0x3d,allowed=False)
  if widget.get('selectable') and widget.get('allowed'):p.hover(item,root=0x3d)
  else:p.no_action(p.center(widget),label='native_noninteractive_'+hex(item))
 # Original callback-aware selectability can differ from descriptor flags.
 # The current native runtime decides which rows are interactive.
 p.blank();p.outside();p.hover(0x3f,root=0x3d)
 target=p.widget(0x41,root=0x3d);uv=p.center(target);before=p.observe();focus_before=p.m.state().get('native_focus')
 assert focus_before==0x3f,public_menu(p.m.state())
 # A real click can focus a new row on Down without any settled hover first.
 # Holding across native frames catches footer replacement cancelling Up.
 p.button(True,uv);p.m.delay(.8)
 assert p.m.state().get('native_root')==0x3d,('Click activated before release',public_menu(p.m.state()))
 p.button(False,uv);p.expect_root(0xf0);p.m.delay(.3)
 assert p.m.state().get('native_root')==0xf0,public_menu(p.m.state())
 assert p.observe().get('activations',0)==before.get('activations',0)+1,('Click must activate exactly once',before,p.observe())
 p.exercised_widgets.add((0x3d,str(target['id'])))
 p.report['steps'].append({'action':'direct_down_unfocused_row_hold_release','initial_focus':focus_before,'clicked':0x41,'opened':0xf0,'settled_target_hover':False,'activations':1,'surface_generation_before':before.get('surface_generation'),'surface_generation_after':p.observe().get('surface_generation')})


def option_tests(p):
 p.expect_root(0xf0);p.m.delay(.6)
 for item in (0xf5,0xf8,0xfb):p.slider(item)
 p.selector(0xfd);p.selector(0xff);p.blank();p.outside();p.back();p.expect_root(0x3d)


def return_to_main(p,role='host'):
 for _ in range(10):
  state=p.m.state(role)
  if state.get('native_root')==0x3d and not state.get('active'):p.expect_root(0x3d,role);return
  if state.get('native_root')==0xdd:
   p.m.wait(lambda:p.m.state(role).get('native_root')!=0xdd,20);continue
  p.back(role);p.m.delay(.4)
 p.expect_root(0x3d,role)


def select_original_profile(p,footer=False):
 p.expect_root(0xaf);p.m.delay(.6);p.m.press('host','UISHOT')
 # Confirm the currently selected native profile. No profile ID/configuration
 # is written by the fixture; the retained original enum owns that choice.
 if footer:p.hover(0xb1,root=0xaf);p.click('footer.confirm',root=0xaf)
 else:p.click(0xb1,root=0xaf)


def profile_tests(p):
 p.expect_root(0x3d);p.click(0x40,root=0x3d);select_original_profile(p);p.expect_root(0x123);p.m.delay(.6)
 p.m.press('host','UISHOT');p.click(0x125,root=0x123);p.expect_root(0x115);p.m.delay(.6);p.m.press('host','UISHOT')
 for item in (0x118,0x11a,0x11c):p.selector(item)
 for item in (0x11f,0x122):p.slider(item)
 p.back();p.expect_root(0x123);p.click(0x126,root=0x123);p.expect_root(0x10a);p.m.delay(.6);p.m.press('host','UISHOT')
 for item in (0x10d,0x10f,0x111,0x114):p.selector(item)
 p.back();p.expect_root(0x123);p.click(0x128,root=0x123);p.expect_root(0x100);p.m.delay(.6);p.m.press('host','UISHOT');p.selector(0x102)
 p.back();p.expect_root(0x123);p.click(0x127,root=0x123);p.expect_root(0);p.m.delay(.6);p.m.press('host','UISHOT')
 before=p.observe().get('activations',0)
 for item in (3,4,0x27,0x28,0x2b,5,0x2c,6):p.click(item,root=0)
 assert p.observe().get('activations',0)>=before+8,('Original keyboard click intents did not dispatch',p.observe())
 p.m.press('host','UISHOT');p.click(0x30,root=0);p.expect_root(0x123)
 p.report['steps'].append({'action':'original_profile_controls_and_keyboard','groups':[0xaf,0x123,0x115,0x10a,0x100,0],'keyboard_controls':['letter','delete','space','shift','caps','done'],'scope':'Pointer clicks invoke original keyboard callbacks on the private card; edited text contents are not read back by this diagnostic.'})
 return_to_main(p)


def carousel_surface(p):
 surfaces=p.m.carousel().get('surfaces',[])
 if len(surfaces)!=1:raise AssertionError('Expected one copied original carousel state '+str(surfaces))
 return surfaces[0]


def carousel_tests(p):
 p.expect_root(0x3d);p.click(0x3e,root=0x3d);select_original_profile(p,footer=True)
 # A profile containing an original resume checkpoint may start that campaign
 # instead. This test requires the actual authored level menu, never forces it.
 p.expect_root(0x4b);p.m.wait(lambda:len(p.m.carousel().get('surfaces',[]))==1,10);p.m.delay(.8);p.m.press('host','UISHOT')
 initial=carousel_surface(p);count=ident(initial['count']);index=ident(initial['selected'])
 if count>1:
  forward=index+1<count;first='carousel.next' if forward else 'carousel.previous';second='carousel.previous' if forward else 'carousel.next'
  changed=index+(1 if forward else -1)
  p.click(first,root=0x4b);p.m.wait(lambda:ident(carousel_surface(p)['selected'])==changed,8);p.m.delay(.8)
  p.click(second,root=0x4b);p.m.wait(lambda:ident(carousel_surface(p)['selected'])==index,8);p.m.delay(.8)
  confirm=p.widget('carousel.confirm',root=0x4b);amount=-1 if forward else 1
  p.wheel(p.center(confirm),amount);p.m.wait(lambda:ident(carousel_surface(p)['selected'])==changed,8);p.m.delay(.8)
  p.wheel(p.center(p.widget('carousel.confirm',root=0x4b)),-amount);p.m.wait(lambda:ident(carousel_surface(p)['selected'])==index,8);p.m.delay(.8)
  p.report['steps'].append({'action':'original_carousel_thumbnail_and_wheel','initial':initial,'observed_changed_index':changed,'restored':carousel_surface(p),'scope':'Actual copied original selected index must change through native events.'})
 else:p.report['steps'].append({'action':'carousel_single_native_level','state':initial,'scope':'No alternative unlocked native level exists in this private profile.'})
 if initial.get('category_enabled'):
  category=ident(initial['category']);p.click('carousel.category',root=0x4b);p.m.wait(lambda:ident(carousel_surface(p)['category'])!=category,8);changed=carousel_surface(p);p.m.delay(.8)
  p.click('carousel.category',root=0x4b);p.m.wait(lambda:ident(carousel_surface(p)['category'])==category,8);p.m.delay(.8)
  p.report['steps'].append({'action':'original_carousel_category','initial':initial,'changed':changed,'restored':carousel_surface(p)})
 else:p.report['steps'].append({'action':'carousel_category_unavailable','scope':'Original category callback is not registered; no unlock or forced category selection.'})
 p.m.press('host','UISHOT');p.click('carousel.confirm',root=0x4b);p.expect_root(0x44);p.m.delay(.6);p.m.press('host','UISHOT')
 p.report['steps'].append({'action':'original_carousel_confirm_opens_difficulty','native_root':p.m.state().get('native_root')})
 for difficulty in (0x49,0x4a):
  p.hover(difficulty,root=0x44);p.m.wait(lambda:p.m.state().get('native_focus')==difficulty,8)
  p.report['steps'].append({'action':'original_difficulty_focus_choice','id':difficulty,'native_focus':p.m.state().get('native_focus'),'scope':'Original focus callback chooses the difficulty; clicking Continue would start the level.'})
 p.m.press('host','UISHOT')
 p.back();p.expect_root(0x4b);return_to_main(p)


def credits_tests(p):
 p.expect_root(0x3d);p.click(0x42,root=0x3d);p.m.wait(lambda:p.m.state().get('scene')==5,20)
 p.m.wait(lambda:p.ack().get('presentation') and not p.ack().get('active') and not p.observe().get('has_surface',True),15)
 p.m.delay(.8);p.m.press('host','UISHOT');before=dict(p.ack());button=p.m.a.presentation_button
 # No GUI hit rectangle exists in original Credits. A fresh inside-image
 # press must reach the existing pad Continue/Back path through the adapter.
 p.button(True,[.5,.5],button=button);after_down=dict(p.ack())
 assert after_down.get('fallback_sequence',0)>before.get('fallback_sequence',0),('Credits pointer was not routed through presentation input',before,after_down)
 p.button(False,[.5,.5],button=button)
 if button=='right':
  # Original80164B60 accepts A/Start and has no B exit. Preserve that
  # negative action rather than inventing a mouse-specific Back mapping.
  p.m.delay(.75);assert p.m.state().get('scene')==5,('Original Credits unexpectedly treated B as Continue',public_menu(p.m.state()))
  p.report['steps'].append({'action':'original_credits_right_b_no_action','scene_before':5,'scene_after':5,'fallback_sequence_before':before.get('fallback_sequence'),'fallback_sequence_after_down':after_down.get('fallback_sequence'),'scope':'Right reaches original B; original Credits has no B exit.'})
  before=dict(p.ack());button='left';p.button(True,[.5,.5],button=button);after_down=dict(p.ack());assert after_down.get('fallback_sequence',0)>before.get('fallback_sequence',0)
  p.button(False,[.5,.5],button=button)
 p.m.wait(lambda:p.m.state().get('scene')==3 and p.m.state().get('native_root')==0x3d,20);p.expect_root(0x3d);p.m.delay(.4)
 assert not p.ack().get('presentation') and p.ack().get('fallback_buttons')==0,('Presentation click remained held after native scene return',p.ack())
 p.report['steps'].append({'action':'original_credits_pointer_continue','button':button,'scene_before':5,'scene_after':p.m.state().get('scene'),'fallback_sequence_before':before.get('fallback_sequence'),'fallback_sequence_after_down':after_down.get('fallback_sequence'),'scope':'Only the selected physical button was tested; original native Continue/Back semantics are preserved.'})
 p.m.press('host','UISHOT')


def frontend_tests(p):
 p.expect_root(0x3d);p.click(0x3f);p.m.wait(lambda:p.m.state().get('page')==1)
 for row in range(0x14a,0x14e):p.hover(row)
 p.blank();p.click(0x14a);p.m.wait(lambda:p.m.state().get('page')==2)
 p.click(0x14b);p.m.wait(lambda:p.m.state().get('editing')==2)
 # Text queue coverage is explicitly limited to native editing after a mouse
 # focus; this does not claim SDL_TEXTINPUT dispatch until that shim exists.
 p.m.command('host','text 123456');p.m.command('host','key 1');p.m.wait(lambda:p.m.state().get('editing')==0)
 p.click(0x14c);p.m.wait(lambda:p.m.state().get('editing')==3);p.m.command('host','text '+str(p.m.a.port));p.m.command('host','key 1')
 p.report['steps'].append({'action':'pointer_focus_native_text_queue','fields':['pin','port'],'scope':'Mouse focus and native text queue only; not SDL text event injection.'})
 p.click(0x14d);p.m.wait(lambda:p.m.state().get('page')==1);p.click(0x14b);p.m.wait(lambda:p.m.state().get('page')==3)
 p.click(0x14a);p.m.wait(lambda:p.m.state().get('editing')==1);p.m.command('host','text localhost');p.m.command('host','key 1')
 p.click(0x14b);p.m.wait(lambda:p.m.state().get('editing')==2);p.m.command('host','text 12345');p.m.command('host','key 1')
 p.click(0x14c);p.m.wait(lambda:p.m.state().get('page')==5);p.click(0x14c);p.m.wait(lambda:p.m.state().get('editing')==2);p.m.command('host','key 2')
 p.click(0x14d);p.m.wait(lambda:p.m.state().get('page')==8);p.m.delay(2);p.disabled();p.back();p.m.wait(lambda:p.m.state().get('page')==3)
 p.back();p.m.wait(lambda:p.m.state().get('page')==1);p.click(0x14d);p.expect_root(0x3d)


def local_mode_tests(p):
 for mode in (0xa9,0xaa,0xab,0xac):
  p.expect_root(0x3d);p.click(0x3f);p.m.wait(lambda:p.m.state().get('page')==1);p.click(0x14c);p.expect_root(0xa7)
  p.click(mode);p.m.wait(lambda:p.m.state().get('native_root')!=0xa7)
  assert not p.m.state().get('active'),public_menu(p.m.state())
  p.report['steps'].append({'action':'original_local_mode_open','mode_id':mode,'menu':public_menu(p.m.state())})
  for _ in range(8):
   if p.m.state().get('native_root')==0x3d:break
   p.back();p.m.delay(.25)
  p.expect_root(0x3d)


def local_roster_tests(p):
 p.expect_root(0x3d);p.click(0x3f,root=0x3d);p.m.wait(lambda:p.m.state().get('page')==1)
 p.click(0x14c);p.expect_root(0xa7);p.click(0xa9,root=0xa7);p.expect_root(0x59);p.m.delay(.6);p.m.press('host','UISHOT')
 p.click(0x5d,root=0x59);p.expect_root(0x9a);p.m.delay(.8);p.m.press('host','UISHOT')
 assert not p.m.state().get('active'),('Local roster unexpectedly opened a network session',public_menu(p.m.state()))
 def port0_roster():return any(ident(surface['root'])==0xa0 and ident(surface.get('port',999))==0 for surface in p.observe().get('surfaces',[]))
 if not port0_roster():
  # Original9A begins with Join, before a profile pane exists. The visible
  # footer must use the production mouse-to-original-PAD-A route, keeping
  # FF8E4's existing pad/assignment checks; no fixture Join key is sent.
  join=p.widget('footer.confirm',root=0x9a);before_join=p.observe().get('activations',0);p.click('footer.confirm',root=0x9a);p.m.wait(port0_roster,15)
  assert p.observe().get('activations',0)==before_join+1,('Local Join did not dispatch exactly once',before_join,p.observe())
  p.report['steps'].append({'action':'original_local_roster_join_by_mouse','port':0,'region':join,'a0_observed':True,'scope':'Visible original Join footer invokes normal PAD-A on the existing controller0; no pad connectivity or roster state is written by the test.'});p.m.delay(.5);p.m.press('host','UISHOT')
 # Inspect only the original controller-zero pane. No additional pads or
 # players are synthesized to make the local roster look populated.
 widget=p.widget(0xa1,root=0xa0,port=0);before=widget.get('value');p.hover(0xa1,root=0xa0,port=0)
 p.wheel(p.center(widget),1);changed=p.widget(0xa1,root=0xa0,port=0).get('value')
 if changed==before:p.wheel(p.center(p.widget(0xa1,root=0xa0,port=0)),-1);changed=p.widget(0xa1,root=0xa0,port=0).get('value');direction=-1
 else:direction=1
 assert changed!=before,('Original local profile selector did not change its native value',widget,before,changed)
 p.wheel(p.center(p.widget(0xa1,root=0xa0,port=0)),-direction);p.m.wait(lambda:p.widget(0xa1,root=0xa0,port=0).get('value')==before,8)
 p.click(0xa1,root=0xa0,port=0)
 def assigned_transition():
  panes=[surface for surface in p.observe().get('surfaces',[]) if ident(surface['root'])==0xa0 and ident(surface.get('port',999))==0 and surface['gui']==widget['gui']]
  return len(panes)==1 and any(ident(w['id'])==0xa2 for w in panes[0].get('widgets',[])) and not any(ident(w['id'])==0xa1 for w in panes[0].get('widgets',[]))
 p.m.wait(assigned_transition,10);assigned=p.widget(0xa2,root=0xa0,port=0,allowed=False);p.m.delay(.5);p.m.press('host','UISHOT')
 p.report['steps'].append({'action':'original_local_roster_profile_mouse','port':0,'initial_value':before,'observed_changed_value':changed,'region':widget,'assigned_profile_region':assigned,'a1_hidden_a2_visible':True,'scope':'Native selector roundtrip and successful original800FFD60/800FF2D4 assignment transition: A1 hidden, A2 visible on the same port0 GUI. Assigned profile ID/text is not read back; no extra controller count is claimed.'})
 return_to_main(p)


def run_plan(p,steps):
 for step in steps:
  role=step.get('role','host');action=step['action']
  if action=='start_client':p.m.start_role('client',step.get('language',p.m.a.language));continue
  if action=='expect_root':p.expect_root(step['root'],role)
  elif action=='click':p.click(step['id'],role,step.get('root'),step.get('hold',.06),step.get('port'))
  elif action=='hover':p.hover(step['id'],role,step.get('root'),step.get('port'))
  elif action=='back':p.back(role)
  elif action=='slider':p.slider(step['id'],role)
  elif action=='selector':p.selector(step['id'],role)
  elif action=='blank':p.blank(role)
  elif action=='outside':p.outside(role)
  elif action=='cancel':p.cancel(role)
  elif action in ('focus_lost','focus_gained'):p.command(role,action)
  elif action=='move':p.move(step['uv'],role)
  elif action=='down':p.button(True,step['uv'],role,step.get('button','left'))
  elif action=='up':p.button(False,step['uv'],role,step.get('button','left'))
  elif action=='wheel':p.wheel(step['uv'],step['amount'],role)
  elif action=='wait':p.m.delay(float(step['seconds']))
  elif action=='expect':
   p.m.wait(lambda:all(p.m.state(role).get(key)==value for key,value in step['menu'].items()),step.get('timeout',15))
  elif action=='native_text':
   p.m.command(role,'text '+step['text']);p.report['steps'].append({'action':action,'role':role,'scope':'Native edit queue after pointer focus.'})
  elif action=='edit_key':p.m.command(role,'key '+str(step['key']))
  elif action=='setup_pad':
   if not step.get('reason'):raise ValueError('Setup pad requires an explicit fixture reason')
   p.m.press(role,step['token']);p.report['steps'].append({'action':action,'role':role,'reason':step['reason'],'scope':'Fixture setup only; does not count as pointer coverage.'})
  elif action=='screenshot':p.m.press(role,'UISHOT')
  else:raise ValueError('Unknown pointer plan action '+str(action))


def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--language',type=int,choices=(0,1,2),default=1);ap.add_argument('--aspect',choices=('4x3','16x9','21x9'),default='16x9');ap.add_argument('--fps',type=int,default=60);ap.add_argument('--port',type=int,default=27930)
 ap.add_argument('--presentation-button',choices=('left','right'),default='left');ap.add_argument('--after-options',action='store_true',help='For local-roster only: first exercise original Options including the ScreenMode selector.')
 ap.add_argument('--suite',choices=('smoke','options','profiles','carousel','credits','frontend','local-modes','local-roster','all','plan'),default='all');ap.add_argument('--plan',type=Path);ap.add_argument('--require-all-groups',action='store_true');a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 if a.suite=='plan' and not a.plan:raise ValueError('--suite plan requires --plan')
 if a.after_options and a.suite!='local-roster':raise ValueError('--after-options is only valid with --suite local-roster')
 a.output.mkdir(parents=True,exist_ok=False);before=sha(a.card);inventory=original_inventory(a.data,a.language)
 (a.output/'original-menu-inventory.json').write_text(json.dumps(inventory,indent=2,ensure_ascii=False),encoding='utf8')
 aspect,width={'4x3':(0,960),'16x9':(3,960),'21x9':(3,1260)}[a.aspect]
 a.private_settings='[graphics]\nrenderer=gpu\nscale=1\naspect='+str(aspect)+'\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w='+str(width)+'\nwindow_h=540\ntop_bar=0\nfullscreen=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_NATIVE_MOUSE_COMMAND':COMMAND_FILE}
 m=MouseMenus(a,'',a.output);r={'passed':False,'exe_sha256':sha(a.exe),'language':a.language,'aspect':a.aspect,'fps':a.fps,'suite':a.suite,'steps':[],'scope':__doc__};pointer=Pointer(m,r)
 try:
  m.start_role('host',a.language);m.wait(lambda:m.mouse().get('surfaces') and m.mouse_ack().get('image_rect'),20);pointer.command('host','cancel')
  m.press('host','UISHOT');r['steps'].append({'action':'initial_visual_screenshot','scope':'Native screen before the first pointer hover; compare visible labels with reported bounds independently.'})
  if a.suite in ('smoke','options','all'):main_smoke(pointer)
  if a.suite in ('options','all'):option_tests(pointer)
  elif a.suite=='smoke':pointer.back();pointer.expect_root(0x3d)
  if a.suite in ('profiles','all'):profile_tests(pointer)
  if a.suite in ('carousel','all'):carousel_tests(pointer)
  if a.suite in ('credits','all'):credits_tests(pointer)
  if a.suite in ('frontend','all'):frontend_tests(pointer)
  if a.suite in ('local-modes','all'):local_mode_tests(pointer)
  if a.suite=='local-roster':
   if a.after_options:
    main_smoke(pointer);option_tests(pointer);m.delay(.5);r['steps'].append({'action':'local_roster_after_original_screen_mode_change','native_projection':{key:pointer.observe().get(key) for key in ('projection_source','native_half_extent','projection','wide')},'scope':'Original FF selector was changed through mouse wheel by option_tests before entering the native A1 pane.'})
   local_roster_tests(pointer)
  if a.plan:run_plan(pointer,json.loads(a.plan.read_text(encoding='utf8'))['steps'])
  pointer.observe();pointer.save_coverage(inventory)
  if a.require_all_groups:assert r['coverage']['all_original_groups_observed'],r['coverage']
  m.press('host','UISHOT');r['passed']=True
 except Exception:
  r['error']=traceback.format_exc();print(r['error'],flush=True)
  for role,process in m.processes.items():
   if process.poll() is None:
    try:m.press(role,'UISHOT')
    except Exception:pass
 finally:
  for role in tuple(m.processes):
   if m.processes[role].poll() is None:
    try:pointer.cancel(role)
    except Exception:pass
  pointer.save_coverage(inventory);r['final_states']={role:public_menu(m.state(role)) for role in m.processes};m.close()
  r.update(visible=m.visible,focused=m.focused,exits={role:process.returncode for role,process in m.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not m.visible and not m.focused and r['original_card_unchanged'] and all(code==0 for code in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps({k:v for k,v in r.items() if k not in ('steps','final_states')},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
