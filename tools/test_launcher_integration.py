"""Normal launcher UI -> owned PAL build -> actual native-game launch acceptance.

Only private package copies are changed. A cold case starts with no game executable,
translated sources or compiled game-object cache. Launcher commands feed the same
SDL/ImGui input path as a physical pointer; direct LauncherApp activation is never
used as UI proof. The optional peer bootstrap is qualified separately from the
launcher-originated host process. No test executes merely by importing this file.
"""
from pathlib import Path
import argparse,ctypes,hashlib,json,os,re,shutil,struct,subprocess,time,traceback
from test_native_mouse import Pointer,COMMAND_FILE,ident,select_original_profile
from test_native_mouse_multiplayer import MultiplayerMenus,host_create,client_join_and_roles,begin_campaign,leave_sessions

LAUNCHER_COMMAND='launcher-command.txt'
LAUNCHER_STATE='launcher-test-state.json'
PROCESS_LOG='launcher-processes.jsonl'
MP_REL=Path('Multiplayer/SSNE-Native-Multiplayer-Game.exe')
SETTINGS='[graphics]\nrenderer=gpu\nscale=1\naspect=3\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w=960\nwindow_h=540\ntop_bar=0\nfullscreen=0\nfov=73\nfps_limit=60\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'


def digest(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as stream:
  for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
 return h.hexdigest()


def pe_sections(path):
 """Hash actual PE section contents; exclude the timestamp-bearing PE header."""
 data=Path(path).read_bytes()
 if data[:2]!=b'MZ':raise ValueError('Not a PE executable: '+str(path))
 pe=struct.unpack_from('<I',data,0x3c)[0]
 if data[pe:pe+4]!=b'PE\0\0':raise ValueError('Invalid PE signature')
 count=struct.unpack_from('<H',data,pe+6)[0];optional=struct.unpack_from('<H',data,pe+20)[0]
 if not 1<=count<=96:raise ValueError('Invalid PE section count')
 result={}
 for index in range(count):
  entry=pe+24+optional+index*40;name=data[entry:entry+8].split(b'\0',1)[0].decode('ascii')
  virtual_size,virtual_address,size,offset=struct.unpack_from('<IIII',data,entry+8)
  if offset+size>len(data):raise ValueError('PE section exceeds file: '+name)
  if name in result:raise ValueError('Duplicate PE section '+name)
  result[name]={'virtual_address':virtual_address,'virtual_size':virtual_size,'raw_size':size,'sha256':hashlib.sha256(data[offset:offset+size]).hexdigest()}
 return result


def buildid_bytes(path):
 data=Path(path).read_bytes();pe=struct.unpack_from('<I',data,0x3c)[0];count=struct.unpack_from('<H',data,pe+6)[0];optional=struct.unpack_from('<H',data,pe+20)[0]
 for index in range(count):
  entry=pe+24+optional+index*40
  if data[entry:entry+8].rstrip(b'\0')!=b'.buildid':continue
  virtual_size,rva,size,offset=struct.unpack_from('<IIII',data,entry+8);raw=data[offset:offset+size];directory=struct.unpack_from('<IIHHIIII',raw)
  assert virtual_size==53 and directory[0]==0 and directory[2:6]==(0,0,2,25) and directory[6]==rva+28 and directory[7]==offset+28,('Unexpected PE debug identity structure',directory)
  assert raw[28:32]==b'RSDS' and raw[48:53]==b'\x01\0\0\0\0','Unexpected CodeView age/PDB record'
  normalized=raw[:4]+b'\0'*4+raw[8:]
  return normalized,{'timestamp':directory[1],'type':directory[4],'codeview_signature':'RSDS','guid_hex':raw[32:48].hex(),'age':1,'pdb_path':''}
 raise AssertionError('Missing expected PE .buildid section')


def compare_pe(path,reference,report):
 a=pe_sections(path);b=pe_sections(reference);report['built_pe_sections']=a;report['reference_pe_sections']=b;report['pe_sections_equal']=a==b
 assert set(a)==set(b),'PE section sets differ'
 game_names=sorted(set(a)-{'.buildid'});report['exact_game_sections']=game_names;report['code_data_sections_equal']=all(a[name]==b[name] for name in game_names)
 first,first_debug=buildid_bytes(path);second,second_debug=buildid_bytes(reference)
 report['buildid_metadata']={'built':first_debug,'reference':second_debug,'normalized_equal':first==second,'only_allowed_difference':'IMAGE_DEBUG_DIRECTORY.TimeDateStamp at .buildid offset4 size4; RSDS GUID/age/PDB and every other section byte must match.'}
 report['pe_sections_accepted']=report['code_data_sections_equal'] and first==second
 assert report['pe_sections_accepted'],'Native code/data or non-timestamp PE debug identity differs from the accepted reference'


def same_path(a,b):return os.path.normcase(os.path.abspath(a))==os.path.normcase(os.path.abspath(b))


def write_atomic(path,text):
 pending=path.with_suffix(path.suffix+'.pending');pending.write_text(text,encoding='utf8');deadline=time.monotonic()+2
 while True:
  try:os.replace(pending,path);return
  except PermissionError:
   if time.monotonic()>deadline:raise
   time.sleep(.02)


class ExistingProcess:
 """Track only the private PID actually returned by platform::launch."""
 def __init__(self,pid):
  self.pid=int(pid);self.returncode=None;self.api=ctypes.windll.kernel32
  self.api.OpenProcess.argtypes=(ctypes.c_ulong,ctypes.c_int,ctypes.c_ulong);self.api.OpenProcess.restype=ctypes.c_void_p
  self.handle=self.api.OpenProcess(0x1000|0x100000|0x1,False,self.pid)
  if not self.handle:raise OSError('Cannot track launcher child PID '+str(pid))
  self.api.WaitForSingleObject.argtypes=(ctypes.c_void_p,ctypes.c_ulong);self.api.GetExitCodeProcess.argtypes=(ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong))
  self.api.TerminateProcess.argtypes=(ctypes.c_void_p,ctypes.c_uint)
 def image_path(self):
  size=ctypes.c_ulong(32768);buf=ctypes.create_unicode_buffer(size.value);self.api.QueryFullProcessImageNameW.argtypes=(ctypes.c_void_p,ctypes.c_ulong,ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_ulong))
  if not self.api.QueryFullProcessImageNameW(self.handle,0,buf,ctypes.byref(size)):raise OSError('Cannot query actual game image')
  return buf.value
 def poll(self):
  if self.returncode is None and self.api.WaitForSingleObject(self.handle,0)==0:
   code=ctypes.c_ulong();assert self.api.GetExitCodeProcess(self.handle,ctypes.byref(code));self.returncode=int(code.value)
  return self.returncode
 def wait(self,timeout=None):
  until=time.monotonic()+timeout if timeout is not None else float('inf')
  while self.poll() is None:
   if time.monotonic()>until:raise subprocess.TimeoutExpired(str(self.pid),timeout)
   time.sleep(.05)
  return self.returncode
 def terminate(self):
  if self.poll() is None and not self.api.TerminateProcess(self.handle,1):raise OSError('Cannot terminate private game PID')


