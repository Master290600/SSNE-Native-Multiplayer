"""Hidden PAL campaign integration matrix. Native events are explicit shortcuts;
this is not a claim that an input bot has completed every encounter normally.
All cards/settings are private and all controls stay inside the child process.
"""
import argparse,ctypes,hashlib,json,os,shutil,subprocess,time,traceback
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
class Peers:
 def __init__(self,a,level,folder):
  self.a=a;self.level=level;self.folder=folder;self.processes={};self.streams=[];self.sequence=0;self.command_sequence=0;self.visible=[];self.focused=[];self.last_state={}
 def inspect(self):
  u=ctypes.windll.user32;u.GetForegroundWindow.restype=ctypes.c_void_p;u.GetWindowThreadProcessId.argtypes=(ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong));u.IsWindowVisible.argtypes=(ctypes.c_void_p,)
  cbtype=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p);u.EnumWindows.argtypes=(cbtype,ctypes.c_void_p);fg=u.GetForegroundWindow();pids={p.pid for p in self.processes.values()}
  @cbtype
  def visit(hwnd,_):
   pid=ctypes.c_ulong();u.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
   if pid.value in pids:
    if u.IsWindowVisible(hwnd):self.visible.append(int(hwnd))
    if hwnd==fg:self.focused.append(int(hwnd))
   return True
  u.EnumWindows(visit,0)
  if self.visible or self.focused:raise RuntimeError('Peer window became visible or focused')
 def delay(self,seconds):
  until=time.monotonic()+seconds
  while time.monotonic()<until:self.inspect();time.sleep(.08)
 def log(self,role):
  p=self.folder/role/('ssne-coop-host.log' if role=='host' else 'ssne-coop-client.log');return p.read_text(encoding='utf8',errors='replace') if p.exists() else ''
 def pulse(self,token,roles=('host','client'),seconds=.18):
  self.sequence+=1
  for role in roles:
   p=self.folder/role/'input.txt';t=p.with_suffix('.pending');t.write_text(','*self.sequence+token+'@0:100000',encoding='ascii')
   for attempt in range(100):
    try:os.replace(t,p);break
    except PermissionError:time.sleep(.01)
  self.delay(seconds)
  for role in roles:(self.folder/role/'input.txt').write_text('')
  self.delay(.2)
 def command(self,action):
  self.command_sequence+=1;p=self.folder/'host/command.txt';t=p.with_suffix('.pending');t.write_text(str(self.command_sequence)+' '+action);os.replace(t,p)
 def state(self):
  # The Windows native writer replaces its diagnostic JSON between frames.
  # Retain the last complete snapshot across that brief replacement interval.
  try:self.last_state=json.loads((self.folder/'host/campaign-state.json').read_text())
  except (OSError,ValueError):pass
  return self.last_state
 def performance(self,role):
  try:
   with (self.folder/role/'performance.jsonl').open('rb') as f:
    f.seek(max(0,f.seek(0,2)-100000));lines=f.read().splitlines()
   return next(json.loads(x) for x in reversed(lines) if b'"type":"sample"' in x)
  except (OSError,ValueError,StopIteration):return {}
 def ready(self,level):
  return all((s:=self.performance(role)).get('level')==level and s.get('local_ready') and s.get('ready_players')==2 for role in self.processes)
 def wait(self,predicate,timeout=60):
  start=time.monotonic();next_skip=3
  while not predicate():
   self.inspect()
   for role,p in self.processes.items():
    if p.poll() is not None:raise RuntimeError(role+' exited '+str(p.returncode))
    if '[port] Stopped:' in self.log(role):raise RuntimeError(role+' guest stopped')
   elapsed=time.monotonic()-start
   if elapsed>timeout:raise TimeoutError('State wait exceeded '+str(timeout)+' seconds; '+str({r:self.performance(r).get('local_ready') for r in self.processes}))
   if elapsed>=next_skip:
    # Terminal links may return to the main menu. Never let an old results
    # log turn automatic continuation into an unintended new game there.
    if self.performance('host').get('scene')==4:
     log=self.log('host');self.pulse('A' if log.rfind('[coop-host-results]')>log.rfind('[coop-results-cleanup]') else 'B')
    next_skip+=3
   self.delay(.2)
 def start(self):
  self.folder.mkdir(parents=True,exist_ok=False)
  for role in ('host','client'):
   f=self.folder/role;f.mkdir();(f/'steps').mkdir();shutil.copy2(self.a.exe,f/self.a.exe.name);shutil.copy2(self.a.card,f/'card.raw');(f/'input.txt').write_text('');(f/'command.txt').write_text('')
   (f/'settings-netplay.ini').write_text('[graphics]\nrenderer=gpu\nscale=1\nvsync=0\nwindow_w=960\nwindow_h=540\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n')
   env={k:v for k,v in os.environ.items() if not k.startswith('SSNE_')};env.update(SSNE_TEST_HIDDEN='1',SSNE_PAD_SCRIPT_FILE=str(f/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(f/'steps'),SSNE_PERF_LOG=str(f/'performance.jsonl'),SSNE_COOP_TEST_SKIP_DELAY_MS='1500',SSNE_TEST_SKIP_FIRST_VOTE='1',SSNE_COOP_CAMPAIGN_VERIFY='1')
   if role=='host':env.update(SSNE_COOP_CAMPAIGN_COMMAND=str(f/'command.txt'),SSNE_NETPLAY_MOD_FLAGS='0')
   args=[str(f/self.a.exe.name),'--data',str(self.a.data),'--memcard',str(f/'card.raw'),'--lan-port',str(self.a.port),'--lan-pin','123456','--player-name',role,'--mute']+(['--coop-host','--coop-capacity','8','--coop-level',self.level] if role=='host' else ['--coop-join','127.0.0.1'])
   si=subprocess.STARTUPINFO();si.dwFlags|=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=0;stream=(f/'console.log').open('wb');self.streams.append(stream);self.processes[role]=subprocess.Popen(args,cwd=f,env=env,stdout=stream,stderr=stream,startupinfo=si,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
   self.delay(2 if role=='host' else .2)
 def restart_client(self):
  old=self.processes['client'];self.pulse('QUIT',('client',));old.wait(timeout=8)
  self.streams[-1].close();previous=self.folder/'client-before-rejoin';(self.folder/'client').rename(previous)
  f=self.folder/'client';f.mkdir();(f/'steps').mkdir();shutil.copy2(self.a.exe,f/self.a.exe.name);shutil.copy2(previous/'card.raw',f/'card.raw');shutil.copy2(previous/'settings-netplay.ini',f/'settings-netplay.ini');(f/'input.txt').write_text('')
  env={k:v for k,v in os.environ.items() if not k.startswith('SSNE_')};env.update(SSNE_TEST_HIDDEN='1',SSNE_PAD_SCRIPT_FILE=str(f/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(f/'steps'),SSNE_PERF_LOG=str(f/'performance.jsonl'),SSNE_COOP_TEST_SKIP_DELAY_MS='1500',SSNE_TEST_SKIP_FIRST_VOTE='1',SSNE_COOP_CAMPAIGN_VERIFY='1')
  si=subprocess.STARTUPINFO();si.dwFlags|=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=0;stream=(f/'console.log').open('wb');self.streams.append(stream)
  self.processes['client']=subprocess.Popen(old.args,cwd=f,env=env,stdout=stream,stderr=stream,startupinfo=si,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
  return previous
 def close(self):
  for role,p in reversed(list(self.processes.items())):
   if p.poll() is None:
    try:self.pulse('QUIT',(role,));p.wait(timeout=8)
    except Exception:p.terminate();p.wait(timeout=8)
  for f in self.streams:f.close()
 def stage(self,level):
  self.wait(lambda:self.ready(level) and self.state().get('level')==level);self.delay(4);self.pulse('SHOT');s=self.state();s['client_performance']=self.performance('client');return s

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--exe',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);ap.add_argument('--card',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--levels',nargs='*');ap.add_argument('--links',action='store_true');ap.add_argument('--port',type=int,default=27816);a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.mkdir(parents=True,exist_ok=True);levels=a.levels or [p.stem for p in sorted((a.data/'files/Levels').glob('*.ssw')) if 'DM' not in p.stem and p.stem!='Gallery'];results=[];before=sha(a.card)
 for level in levels:
  peers=Peers(a,level,a.output/level);row={'level':level,'exe_sha256':sha(a.exe),'passed':False,'stages':[]};print('Starting '+level,flush=True)
  try:
   peers.start();state=peers.stage(level);row['stages'].append(state);print('Ready '+level+' objects='+str(len(state.get('objects',[]))),flush=True)
   links=[o for o in state.get('objects',[]) if o.get('target')]
   if a.links and links:
    link=next((x for x in links if not x.get('terminal')),links[0]);peers.command('link '+link['target']);peers.wait(lambda:'action=link from='+level in peers.log('host'),15)
    if not link.get('terminal'):row['stages'].append(peers.stage(link['target']))
    else:peers.wait(lambda:'[coop-client-native-results]' in peers.log('client'),30);peers.delay(3)
   peers.close();client=peers.log('client');checks=[x for x in client.splitlines() if '[campaign-verify]' in x];row['verification_samples']=checks;row['errors']=[x for x in client.splitlines() if '[campaign-field-difference]' in x or '[port] Stopped:' in x];row['passed']=bool(checks) and not row['errors'] and all('differences=0' in x for x in checks) and all(p.returncode==0 for p in peers.processes.values())
  except Exception:row['error']=traceback.format_exc();print(row['error'],flush=True)
  finally:
   peers.close();row['visible']=peers.visible;row['focused']=peers.focused;row['exits']={r:p.returncode for r,p in peers.processes.items()};row['passed']&=not peers.visible and not peers.focused and sha(a.card)==before
   (peers.folder/'result.json').write_text(json.dumps(row,indent=2));results.append(row);(a.output/'result.json').write_text(json.dumps({'passed':all(x['passed'] for x in results),'exe_sha256':sha(a.exe),'card_unchanged':sha(a.card)==before,'cases':results},indent=2));print('Completed '+level+' passed='+str(row['passed']),flush=True)
 return 0 if all(x['passed'] for x in results) else 1
if __name__=='__main__':raise SystemExit(main())
