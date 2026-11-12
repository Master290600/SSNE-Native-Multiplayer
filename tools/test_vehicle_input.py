"""Private PAL Jeep input regression through the normal KBM conversion.

CLI host/client, intro skip, original level Show and original vehicle Enter are
explicit setup shortcuts. FIRE is a virtual mouse device snapshot; Space/E are
raw SDL scancodes consumed by the test-only key-file adapter. All cards/settings
are private. RAM dumps use the existing hidden campaign command at guest safe
points. No real desktop input, normal menu/LAN coverage or campaign playthrough
is claimed. --expect-bug is a separately labelled baseline reproduction.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,struct,subprocess,time,traceback
from test_coop_campaign import Peers,sha
from test_native_mouse import Pointer,COMMAND_FILE
from test_native_mouse_multiplayer import MultiplayerMenus,close_initial_information_by_mouse

SETTINGS='[graphics]\nrenderer=gpu\nscale=1\naspect=3\nvsync=0\nwindow_w=960\nwindow_h=540\nfps_limit=60\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'

class VehiclePeers(Peers):
 def __init__(self,a):
  super().__init__(a,a.level,a.output/'peers');self.active={'host':[],'client':[]};self.keys={'host':[],'client':[]};self.input_cache={}
 def publish(self,pending,path):
  deadline=time.monotonic()+2
  while True:
   try:os.replace(pending,path);return
   except PermissionError:
    if time.monotonic()>=deadline:raise
    self.delay(.01)  # Retry publication only, never repeat an input intent.
 def script(self,role,tokens):
  self.sequence+=1;path=self.folder/role/'input.txt';pending=path.with_suffix('.pending')
  pending.write_text(','*self.sequence+','.join(t+'@0:100000' for t in tokens),encoding='ascii');self.publish(pending,path)
 def raw(self,role,codes):
  self.keys[role]=list(codes);path=self.folder/role/'keys.txt';pending=path.with_suffix('.pending');pending.write_text(' '.join(map(str,codes)),encoding='ascii');self.publish(pending,path)
 def hold(self,role,tokens=(),keys=()):
  self.active[role]=list(tokens);self.script(role,self.active[role]);self.raw(role,keys)
 def pulse(self,token,roles=('host','client'),seconds=.18):
  # Put one-shot events first: their unique prefix positions never collide with
  # a later one-shot in this process, even when FIRE remains held afterwards.
  for role in roles:self.script(role,[token,*self.active[role]])
  self.delay(seconds)
  for role in roles:self.script(role,self.active[role])
  self.delay(.2)
 def start(self):
  self.folder.mkdir(parents=True,exist_ok=False)
  for role in ('host','client'):
   folder=self.folder/role;folder.mkdir();(folder/'steps').mkdir();shutil.copy2(self.a.exe,folder/'game.exe');shutil.copy2(self.a.card,folder/'card.raw')
   for name in ('input.txt','keys.txt','command.txt',COMMAND_FILE):(folder/name).write_text('')
   (folder/'settings-netplay.ini').write_text(SETTINGS,encoding='utf8')
   env={k:v for k,v in os.environ.items() if not k.startswith('SSNE_')}
   env.update(SSNE_TEST_HIDDEN='1',SSNE_DEV_NATIVE_MENU='1',SSNE_TEST_DEVICE_INPUT='1',SSNE_TEST_DEVICE_KEY_FILE=str(folder/'keys.txt'),SSNE_NATIVE_MOUSE_COMMAND=str(folder/COMMAND_FILE),SSNE_PAD_SCRIPT_FILE=str(folder/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(folder/'steps'),SSNE_PERF_LOG=str(folder/'performance.jsonl'),SSNE_COOP_TEST_SKIP_DELAY_MS='1500',SSNE_TEST_SKIP_FIRST_VOTE='1',SSNE_COOP_CAMPAIGN_VERIFY='1',SSNE_COOP_TRACE_LIFETIMES='1')
   if role=='host':env.update(SSNE_COOP_CAMPAIGN_COMMAND=str(folder/'command.txt'),SSNE_NETPLAY_MOD_FLAGS='0')
   args=[str(folder/'game.exe'),'--data',str(self.a.data),'--memcard',str(folder/'card.raw'),'--lan-port',str(self.a.port),'--lan-pin','123456','--player-name',role,'--mute','--fps',str(self.a.fps)]+(['--coop-host','--coop-capacity','8','--coop-level',self.a.level] if role=='host' else ['--coop-join','127.0.0.1'])
   startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0;stream=(folder/'console.log').open('wb');self.streams.append(stream)
   self.processes[role]=subprocess.Popen(args,cwd=folder,env=env,stdout=stream,stderr=stream,startupinfo=startup,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
   self.delay(2 if role=='host' else .2)
 def action(self,text):
  self.command(text);sequence=self.command_sequence;self.wait(lambda:'sequence='+str(sequence)+' action='+text.split()[0] in self.log('host'),20)
 def input_state(self,role):
  try:self.input_cache[role]=json.loads((self.folder/role/'native-mouse-input-state.json').read_bytes())
  except (OSError,ValueError):pass
  return self.input_cache.get(role,{})
 def shots(self):
  return re.findall(r'\[coop-projectile-visual-host\] sequence=(\d+) id=(\d+) definition=1(?:\s|$)',self.log('host'))
 def dump(self,label,actors,wrapper):
  self.action('dump');path=self.folder/'host/campaign-capture.ram';size=path.stat().st_size
  with path.open('rb') as ram:
   def word(address):
    offset=address-0x80000000
    if offset<0 or offset+4>size:return 0
    ram.seek(offset);return struct.unpack('>I',ram.read(4))[0]
   def floating(address):return struct.unpack('>f',struct.pack('>I',word(address)))[0]
   car=word(wrapper+0x60);vtable=word(car+0x48);players={}
   for slot,actor in actors.items():players[str(slot)]={'actor':actor,'vehicle':word(actor+0x6c4),'movement_mode':word(actor+0x340)}
   guns=[]
   if vtable==0x80225568:
    for offset in (0x368,0x36c):
     gun=word(car+offset);config=word(gun+0x1d0);mode=word(gun+0x1e0)
     guns.append({'address':gun,'vtable':word(gun+0xc),'state':word(gun+0x1b4),'timer':floating(gun+0x1e8),'attack_rate':floating(config+mode*60+0x1c)})
   result={'label':label,'tick':self.state().get('tick'),'car':car,'car_class':vtable,'driver':word(car+0x160),'players':players,'guns':guns,'input':{r:self.input_state(r) for r in ('host','client')},'projectile1_events':len(self.shots())}
  target=self.a.output/'samples';target.mkdir(exist_ok=True);(target/(label+'.json')).write_text(json.dumps(result,indent=2)+'\n',encoding='utf8');return result

class AttachedMenus(MultiplayerMenus):
 def __init__(self,peers):
  super().__init__(peers.a,'',peers.folder);self.peers=peers;self.processes=peers.processes
 def inspect(self):self.peers.inspect()
 def delay(self,seconds):self.peers.delay(seconds)
 def log(self,role):return self.peers.log(role)
 def pulse(self,*args,**kwargs):return self.peers.pulse(*args,**kwargs)

def retire_original_setup_gui(peers,report):
 menus=AttachedMenus(peers);pointer=Pointer(menus,report)
 menus.wait(lambda:all(menus.state(r).get('scene')==4 and menus.mouse_ack(r) for r in ('host','client')),30)
 close_initial_information_by_mouse(pointer)
 menus.wait(lambda:all(not menus.state(r).get('info') and not menus.mouse(r).get('has_surface') and not menus.mouse_ack(r).get('presentation') and not menus.mouse_ack(r).get('active') for r in ('host','client')),75)
 menus.delay(1)
 report['setup_gui_retired']={r:{'menu':menus.state(r),'mouse':menus.mouse(r),'input':menus.mouse_ack(r)} for r in ('host','client')}
 return peers.state()

def occupied(sample,slot,actors):
 player=sample['players'][str(slot)]
 return bool(sample['car']) and player['vehicle']==sample['car'] and player['movement_mode']==3 and sample['driver']==actors[slot]

def parse():
 parser=argparse.ArgumentParser(description=__doc__)
 for name in ('exe','data','card','output'):parser.add_argument('--'+name,type=Path,required=True)
 parser.add_argument('--level',default='Clevel5_1');parser.add_argument('--port',type=int,default=28640);parser.add_argument('--fps',type=int,default=60);parser.add_argument('--roles',nargs='+',choices=('host','client'),default=('host','client'));parser.add_argument('--expect-bug',action='store_true');a=parser.parse_args()
 for name in ('exe','data','card','output'):setattr(a,name,getattr(a,name).resolve())
 if a.expect_bug and list(a.roles)!=['host']:parser.error('--expect-bug requires --roles host (baseline FIRE only)')
 return a

def main():
 a=parse();assert not a.output.exists(),'Each test needs a new private output directory';a.output.mkdir(parents=True)
 protected=[a.exe,a.card,a.data/'sys/main.dol'];before={str(p):sha(p) for p in protected};report={'passed':False,'mode':'baseline_reproduction' if a.expect_bug else 'fixed_regression','scope':__doc__,'exe_sha256':sha(a.exe),'driver_sha256':sha(Path(__file__)),'level':a.level,'roles':a.roles,'phases':[],'steps':[]};p=VehiclePeers(a)
 try:
  p.start();state=p.stage(a.level);state=retire_original_setup_gui(p,report);actors={int(o['slot']):int(o['address']) for o in state['objects'] if o.get('vtable')==0x802121e8 and o.get('slot') in (0,1)};assert set(actors)=={0,1},actors
  vehicle=next(o for o in state['objects'] if o['vtable']==0x80209bb8);wrapper=int(vehicle['address']);reference=int(vehicle['index']);p.action('event '+str(reference)+' 11');p.delay(.6)
  report['setup']={'cli_bootstrap':True,'automatic_intro_fixture':True,'original_show_event':11,'original_enter_function':'800B54D8','level_vehicle_reference':reference,'native_wrapper':wrapper,'actors':actors,'not_menu_or_LAN_acceptance':True}
  for role in a.roles:
   slot=0 if role=='host' else 1;p.action('drive '+str(reference)+' '+str(slot));p.delay(1.5)
   before_fire=p.dump(role+'-idle-before-fire',actors,wrapper);assert occupied(before_fire,slot,actors),before_fire
   shot_marker=len(p.shots());p.hold(role,['FIRE']);p.delay(3.2);during=p.dump(role+'-fire-held',actors,wrapper);p.pulse('SHOT',(role,),.05)
   p.hold(role);p.delay(.45);after_fire=p.dump(role+'-fire-released',actors,wrapper)
   row={'role':role,'idle_no_look_seconds':1.5,'input':'left mouse via normal KBM conversion','before':before_fire,'during':during,'after':after_fire,'actual_projectile1_events':len(p.shots())-shot_marker}
   if a.expect_bug:
    assert not occupied(during,slot,actors) and not during['players'][str(slot)]['vehicle'] and during['driver']==0,('Baseline bug did not reproduce',during)
    row['unexpected_exit_reproduced']=True;report['phases'].append(row);report['bug_reproduced']=True;continue
   assert occupied(during,slot,actors) and occupied(after_fire,slot,actors),('Fire caused an exit',row)
   assert during['input'][role].get('vehicle_control') is True,('Local native vehicle status not propagated',during)
   assert during['input'][role].get('player_control') is True,('Vehicle was treated as menu input',during)
   assert row['actual_projectile1_events']>0,('No actual original Jeep projectile event',row)
   assert any(g['state']==3 and g['attack_rate']>0 and 0<=g['timer']<1/g['attack_rate']+.2 for g in during['guns']),('No running original Jeep gun fire cycle',during['guns'])
   row['fire_keeps_driver_and_shoots']=True;report['phases'].append(row)
   p.delay(1.1);p.hold(role,keys=[44]);p.delay(.5);space=p.dump(role+'-space-held',actors,wrapper);p.hold(role);p.delay(.25)
   assert occupied(space,slot,actors),('Raw SDL Space caused an exit',space)
   assert space['input'][role].get('vehicle_control') is True and space['input'][role].get('player_control') is True,space
   report['phases'].append({'role':role,'input':'raw SDL scancode44 Space','sample':space,'keeps_driver':True})
   p.hold(role,keys=[8]);p.delay(.25);p.hold(role);p.delay(.45);use=p.dump(role+'-e-use-released',actors,wrapper)
   assert use['players'][str(slot)]['vehicle']==0 and use['players'][str(slot)]['movement_mode']!=3 and use['driver']==0,('E use failed to exit',use)
   p.wait(lambda:p.input_state(role).get('vehicle_control') is False,8)
   report['phases'].append({'role':role,'input':'raw SDL scancode8 E','sample':use,'intentional_exit_works':True})
  report['passed']=True
 except Exception:report['error']=traceback.format_exc();print(report['error'],flush=True)
 finally:
  for role in p.processes:p.hold(role)
  p.close();report['process_exits']={r:c.returncode for r,c in p.processes.items()};report['visible']=p.visible;report['focused']=p.focused;report['protected_before']=before;report['protected_after']={str(q):sha(q) for q in protected};report['original_files_unchanged']=report['protected_before']==report['protected_after'];report['client_verifier_scope']='Existing campaign verifier only covers mission/enemy fields; actual local vehicle status and server-side driver identity are checked separately.'
  report['passed']&=all(code==0 for code in report['process_exits'].values()) and not p.visible and not p.focused and report['original_files_unchanged']
  (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8');print(json.dumps({'passed':report['passed'],'mode':report['mode'],'bug_reproduced':report.get('bug_reproduced',False),'exe_sha256':report['exe_sha256'],'error':report.get('error')}),flush=True)
 return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