class RolePaths:
 def __init__(self,root,paths):self.root=root;self.paths=paths
 def __truediv__(self,name):return self.paths.get(str(name),self.root/str(name))


class LauncherHarness:
 def __init__(self,a,base,report):
  self.a=a;self.base=base;self.report=report;self.sequence=0;self.cache={};self.process=None;self.children=[];self.stream=None;self.visible=[];self.focused=[];self.last_phase=None
 def events(self):
  result=[];path=self.base/PROCESS_LOG
  if path.exists():
   for line in path.read_text(encoding='utf8').splitlines():
    try:result.append(json.loads(line))
    except ValueError:pass # Consumer may be appending its last line.
  return result
 def state(self):
  try:
   self.cache=json.loads((self.base/LAUNCHER_STATE).read_text(encoding='utf8'))
   if 'game_ready' in self.cache:self.cache['ready']=self.cache['game_ready']
   if 'game_outdated' in self.cache:self.cache['outdated']=self.cache['game_outdated']
  except (OSError,ValueError):pass
  phase=self.cache.get('phase')
  if phase!=self.last_phase:
   self.report.setdefault('phase_transitions',[]).append({'phase':phase,'build_step':self.cache.get('build_step'),'progress':self.cache.get('progress'),'at':time.monotonic()});self.last_phase=phase
  return self.cache
 def inspect(self):
  processes=[p for p in [self.process,*self.children] if p is not None];pids={p.pid for p in processes};pids.update(int(e['pid']) for e in self.events() if e.get('pid'))
  u=ctypes.windll.user32;u.GetForegroundWindow.restype=ctypes.c_void_p;u.GetWindowThreadProcessId.argtypes=(ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong));u.IsWindowVisible.argtypes=(ctypes.c_void_p,)
  callback=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p);u.EnumWindows.argtypes=(callback,ctypes.c_void_p);foreground=u.GetForegroundWindow()
  @callback
  def visit(hwnd,_):
   pid=ctypes.c_ulong();u.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
   if pid.value in pids:
    if u.IsWindowVisible(hwnd):self.visible.append(int(hwnd))
    if hwnd==foreground:self.focused.append(int(hwnd))
   return True
  u.EnumWindows(visit,0)
  if self.visible or self.focused:raise RuntimeError('Private launcher/build/game window became visible or focused')
 def delay(self,seconds):
  until=time.monotonic()+seconds
  while time.monotonic()<until:self.inspect();time.sleep(.03)
 def wait(self,predicate,timeout=30):
  until=time.monotonic()+timeout
  while not predicate():
   self.inspect()
   if self.process and self.process.poll() is not None:raise RuntimeError('Launcher exited before its UI task completed: '+str(self.process.returncode))
   if time.monotonic()>until:raise TimeoutError(json.dumps(self.state(),ensure_ascii=False))
   self.delay(.05)
 def build_ready(self,target):
  state=self.state()
  if state.get('is_error') and str(state.get('phase','')).lower()=='idle':raise RuntimeError('Actual launcher build failed: '+str(state.get('message')))
  return target.is_file() and state.get('ready') and not state.get('is_error')
 def command(self,action):
  self.sequence+=1;before=self.state();write_atomic(self.base/LAUNCHER_COMMAND,str(self.sequence)+' '+action+'\n')
  self.wait(lambda:self.state().get('accepted_sequence',0)>=self.sequence,10);self.delay(.06)
  row={'sequence':self.sequence,'action':action,'before':before,'after':self.state(),'at':time.monotonic()}
  with (self.a.output/'launcher-ui-events.jsonl').open('a',encoding='utf8') as stream:stream.write(json.dumps(row,ensure_ascii=False)+'\n')
  return self.state()
 def row(self,ident_value):
  rows=[row for row in self.state().get('rows',[]) if int(row['id'])==ident_value]
  if len(rows)!=1:raise AssertionError('Expected one actual drawn launcher row '+str(ident_value)+' '+json.dumps(self.state()))
  row=rows[0];bounds=row.get('bounds',[])
  assert row.get('enabled') and len(bounds)==4 and bounds[2]>bounds[0] and bounds[3]>bounds[1],row
  return row
 def click(self,ident_value):
  row=self.row(ident_value);left,top,right,bottom=row['bounds'];x=(left+right)*.5;y=(top+bottom)*.5
  self.command(f'move {x} {y}');self.command(f'down left {x} {y}');self.command(f'up left {x} {y}')
  self.report.setdefault('ui_clicks',[]).append({'id':ident_value,'label':row['label'],'bounds':row['bounds'],'point':[x,y]})
 def shot(self,name):self.command('shot '+str(self.a.output/(name+'.ppm')))
 def start(self):
  # A resumed private sandbox must not acknowledge clicks from a previous run.
  for name in (LAUNCHER_COMMAND,LAUNCHER_STATE):
   path=self.base/name
   if path.exists():path.unlink()
  mp=self.base/'Multiplayer';mp.mkdir(exist_ok=True);(mp/'steps').mkdir(exist_ok=True);(mp/'input.txt').write_text('');(mp/'command.txt').write_text('')
  (mp/COMMAND_FILE).write_text('')
  for name in ('native-menu-state.json','native-menu-mouse-state.json','native-mouse-input-state.json','native-carousel-mouse-state.json','native-lobby-mouse-state.json'):
   path=mp/name
   if path.exists():path.unlink()
  env={key:value for key,value in os.environ.items() if not key.startswith('SSNE_')}
  env.update(SSNE_TEST_HIDDEN='1',SSNE_NO_UPDATE_CHECK='1',SSNE_LAUNCHER_TEST_COMMAND=str(self.base/LAUNCHER_COMMAND),SSNE_DEV_NATIVE_MENU='1',SSNE_TEST_DEVICE_INPUT='1',SSNE_NATIVE_MOUSE_COMMAND=COMMAND_FILE,SSNE_PAD_SCRIPT_FILE='input.txt',SSNE_NATIVE_MENU_COMMAND='command.txt',SSNE_TEST_SCREENSHOT_DIR='steps',SSNE_PERF_LOG='performance.jsonl',SSNE_NATIVE_TEST_LANGUAGE='1',SSNE_NATIVE_MENU_PORT=str(self.a.port),SSNE_NATIVE_MENU_DISCOVERY_PORT=str(self.a.port+1))
  assert 'SSNE_COOP_TEST_SKIP_DELAY_MS' not in env and 'SSNE_TEST_SKIP_FIRST_VOTE' not in env
  startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
  self.stream=(self.a.output/'launcher-console.log').open('wb');unrelated=self.a.output/'unrelated-startup-directory';unrelated.mkdir(exist_ok=True)
  self.process=subprocess.Popen([str(self.base/self.a.launcher_name)],cwd=unrelated,env=env,stdout=self.stream,stderr=self.stream,startupinfo=startup,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
  self.wait(lambda:self.state().get('frame',0)>0 and self.state().get('rows'),30);self.shot('launcher-before-pointer')
 def close(self):
  if self.process and self.process.poll() is None:
   try:self.command('quit');self.process.wait(timeout=10)
   except Exception:self.process.terminate();self.process.wait(timeout=10)
  for process in self.children:
   if process.poll() is None:process.terminate();process.wait(timeout=8)
  if self.stream:self.stream.close()


class AttachedMenus(MultiplayerMenus):
 def __init__(self,a,root,host_path,launcher,host_process):
  super().__init__(a,'',root);self.folder=RolePaths(root,{'host':host_path,'client':root/'client'});self.launcher=launcher;self.processes['host']=host_process
 def inspect(self):self.launcher.inspect();super().inspect()


def copy_package(a,report):
 seed=a.package;keep_private_cache=False
 if a.repair_from_cold:
  prior=json.loads((a.repair_from_cold/'result.json').read_text(encoding='utf8'));assert a.case=='repair' and prior.get('passed') and prior.get('case')=='cold','Repair cache must come from a completed passing private cold case'
  seed=Path(prior['private_base']);keep_private_cache=True;report['repair_cache_source']=str(a.repair_from_cold)
 base=a.output/('Launcher Test' if a.ascii_package_path else 'Launcher Prüfung Ü mit Leerzeichen');base.mkdir()
 required=(a.launcher_name,'known_dumps.txt')
 for name in required:shutil.copy2(seed/name,base/name)
 for path in seed.glob('*.dll'):shutil.copy2(path,base/path.name)
 def ignored(directory,names):return [name for name in names if name in ('downloads','__pycache__') or name.startswith('multiplayer.build') or (name in ('work','native-multiplayer-work') and not keep_private_cache) or name.endswith('.pending')]
 shutil.copytree(seed/'build-kit',base/'build-kit',ignore=ignored)
 # These files are native runtime/source inputs, never translated game code.
 patch=seed/'source-multiplayer/tools/patch_multiplayer.py'
 if patch.exists():(base/'source-multiplayer/tools').mkdir(parents=True);shutil.copy2(patch,base/'source-multiplayer/tools/patch_multiplayer.py')
 mp=base/'Multiplayer';mp.mkdir()
 for name in ('release-manifest.json','skins.bin'):
  source=seed/'Multiplayer'/name
  if source.exists():shutil.copy2(source,mp/name)
 if a.case in ('adopt','repair'):
  assert (seed/'game_data/sys/main.dol').is_file(),'Adopt/repair requires installed owned PAL data'
  shutil.copytree(seed/'game_data',base/'game_data')
  for name in ('SSNE-Native-Multiplayer-Game.exe','SSNE-Native-Multiplayer.exe'):
   if (seed/'Multiplayer'/name).exists():shutil.copy2(seed/'Multiplayer'/name,mp/name)
  # Retain a real recipe/build stamp if the staged package contains one.
  source=seed/'build-kit/built-with.txt'
  if source.exists():shutil.copy2(source,base/'build-kit/built-with.txt')
 if a.overlay:
  # Apply the frozen integration overlay onto the private installed-package
  # copy. Never seed a cold build with an executable or extracted game data.
  copied=[]
  for source in a.overlay.rglob('*'):
   if not source.is_file():continue
   relative=source.relative_to(a.overlay)
   if any(part in ('work','native-multiplayer-work','downloads','__pycache__','.git') for part in relative.parts):continue
   if relative.parts[0] not in ('build-kit','source-multiplayer','Multiplayer') and relative.name not in (a.launcher_name,'known_dumps.txt') and source.suffix.lower()!='.dll':continue
   if relative.parts[0]=='Multiplayer' and relative.name not in ('release-manifest.json','skins.bin','SSNE-Native-Multiplayer-Game.exe','SSNE-Native-Multiplayer.exe'):continue
   if a.case!='adopt' and relative.name in ('SSNE-Native-Multiplayer-Game.exe','SSNE-Native-Multiplayer.exe'):continue
   destination=base/relative;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,destination);copied.append(relative.as_posix())
  report['integration_overlay']=str(a.overlay);report['overlay_files']=copied
 for directory in (base/'saves',mp/'saves'):directory.mkdir(exist_ok=True)
 shutil.copy2(a.card,base/'saves/MemoryCardA.EUR.raw');shutil.copy2(a.card,mp/'saves/Host-MemoryCardA.EUR.raw')
 (mp/'settings.ini').write_text(SETTINGS);(mp/'settings-netplay.ini').write_text(SETTINGS)
 cfg='rom_path='+str(a.disc)+'\nrom_sha1='+a.disc_sha1+'\ngame_id=G3BP9G\ninstalled='+('0' if a.case=='cold' else '1')+'\nclose_on_launch=0\nfullscreen=0\ncheck_updates=0\nskip_version=\n'
 (base/'launcher.ini').write_text(cfg,encoding='utf8')
 target=base/MP_REL
 if a.case=='cold':
  assert not target.exists() and not (base/'game_data').exists()
  assert not any(path.is_file() for path in (base/'build-kit').rglob('*') if any(part in ('work','native-multiplayer-work') for part in path.relative_to(base/'build-kit').parts))
 if a.case=='repair':
  # Remove only the two resolved private-copy game names, preventing a valid
  # compatibility alias from satisfying repair without a real cold rebuild.
  report['private_repair_removed']=[]
  for path in (target,mp/'SSNE-Native-Multiplayer.exe'):
   assert path.resolve().is_relative_to(base.resolve())
   if path.exists():path.unlink();report['private_repair_removed'].append(str(path))
 report['private_base']=str(base);report['cold_no_game_exe_or_extracted_data']=a.case=='cold';report['translated_game_cache_seeded']=keep_private_cache;report['cache_seed_scope']='Only the separate repair case may copy objects from a completed passing private cold build.'
 return base


