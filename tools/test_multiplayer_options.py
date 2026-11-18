"""Private PAL menus -> authenticated LAN join -> authoritative extended rules.
Mouse commands traverse the real SDL/native widget path. Intro/map shortening
uses only the explicitly reported fixtures from the existing integration tools.
This does not claim a complete campaign playthrough.
"""
from pathlib import Path
import argparse,json,traceback
from test_native_mouse import Pointer,COMMAND_FILE
from test_native_mouse_multiplayer import MultiplayerMenus,page,host_create,wait_lobby,begin_campaign,open_pause,connection
from test_coop_campaign import sha

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--port',type=int,default=28760);ap.add_argument('--language',type=int,default=1);a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.mkdir(parents=True,exist_ok=False);a.fps=60;a.client_language=2;a.allow_start_key_fallback=False
 a.private_settings='[graphics]\nrenderer=gpu\nscale=1\naspect=3\nvsync=0\nwindow_w=960\nwindow_h=540\ntop_bar=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_NATIVE_MOUSE_COMMAND':COMMAND_FILE}
 report={'passed':False,'steps':[]};m=MultiplayerMenus(a,'',a.output/'peers');p=Pointer(m,report)
 protected={key:sha(path) for key,path in {'card':a.card,'exe':a.exe,'dol':a.data/'sys/main.dol'}.items()}
 def options(role):p.click(0x155,role,root=0x148);page(p,9,role)
 def setup_names(role,value,host_name=None):
  p.click(0x14c,role,root=0x148);page(p,14,role)
  for widget,editing,text in [(0x163,4,value)]+([(0x164,6,host_name)] if host_name is not None else []):
   p.click(widget,role,root=0x148);m.wait(lambda:m.state(role).get('editing')==editing,10)
   m.command(role,'text '+text);m.command(role,'key 1');m.wait(lambda:m.state(role).get('editing')==0,10)
  assert m.state(role)['name_bytes']==len(value.encode('latin1'))
  saved=(m.folder/role/'settings.ini.multiplayer').read_text(encoding='latin1')
  assert saved.startswith('SSNE-MP-2\n') and '"'+value+'"' in saved
  if host_name is not None:assert '"'+host_name+'"' in saved
  p.back(role);page(p,1,role)
 def host_rules():
  p.back();p.expect_root(0x3d);host_create(p)
  p.click(0x154,root=0x9a);page(p,15)
 try:
  m.start_role('host',a.language);m.wait(lambda:m.mouse().get('surfaces') and m.mouse_ack().get('image_rect'),20)
  p.command('host','cancel');p.click(0x15c,root=0x3d);page(p,1);setup_names('host','Joerg Host','Joerg Coop');host_rules()
  # Cycle every factor, not just the two end points. Capacity keeps native max last.
  for widget,key,expected in ((0x14a,'enemy_factor',[2,3,4,5]),(0x14b,'ammo_capacity',[1,3,4,5,2,0,1,3,4,5]),(0x14c,'pickup_factor',[2,3,4,5])):
   for value in expected:p.click(widget,root=0x148);m.wait(lambda:m.state().get(key)==value,5)
  m.press('host','UISHOT');m.delay(.3);report['configured_host']=dict(m.state())
  # Bots is between the last rule and the native Back footer.
  p.move([.95,.05]);m.press('host','DOWN');m.press('host','DOWN')
  assert m.state()['focus']==4 and m.state()['native_focus']==0x155,m.state()
  m.press('host','KEY_ENTER');wait_lobby(p)
  m.start_role('client',a.client_language);m.wait(lambda:m.mouse('client').get('surfaces') and m.mouse_ack('client').get('image_rect'),20)
  p.command('client','cancel');p.click(0x15c,'client',root=0x3d);page(p,1,'client');setup_names('client','Client Test')
  p.click(0x14a,'client');page(p,3,'client');p.click(0x14b,'client');page(p,8,'client');m.wait(lambda:m.state('client').get('rooms')==1,20);p.click(0x14a,'client')
  m.wait(lambda:m.state('client').get('active') and m.state().get('players')==2,40);wait_lobby(p,'host');wait_lobby(p,'client')
  begin_campaign(p,exercise_neighbors=False)
  expected={'shared_enemies':5,'shared_capacity':5,'shared_pickups':5}
  m.wait(lambda:all(all(m.state(r).get(k)==v for k,v in expected.items()) for r in ('host','client')),20)
  report['replicated_rules']={r:{k:m.state(r).get(k) for k in expected} for r in ('host','client')}
  open_pause(p,'client');connection(p,'client');options('client');m.press('client','UISHOT');m.delay(.3)
  # All host rule controls and the active name field must be read-only on a client.
  w=p.widget(0x14a,'client',root=0x148,allowed=False)
  assert not w.get('allowed'),('Active client can edit its locked name',w)
  rules=[w for s in p.observe('client').get('surfaces',[]) for w in s.get('widgets',[]) if w.get('id') in (0x14b,0x14c,0x14d)]
  assert not any(w.get('allowed') and w.get('selectable') for w in rules),('Client can edit shared host rules',rules)
  p.click(0x155,'client');page(p,7,'client');p.click(0x14a,'client');p.expect_root(0x129,'client');m.press('client','START')
  report['campaign_sample']={r:m.performance(r) for r in ('host','client')}
  report['host_player_sample']=[o for o in m.campaign().get('objects',[]) if o.get('kind')==0]
  report['passed']=True
 except Exception:report['error']=traceback.format_exc();print(report['error'],flush=True)
 finally:
  m.close();report.update(visible=m.visible,focused=m.focused,exits={r:proc.returncode for r,proc in m.processes.items()})
  report['protected_unchanged']={key:sha(path)==protected[key] for key,path in {'card':a.card,'exe':a.exe,'dol':a.data/'sys/main.dol'}.items()}
  report['passed'] &= all(report['protected_unchanged'].values()) and not m.visible and not m.focused and all(proc.returncode==0 for proc in m.processes.values())
  (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8');print(json.dumps({k:v for k,v in report.items() if k not in ('steps','campaign_sample','host_player_sample')},indent=2),flush=True)
 return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
