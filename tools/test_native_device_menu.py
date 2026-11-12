"""Real update_input binding/routing with private keyboard/mouse snapshots."""
from test_native_frontend import Menus,sha
from pathlib import Path
import argparse,json,time,traceback

def main():
 ap=argparse.ArgumentParser()
 for k in ('exe','data','card','output'):ap.add_argument('--'+k,type=Path,required=True)
 ap.add_argument('--fps',type=int,default=60);ap.add_argument('--menu-only',action='store_true');ap.add_argument('--cycles',type=int,default=3);ap.add_argument('--hold',type=float,default=1.2);ap.add_argument('--port',type=int,default=27850)
 a=ap.parse_args()
 for k in ('exe','data','card','output'):setattr(a,k,getattr(a,k).resolve())
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1'};a.output.mkdir(parents=True,exist_ok=False);m=Menus(a,'',a.output);before=sha(a.card)
 r={'passed':False,'exe_sha256':sha(a.exe),'cycles':[],'device_input':True,'fps':a.fps}
 def hold(token,seconds):
  m.sequence+=1;f=a.output/'host/input.txt';f.write_text(','*m.sequence+token+'@0:100000');states=[];end=time.monotonic()+seconds
  while time.monotonic()<end:states.append(m.state().copy());m.delay(.05)
  f.write_text('');m.delay(.5);return states
 try:
  m.start_role('host',1);pid=m.processes['host'].pid
  for cycle in range(a.cycles):
   m.network('host');m.choose('host',0);m.page('host',2)
   states=hold('A',a.hold);m.delay(1)
   c={'cycle':cycle,'after_start':m.state().copy(),'main_gap_samples':[s for s in states if s.get('active') and s.get('page')==0 and s.get('native_root')==0x3d]}
   r['cycles'].append(c)
   assert m.state().get('active') and m.state().get('native_root')==0xa7,('held Start click skipped mode chooser',m.state())
   c['cancel_states']=hold('START',1);m.delay(1);c['after_release']=m.state().copy()
   assert not m.state().get('active') and m.state().get('native_root')==0x3d and not m.state().get('returning'),('Escape release stuck after Host stopped',m.state())
   assert m.state().get('input')==0,m.state()
   assert not c['main_gap_samples'],('main screen flashed during Host start',len(c['main_gap_samples']))
   # Prove another menu can be opened/clicked after the shutdown/release.
   m.network('host');m.choose('host',1);m.page('host',3);m.press('host','START');m.page('host',1);m.choose('host',3)
   m.wait(lambda:m.state().get('native_root')==0x3d and not m.state().get('returning'))
   assert m.processes['host'].pid==pid
   print('Device cycle '+str(cycle)+' passed',flush=True)
  m.network('host');m.choose('host',2);m.page('host',0);m.wait(lambda:m.state().get('native_root')==0xa7);m.press('host','START');m.wait(lambda:m.state().get('native_root')==0x3d)
  m.network('host');m.choose('host',0);m.page('host',2)
  if not a.menu_only:
   hold('A',a.hold)
   m.wait(lambda:m.state().get('native_root')==0xa7);m.press('host','A');m.delay(1);m.press('host','KEY_ENTER');m.wait(lambda:m.state().get('lobby'))
   m.press('host','X');m.page('host',7);hold('START',1);m.wait(lambda:m.state().get('lobby'))
   assert m.state().get('active'),m.state()
   m.press('host','KEY_ENTER');m.delay(1);m.press('host','A');m.wait(lambda:'original difficulty selection opened' in m.log('host'));m.press('host','A')
   m.wait(lambda:m.state().get('scene')==4 and m.performance('host').get('local_ready'),90);m.delay(5)
   r['pause_cycles']=[]
   for cycle in range(3):
    hold('START',1);assert m.state().get('pause'),('held Esc bounced out of pause',m.state())
    if cycle==0:
     m.press('host','X');m.page('host',7);hold('START',1);assert m.state().get('pause') and not m.state().get('visible'),('Esc skipped original pause after connection',m.state())
    hold('START',1);assert not m.state().get('pause'),('Esc reopened pause',m.state())
    r['pause_cycles'].append(True)
   p=m.performance('host')['position'];hold('SUP',1);m.delay(1.1);q=m.performance('host')['position']
   import math
   r['movement_after_escape']=math.hypot(q[0]-p[0],q[2]-p[2]);assert r['movement_after_escape']>.5
  r['passed']=True
 except Exception:r['error']=traceback.format_exc()
 finally:
  r['last_state']=m.state().copy();m.close();r.update(visible=m.visible,focused=m.focused,exits={role:p.returncode for role,p in m.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not m.visible and not m.focused and r['original_card_unchanged'] and all(x==0 for x in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps({k:v for k,v in r.items() if k!='cycles'},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