def wait_main(m,p,role='host'):
 deadline=time.monotonic()+90
 while not(m.state(role).get('scene')==3 and any(ident(s['root'])==0x3d for s in p.observe(role).get('surfaces',[]))):
  if '[port] Stopped:' in m.log(role):raise RuntimeError('Actual launcher child stopped: '+next(line for line in m.log(role).splitlines() if '[port] Stopped:' in line))
  if m.processes[role].poll() is not None:raise RuntimeError('Actual launcher child exited before native Main: '+str(m.processes[role].returncode))
  if time.monotonic()>deadline:raise TimeoutError('Actual launcher child did not reach native Main: '+str(m.state(role)))
  if any(ident(surface['root'])==0xb7 for surface in p.observe(role).get('surfaces',[])):
   # An empty native card first opens the game's original language selector.
   # Choose the test's German language through its real native mouse widget.
   m.wait(lambda:p.ack(role).get('active'),15);m.press(role,'UISHOT')
   p.click(0xba,role,root=0xb7)
   p.report.setdefault('fresh_card_setup',[]).append({'role':role,'original_root':0xb7,'language_widget':0xba,'method':'native mouse'})
  if m.state(role).get('scene')==2:m.press(role,'B') # Qualified initial game boot only; independent of launcher UI proof.
  m.delay(.1)
 m.wait(lambda:p.ack(role).get('active'),15);m.press(role,'UISHOT')


