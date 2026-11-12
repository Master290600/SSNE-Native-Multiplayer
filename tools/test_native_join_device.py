"""Same-PC native joins through device binding conversion; private shared card.
Text uses the native event queue while simultaneous WASD snapshots exercise
keyboard/menu isolation. No desktop events, visibility or focus changes.
"""
from test_native_frontend import Menus,sha
from pathlib import Path
import argparse,json,time,traceback,math

def main():
 ap=argparse.ArgumentParser()
 for k in ('exe','data','card','output'):ap.add_argument('--'+k,type=Path,required=True)
 ap.add_argument('--port',type=int,default=27854);ap.add_argument('--fps',type=int,default=60);ap.add_argument('--language',type=int,choices=(0,1,2),default=1)
 a=ap.parse_args()
 for k in ('exe','data','card','output'):setattr(a,k,getattr(a,k).resolve())
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1'};a.shared_card=True
 a.output.mkdir(parents=True,exist_ok=False);m=Menus(a,'',a.output);before=sha(a.card)
 r={'passed':False,'exe_sha256':sha(a.exe),'steps':[],'shared_private_card':True,'device_bindings':True}
 def record(s):r['steps'].append(s);print(s,flush=True)
 def typing(role,index,kind,value):
  m.choose(role,index);m.wait(lambda:m.state(role).get('editing')==kind)
  # Address letters W/S are held keyboard movement keys as text arrives.
  m.sequence+=1;f=a.output/role/'input.txt';f.write_text(','*m.sequence+'DOWN@0:100000')
  m.command(role,'text '+value);m.delay(.4);f.write_text('');m.delay(.2)
  assert m.state(role).get('editing')==kind,m.state(role)
  m.command(role,'key 1');m.wait(lambda:m.state(role).get('editing')==0)
 def leave_client():
  m.press('client','X');m.page('client',7);m.choose('client',2)
  m.wait(lambda:not m.state('client').get('active') and m.state('client').get('native_root')==0x3d and not m.state('client').get('returning'))
  m.wait(lambda:m.state().get('players')==1)
 try:
  m.start_role('host',1);m.network('host');m.choose('host',0);m.page('host',2);pin=m.state()['pin'];m.choose('host',0)
  m.wait(lambda:m.state().get('active') and m.state().get('native_root')==0xa7)
  m.press('host','A');m.delay(1);m.press('host','KEY_ENTER');m.wait(lambda:m.state().get('lobby'))
  m.start_role('client',a.language);m.network('client');m.choose('client',1);m.page('client',3)
  typing('client',0,1,'localhost');assert m.state('client')['address']=='localhost',m.state('client')
  typing('client',1,2,'12345');m.choose('client',2);m.page('client',5)
  assert '[native-network-menu] validation failed: invalid-pin' in m.log('client')
  assert m.state().get('players')==1
  record('Five-digit PIN reproduces pre-connect rejection; corrected field remains editable')
  m.choose('client',2);m.page('client',3);m.command('client','text '+('000000' if pin!='000000' else '111111'));m.command('client','key 1')
  start=time.monotonic();m.choose('client',2);m.page('client',5)
  r['rejection_seconds']=time.monotonic()-start;assert r['rejection_seconds']<6,r['rejection_seconds']
  record('Wrong PIN reaches Retry promptly; hostname typing survives held movement binding')
  m.choose('client',2);m.page('client',3);m.command('client','text '+pin);m.command('client','key 1');m.choose('client',2)
  m.wait(lambda:m.state('client').get('active') and m.state().get('players')==2 and m.state('client').get('lobby'))
  assert m.state('client').get('local_slot')==1,m.state('client');record('Direct address and PIN join same-PC host; assigned client slot 1')
  m.press('client','UISHOT');leave_client();m.network('client');m.choose('client',1);m.page('client',3)
  typing('client',1,2,'000000' if pin!='000000' else '111111');assert m.state('client')['pin']!=pin
  m.choose('client',3);m.page('client',8);m.wait(lambda:m.state('client').get('rooms',0)>0);m.delay(2.5)
  assert m.state('client').get('rooms')==1 and m.state('client').get('room_focus_matches'),m.state('client');r['unique_lan_rooms']=m.state('client')['rooms'];m.press('client','UISHOT')
  m.choose('client',0)
  m.wait(lambda:m.state('client').get('active') and m.state().get('players')==2 and m.state('client').get('lobby'))
  assert m.state('client').get('editing')==0,m.state('client')
  record('One LAN room despite loopback/broadcast replies; selection reconnects directly with an intentionally wrong cached PIN')
  m.press('host','KEY_ENTER');m.delay(1);m.press('host','A');m.wait(lambda:'original difficulty selection opened' in m.log('host'));m.press('host','A')
  m.wait(lambda:all(m.state(x).get('scene')==4 and m.performance(x).get('local_ready') and m.performance(x).get('ready_players')==2 for x in ('host','client')),90)
  m.delay(4);r['gameplay']={x:m.performance(x) for x in ('host','client')};m.pulse('UISHOT')
  for role in ('host','client'):
   p=m.performance(role)['position'];m.pulse('SUP',(role,),seconds=1);m.delay(1);q=m.performance(role)['position']
   r[role+'_movement']=math.hypot(q[0]-p[0],q[2]-p[2]);assert r[role+'_movement']>.5
  record('Both peers ready in same world; host and client movement works')
  r['passed']=True
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  r['states']={x:m.state(x).copy() for x in m.processes};m.close()
  r.update(visible=m.visible,focused=m.focused,exits={x:p.returncode for x,p in m.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not m.visible and not m.focused and r['original_card_unchanged'] and all(x==0 for x in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8')
  print(json.dumps({k:v for k,v in r.items() if k not in ('states','gameplay')},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
