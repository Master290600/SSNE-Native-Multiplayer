"""Exercises native menus in hidden private processes without desktop input."""
from test_coop_campaign import Peers,sha
from pathlib import Path
import argparse,json,os,shutil,subprocess,time,traceback
class Menus(Peers):
 def state(self,role='host'):
  try:self.last_state[role]=json.loads((self.folder/role/'native-menu-state.json').read_text())
  except (OSError,ValueError):pass
  return self.last_state.get(role,{})
 def log(self,role):
  p=self.folder/role/'ssne.log';return p.read_text(encoding='utf8',errors='replace') if p.exists() else ''
 def wait(self,predicate,timeout=45):
  until=time.monotonic()+timeout;next_skip=time.monotonic()+3
  while not predicate():
   self.inspect()
   for role,p in self.processes.items():
    if p.poll() is not None:raise RuntimeError(role+' exited '+str(p.returncode))
    if '[port] Stopped:' in self.log(role):raise RuntimeError(role+' guest stopped')
   if time.monotonic()>until:raise TimeoutError(str({r:self.state(r) for r in self.processes}))
   if time.monotonic()>next_skip and len(self.processes)==2 and all(self.state(r).get("scene")==4 and not self.state(r).get("pause") for r in self.processes) and not all(self.performance(r).get("local_ready") for r in self.processes):
    self.pulse("B",tuple(self.processes));next_skip=time.monotonic()+3
   self.delay(.15)
 def start_role(self,role,language):
  f=self.folder/role;f.mkdir(parents=True);(f/'steps').mkdir();shutil.copy2(self.a.exe,f/'game.exe');shutil.copy2(self.a.card,f/'card.raw');(f/'input.txt').write_text('');(f/'command.txt').write_text('')
  (f/'settings.ini').write_text(getattr(self.a,'private_settings','[graphics]\nrenderer=gpu\nscale=1\nvsync=0\nwindow_w=960\nwindow_h=540\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'))
  env={k:v for k,v in os.environ.items() if not k.startswith('SSNE_')};env.update(SSNE_TEST_HIDDEN='1',SSNE_DEV_NATIVE_MENU='1',SSNE_PAD_SCRIPT_FILE=str(f/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(f/'steps'),SSNE_PERF_LOG=str(f/'performance.jsonl'),SSNE_NATIVE_MENU_COMMAND=str(f/'command.txt'),SSNE_NATIVE_MENU_PORT=str(self.a.port),SSNE_NATIVE_MENU_DISCOVERY_PORT=str(self.a.port+1),SSNE_NATIVE_TEST_LANGUAGE=str(language),SSNE_COOP_TEST_SKIP_DELAY_MS='1500',SSNE_TEST_SKIP_FIRST_VOTE='1')
  env.update(getattr(self.a,'test_env',{}))
  card=f/'card.raw'
  if getattr(self.a,'shared_card',False):
   card=self.folder/'shared-card.raw'
   if not card.exists():shutil.copy2(self.a.card,card)
  si=subprocess.STARTUPINFO();si.dwFlags|=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=0;stream=(f/'console.log').open('wb');self.streams.append(stream)
  self.processes[role]=subprocess.Popen([str(f/'game.exe'),'--data',str(self.a.data),'--memcard',str(card),'--mute']+(['--fps',str(self.a.fps)] if getattr(self.a,'fps',None) else []),cwd=f,env=env,stdout=stream,stderr=stream,startupinfo=si,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
  until=time.monotonic()+60
  while self.state(role).get('scene')!=3:
   if time.monotonic()>until:raise TimeoutError('Boot '+str(self.state(role)))
   self.delay(2)
   if self.state(role).get('scene')==2:self.press(role,'B')
  self.delay(3)
 def command(self,role,text):
  self.command_sequence+=1;p=self.folder/role/'command.txt';t=p.with_suffix('.pending');t.write_text(str(self.command_sequence)+' '+text);os.replace(t,p);self.delay(.25)
 def press(self,role,token):self.pulse(token,(role,));self.delay(.18)
 def page(self,role,n):self.wait(lambda:self.state(role).get('page')==n);self.delay(.4)
 def choose(self,role,index):
  for _ in range(6):
   if self.state(role).get('focus')==index:break
   self.press(role,'DOWN')
  assert self.state(role).get('focus')==index,self.state(role)
  self.press(role,'A')
 def network(self,role):
  self.wait(lambda:self.state(role).get('native_root')==0x3d and not self.state(role).get('returning'))
  for _ in range(6):
   if self.state(role).get('native_focus')==0x3f:break
   self.press(role,'DOWN')
  assert self.state(role).get('native_focus')==0x3f,self.state(role)
  self.press(role,'A');self.page(role,1)
 def field(self,role,index,kind,value):
  self.choose(role,index);self.wait(lambda:self.state(role).get('editing')==kind);self.command(role,'text '+value);self.command(role,'key 1');self.wait(lambda:self.state(role).get('editing')==0)
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--exe',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);ap.add_argument('--card',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--port',type=int,default=27836);a=ap.parse_args()
 for k in ('exe','data','card','output'):setattr(a,k,getattr(a,k).resolve())
 a.output.mkdir(parents=True,exist_ok=False);m=Menus(a,'',a.output);result={'passed':False,'exe_sha256':sha(a.exe),'steps':[]};before=sha(a.card)
 def record(s):result['steps'].append(s);print(s,flush=True)
 try:
  m.start_role('host',1);m.network('host');m.press('host','UISHOT');m.choose('host',0);m.page('host',2);pin=m.state()['pin'];m.press('host','UISHOT');m.choose('host',0);m.wait(lambda:m.state().get('active'));m.delay(2);record('Native German host created without launcher or network CLI flags')
  m.wait(lambda:'original mode selection opened' in m.log('host'));m.delay(2);m.press('host','A');m.delay(2);m.press('host','START');m.wait(lambda:m.state().get('lobby'));m.press('host','Y');m.page('host',6);m.choose('host',0);m.choose('host',0);m.wait(lambda:m.state().get('bots')==2);m.choose('host',1);m.wait(lambda:m.state().get('skill')==3);m.choose('host',2);m.wait(lambda:m.state().get('bots')==1);m.choose('host',0);m.wait(lambda:m.state().get('bots')==2);m.press('host','UISHOT');m.choose('host',3);m.wait(lambda:m.state().get('lobby'));record('Native bot management: add, remove, difficulty, return to original lobby')
  m.start_role('client',2);m.network('client');m.choose('client',1);m.page('client',3);m.choose('client',3);m.page('client',8);m.wait(lambda:m.state('client').get('rooms',0)>0);m.delay(2.5);assert m.state('client').get('rooms')==1;m.press('client','UISHOT');m.choose('client',3);m.page('client',3);record('French LAN search lists the native host once; Back returns to manual entry')
  m.field('client',0,1,'localhost');m.field('client',1,2,'000000' if pin!='000000' else '111111');m.choose('client',2);m.page('client',5);m.press('client','UISHOT');record('Incorrect PIN rejected in native French error page')
  m.choose('client',2);m.page('client',3);m.wait(lambda:m.state('client').get('editing')==2);m.command('client','text '+pin);m.command('client','key 1');m.choose('client',2);m.wait(lambda:m.state('client').get('active') and m.state().get('players')==4,40);m.wait(lambda:m.state('client').get('lobby'));m.press('client','UISHOT');record('Client joined same running host after correcting PIN')
  m.press('host','START');m.delay(1);m.press('host','A');m.wait(lambda:'original difficulty selection opened' in m.log('host'));m.delay(1);m.press('host','A');m.wait(lambda:m.state().get('scene')==4 and m.state('client').get('scene')==4,60)
  m.wait(lambda:all(m.performance(r).get('local_ready') and m.performance(r).get('ready_players')==4 for r in ('host','client')),60);m.delay(4);result['gameplay']={r:m.performance(r) for r in ('host','client')};record('Original map selection started synchronized match with host, client, two bots')
  m.press('host','START');m.wait(lambda:m.state().get('pause'));m.press('host','UISHOT');m.press('host','SAVE');m.wait(lambda:'[coop-save-result] save=1 slot=1 ok=1' in m.log('host'),20);m.press('host','X');m.page('host',7);m.choose('host',0);m.press('host','B');m.delay(1);m.press('host','LOAD');m.wait(lambda:'[coop-save-result] save=0 slot=1 ok=1' in m.log('host'),25);m.wait(lambda:m.state().get('pause'));m.press('host','X');m.page('host',7);m.choose('host',0);m.press('host','B');m.wait(lambda:not m.state().get('pause'));record('Paused native menu bindings survived cooperative save/load')
  m.wait(lambda:all(m.performance(r).get('local_ready') and m.performance(r).get('ready_players')==4 for r in ('host','client')),45);m.delay(2)
  m.press('client','START');m.wait(lambda:m.state('client').get('pause'));m.press('client','UISHOT');m.press('client','X');m.page('client',7);m.choose('client',2);m.wait(lambda:not m.state('client').get('active') and m.state('client').get('scene')==3);m.wait(lambda:m.state().get('players')==3);record('Client left match through native pause menu; host continued')
  m.press('host','START');m.wait(lambda:m.state().get('pause'));m.press('host','UISHOT');m.press('host','X');m.page('host',7);m.choose('host',2);m.wait(lambda:not m.state().get('active') and m.state().get('scene')==3);m.wait(lambda:m.state().get('native_root')==0x3d and not m.state().get('returning'));record('Host returned to original main menu in same process')
  m.delay(2);m.network('host');m.choose('host',0);m.page('host',2);m.choose('host',0);m.wait(lambda:m.state().get('active'));m.delay(2);m.press('host','B');m.delay(1);m.wait(lambda:not m.state().get('active'));record('Host could create and leave another session without restarting')
  m.close();assert all(p.returncode==0 for p in m.processes.values());assert sha(a.output/'client/card.raw')==before
  english=Menus(a,'',a.output/'english')
  try:
   english.start_role('host',0);english.network('host');english.press('host','UISHOT');pid=english.processes['host'].pid;english.choose('host',2);english.page('host',0);english.delay(2);english.press('host','UISHOT');assert not english.state().get('active') and english.processes['host'].pid==pid;record('English original menu and local multiplayer path verified')
  finally:english.close()
  assert not english.visible and not english.focused;result['passed']=True
 except Exception:
  result['error']=traceback.format_exc();print(result['error'],flush=True)
  for role,p in m.processes.items():
   if p.poll() is None:m.press(role,'UISHOT')
 finally:
  m.close();result.update(visible=m.visible,focused=m.focused,exits={r:p.returncode for r,p in m.processes.items()},original_card_unchanged=sha(a.card)==before,client_card_unchanged=not (a.output/'client/card.raw').exists() or sha(a.output/'client/card.raw')==before,states={r:m.state(r) for r in m.processes});(a.output/'result.json').write_text(json.dumps(result,indent=2))
 return 0 if result['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