def native_smoke(a,h,launch,report):
 target=h.base/MP_REL;host=ExistingProcess(launch['pid']);h.children.append(host);assert same_path(host.image_path(),target),('Actual child image mismatch',host.image_path(),target)
 argv=list(launch.get('argv',[]))
 if argv and same_path(argv[0],target):argv=argv[1:]
 assert '--data' in argv and same_path(argv[argv.index('--data')+1],h.base/'game_data'),launch
 assert same_path(launch['cwd'],h.base/'Multiplayer'),launch
 assert not any(value in argv for value in ('--coop-host','--coop-join','--net-host','--net-join','--memcard')),'Play must reach the ordinary native Main with preserved default save selection'
 report['actual_launch']={**launch,'queried_image_path':host.image_path(),'verified_ordinary_main_args':True}
 game_args=argparse.Namespace(exe=target,data=h.base/'game_data',card=h.base/'Multiplayer/saves/Host-MemoryCardA.EUR.raw',private_settings=SETTINGS,fps=60,port=a.port,language=1,client_language=2,test_env={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_NATIVE_MOUSE_COMMAND':COMMAND_FILE},allow_start_key_fallback=False,case='lobby',continue_method='mouse')
 m=AttachedMenus(game_args,a.output/'native-peers',h.base/'Multiplayer',h,host);native={'steps':[]};pointer=Pointer(m,native)
 try:
  wait_main(m,pointer);report['native_main_observed']=True;pointer.command('host','cancel')
  settings_line='[port] settings: '+str(h.base/'Multiplayer/settings.ini')
  assert any(same_path(line.split('[port] settings: ',1)[1],h.base/'Multiplayer/settings.ini') for line in m.log('host').splitlines() if '[port] settings: ' in line and 'defaults (' not in line),('Actual settings path missing/mismatched',settings_line)
  report['effective_settings_path']=str(h.base/'Multiplayer/settings.ini')
  stdout=launch.get('stdout_log');assert stdout,'Hidden launcher QA must capture actual game stdout for default card-path proof'
  stdout=Path(stdout);stdout=stdout if stdout.is_absolute() else h.base/stdout
  def card_paths():
   text=stdout.read_text(encoding='utf8',errors='replace') if stdout.exists() else ''
   return re.findall(r'\[exi\] (?:created )?memory card A: (.+?) \(',text)
  m.wait(lambda:bool(card_paths()),10);cards=card_paths()
  assert same_path(cards[-1],game_args.card),('Actual default card path mismatch',cards,game_args.card);report['effective_memcard_path']=cards[-1]
  if a.smoke in ('menu','singleplayer'):
   pointer.click(0x3f,root=0x3d);m.wait(lambda:m.state().get('page')==1,15);report['native_multiplayer_home_observed']=True;pointer.click(0x14d);pointer.expect_root(0x3d)
  if a.smoke=='host-client':
   host_create(pointer);client_join_and_roles(pointer);begin_campaign(pointer);report['new_artifact_both_gameplay_ready']={role:m.performance(role) for role in ('host','client')};leave_sessions(pointer)
  elif a.smoke=='singleplayer':
   pointer.click(0x3e,root=0x3d);select_original_profile(pointer,footer=True);pointer.expect_root(0x4b);m.wait(lambda:len(m.carousel().get('surfaces',[]))==1,15)
   state=m.carousel()['surfaces'][0];entries=[entry for entry in state.get('entries',[]) if entry.get('level')=='Rlevel1_1'];assert len(entries)==1,state;wanted=ident(entries[0]['index'])
   for _ in range(ident(state['count'])):
    current=ident(m.carousel()['surfaces'][0]['selected'])
    if current==wanted:break
    direction=1 if current<wanted else -1;pointer.wheel(pointer.center(pointer.widget('carousel.confirm',root=0x4b)),-direction);m.wait(lambda:ident(m.carousel()['surfaces'][0]['selected'])==current+direction,8)
   assert m.carousel()['surfaces'][0]['selected_level']=='Rlevel1_1';pointer.click('carousel.confirm',root=0x4b);pointer.expect_root(0x44);pointer.click(0x47,root=0x44)
   m.wait(lambda:m.state().get('scene')==4 and 'IMPORT_Read_Old(levels/Rlevel1_1.ssw) success' in m.log('host') and any(ident(s.get('actor',0))>0 and ident(s.get('slot',-1))==0 for s in m.information('host').get('surfaces',[])),90);assert not m.state().get('active');report['singleplayer_native_start_proven']=True;report['singleplayer_observation']={'native_scene':m.state(),'original_level_import':'Rlevel1_1 success','original_player_info':m.information('host'),'scope':'Offline native scene4, original authored level import success and live original Info owner/player actor slot0; offline mode does not emit co-op performance telemetry.'};m.press('host','UISHOT')
  report['native_smoke']=native
 finally:
  report['native_final_states']={role:m.state(role) for role in m.processes};m.close();report['native_exits']={role:process.returncode for role,process in m.processes.items()};report['native_visible']=m.visible;report['native_focused']=m.focused
  assert not m.visible and not m.focused and all(code==0 for code in report['native_exits'].values()),report['native_exits']


def main():
 parser=argparse.ArgumentParser(description=__doc__)
 for name in ('package','disc','card','output'):parser.add_argument('--'+name,type=Path,required=True)
 parser.add_argument('--resume-built-from',type=Path,help='Resume Play after a prior completed cold build stopped only at its metadata comparison; prior build proof is retained.');parser.add_argument('--ascii-package-path',action='store_true',help='Run the pinned197 game under an ASCII package path with spaces; Unicode backend proof is separate.');parser.add_argument('--repair-from-cold',type=Path,help='Repair only: copy the verified private package/cache from a prior passed cold-case output.');parser.add_argument('--build-via-play',action='store_true',help='Repair only: click Play once and require build followed by exactly one automatic native-game launch.');parser.add_argument('--overlay',type=Path,help='Frozen integration overlay applied to a private copy of --package.');parser.add_argument('--launcher-name',default='SSNE-Native.exe');preparation=parser.add_mutually_exclusive_group();preparation.add_argument('--prepare-only',action='store_true',help='Only copy the private sandbox and write preparation.json; no launch/build/test acceptance.');preparation.add_argument('--use-prepared',action='store_true',help='Run a previously prepared sandbox at --output without making another package copy.');parser.add_argument('--case',choices=('cold','adopt','repair'),default='cold');parser.add_argument('--smoke',choices=('menu','host-client','singleplayer'),default='host-client');parser.add_argument('--reference-exe',type=Path);parser.add_argument('--disc-sha1',default='70296be19673cc0216f45f6040541a81e507497d');parser.add_argument('--port',type=int,default=27980);parser.add_argument('--build-timeout',type=int,default=1800);a=parser.parse_args()
 for name in ('package','disc','card','output'):setattr(a,name,getattr(a,name).resolve())
 if a.reference_exe:a.reference_exe=a.reference_exe.resolve()
 if a.overlay:a.overlay=a.overlay.resolve()
 if a.repair_from_cold:a.repair_from_cold=a.repair_from_cold.resolve()
 if a.resume_built_from:a.resume_built_from=a.resume_built_from.resolve()
 if a.resume_built_from and (a.case!='cold' or not a.use_prepared):raise ValueError('--resume-built-from requires the cold --use-prepared case')
 if (a.build_via_play or a.repair_from_cold) and a.case!='repair':raise ValueError('--build-via-play/--repair-from-cold are only valid for the repair case')
 if a.use_prepared:assert (a.output/'preparation.json').is_file(),'--use-prepared requires its saved preparation.json'
 else:
  assert not a.output.exists(),'Each proof case needs a new private output directory';a.output.mkdir(parents=True)
 protected=[a.disc,a.card,a.package/'launcher.ini']+[path for root in (a.package/'saves',a.package/'Multiplayer/saves') if root.exists() for path in root.rglob('*') if path.is_file()]+[path for path in (a.package/'settings.ini',a.package/'Multiplayer/settings.ini',a.package/'Multiplayer/settings-netplay.ini') if path.exists()]
 before={str(path):digest(path) for path in protected if path.is_file()};report={'passed':False,'case':a.case,'smoke':a.smoke,'driver_sha256':digest(Path(__file__)),'source_launcher_sha256':digest(a.package/a.launcher_name),'ascii_package_path':a.ascii_package_path,'path_scope':'Pinned197 game runtime acceptance uses an ASCII private path with spaces; failed Unicode runtime I/O is retained separately and no Unicode gameplay claim is made.' if a.ascii_package_path else 'Unicode+spaces private package probe.','protected_before':before,'steps':[],'scope':__doc__};h=None
 try:
  if a.use_prepared:
   prepared=json.loads((a.output/'preparation.json').read_text(encoding='utf8'));assert prepared['case']==a.case and same_path(prepared['disc'],a.disc) and same_path(prepared['card'],a.card),'Prepared sandbox inputs differ from this test case'
   base=Path(prepared['private_base']);assert base.resolve().is_relative_to(a.output.resolve());assert (base/a.launcher_name).is_file();report['private_base']=str(base);report['translated_game_cache_seeded']=prepared.get('translated_game_cache_seeded',False);report['used_prepared_sandbox']=True;report['prepared_package_provenance']=prepared.get('reused_private_package_from','Fresh prepare-only sandbox.')
  else:base=copy_package(a,report)
  report['staged_launcher_sha256']=digest(base/a.launcher_name)
  if a.prepare_only:
   after={path:digest(Path(path)) if Path(path).is_file() else None for path in before};assert after==before,'Preparation modified protected source files'
   report.update(preparation_only=True,acceptance_executed=False,disc=str(a.disc),card=str(a.card),protected_after=after,original_disc_saves_settings_unchanged=True)
   (a.output/'preparation.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf8');print(json.dumps({'prepared':True,'acceptance_executed':False,'private_base':str(base)},ensure_ascii=False),flush=True);return 0
  h=LauncherHarness(a,base,report);h.start();initial=h.state();report['initial_launcher_state']=initial
  prior_build=None
  if a.resume_built_from:
   prior_build=json.loads(a.resume_built_from.read_text(encoding='utf8'));assert prior_build.get('case')=='cold' and prior_build.get('cold_build_completed') and prior_build.get('built_exe_sha256')==digest(base/MP_REL) and prior_build.get('staged_launcher_sha256')==report['staged_launcher_sha256'],'Prior cold build/artifact/launcher identity changed'
   report['resumed_after_metadata_comparison']=True;report['prior_build_proof']=str(a.resume_built_from);report['build_process_events']=prior_build['build_process_events'];report['prior_build_ui_clicks']=prior_build.get('ui_clicks');report['cold_build_completed']=True;report['prior_build_phase_transitions']=prior_build.get('phase_transitions')
  if a.case=='cold' and not prior_build:
   h.command('drop '+str(a.disc));h.wait(lambda:bool(h.state().get('rows')) and any(int(row['id'])==2 and row.get('enabled') for row in h.state()['rows']) and bool(h.state().get('message')),180);h.shot('verified-owned-PAL')
   assert not h.state().get('is_error'),h.state();assert not (base/MP_REL).exists(),'Cold build target unexpectedly appeared before Build click'
  automatic_launch_marker=None
  if a.case!='adopt' and not prior_build:
   starts=len(h.events());automatic_launch_marker=starts if a.build_via_play else None;h.click(0 if a.build_via_play else 2);h.wait(lambda:h.build_ready(base/MP_REL),a.build_timeout);h.shot('build-complete')
   events=h.events()[starts:];assert any(event.get('kind')=='spawn' for event in events),'No actual compiler/translator subprocesses observed'
   report['build_process_events']=events;report['cold_build_completed']=a.case=='cold';report['repair_build_completed']=a.case=='repair'
  elif a.case=='adopt':
   assert initial.get('ready') and initial.get('game_built'),('Known installed artifact was not immediately adopted',initial);report['adopted_without_build']=not any(event.get('kind')=='spawn' for event in h.events())
   assert report['adopted_without_build']
  target=base/MP_REL;report['built_exe_sha256']=digest(target);report['built_pe_sections']=pe_sections(target)
  if a.reference_exe:
   report['reference_exe_sha256']=digest(a.reference_exe);compare_pe(target,a.reference_exe,report)
  marker=automatic_launch_marker
  if marker is None:marker=len(h.events());h.click(0)
  h.wait(lambda:any(event.get('kind')=='launch' and same_path(event.get('exe',''),target) for event in h.events()[marker:]),20)
  launch=next(event for event in h.events()[marker:] if event.get('kind')=='launch' and same_path(event.get('exe',''),target));native_smoke(a,h,launch,report)
  launches=[event for event in h.events()[marker:] if event.get('kind')=='launch' and same_path(event.get('exe',''),target)];assert len(launches)==1,('A single Play intent launched the game more than once',launches);report['build_via_play']=a.build_via_play;report['single_play_single_launch']=True
  report['passed']=True
 except Exception:
  report['error']=traceback.format_exc();print(report['error'],flush=True)
  if h:
   report['failure_launcher_state']=h.state();report['failure_process_events']=h.events()
   if h.process and h.process.poll() is None:
    try:h.shot('failure')
    except Exception:pass
 finally:
  if h:
   h.close();report.update(launcher_exit=h.process.returncode if h.process else None,visible=h.visible,focused=h.focused,process_events=h.events())
  after={path:digest(Path(path)) if Path(path).is_file() else None for path in before};report['protected_after']=after;report['original_disc_saves_settings_unchanged']=before==after
  report['passed'] &= before==after and not report.get('visible') and not report.get('focused') and report.get('launcher_exit')==0
  if not report.get('preparation_only'):
   (a.output/'result.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf8');print(json.dumps({key:report.get(key) for key in ('passed','case','smoke','built_exe_sha256','pe_sections_equal','pe_sections_accepted','error')},indent=2),flush=True)
 return 0 if report['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
