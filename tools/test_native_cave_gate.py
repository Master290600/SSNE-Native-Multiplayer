"""Read-only cave-gate diagnostics in private native peers.

Initial scan only captures the normal level startup. Optional file plans use
ordinary pad input and an explicitly recorded native player-position fixture;
there are no door, event, enemy-damage, spawn or level-link shortcuts.
"""
from test_game_bots import Bots
from test_coop_campaign import sha
from pathlib import Path
import argparse,json,math,os,shutil,struct,time,traceback

RAM_BASE=0x80000000
TRIGGER=0x80209758
SWITCH=0x80208B60
BRUSH=0x80206868
PLAYER=0x802121E8

class Ram:
 def __init__(self,path):
  self.data=Path(path).read_bytes()
  if len(self.data)!=64*1024*1024:raise ValueError('Expected a complete 64 MiB guest RAM capture')
  if self.u32(RAM_BASE)!=0x47334250:raise ValueError('Expected the PAL G3BP guest capture')
 def valid(self,address,size=4):return RAM_BASE<=address<=RAM_BASE+len(self.data)-size
 def u32(self,address):
  if not self.valid(address):raise ValueError('Invalid guest address '+hex(address))
  return struct.unpack_from('>I',self.data,address-RAM_BASE)[0]
 def u16(self,address):
  if not self.valid(address,2):raise ValueError('Invalid guest address '+hex(address))
  return struct.unpack_from('>H',self.data,address-RAM_BASE)[0]
 def f32(self,address):return struct.unpack('>f',struct.pack('>I',self.u32(address)))[0]
 def vector(self,address):return [self.f32(address+i*4) for i in range(3)]
 def words(self,address,size):
  return {hex(i):self.u32(address+i) for i in range(0,size,4)} if self.valid(address,size) else {}

def parse_capture(ram_path,state):
 ram=Ram(ram_path);table=ram.u32(0x802DB7C4);count=ram.u32(0x802DB7C8);capacity=ram.u32(0x802DB7CC)
 if count>capacity or count>32768 or not ram.valid(table,count*8):raise ValueError('Invalid native level script map')
 indexed={ram.u16(table+i*8):ram.u32(table+i*8+4) for i in range(count)}
 reverse={address:index for index,address in indexed.items()}
 objects={int(o['address']):dict(o) for o in state.get('objects',[])}
 for index,address in indexed.items():
  if ram.valid(address,0x58):objects.setdefault(address,{'address':address,'index':index,'vtable':ram.u32(address+0x48),'position':ram.vector(address+0x20)})
 result={'level':state.get('level'),'epoch':state.get('epoch'),'tick':state.get('tick'),'ram_sha256':sha(Path(ram_path)),'script_map':{'address':table,'count':count,'capacity':capacity},'players':[],'doors':[],'scripts':[],'other_indexed_objects':[]}
 for address,obj in objects.items():
  if not ram.valid(address,0x58):continue
  obj.update(vtable=ram.u32(address+0x48),index=ram.u32(address+0x50),position=ram.vector(address+0x20))
  vt=obj['vtable'];config=ram.u32(address+0x54)
  if vt==PLAYER and ram.valid(address,0x790):
   obj.update(slot=ram.u32(address+0x17C),health=ram.f32(address+0xCC),dead=ram.u32(address+0x328),weapon=ram.u32(address+0x5DC),native_words=ram.words(address,0x790));result['players'].append(obj)
  elif vt==BRUSH and ram.valid(address,0x13C):
   brush_config=ram.u32(address+0x5C)
   obj.update(configuration=brush_config,brush_resource=ram.u32(brush_config+0xC) if ram.valid(brush_config,0x38) else None,brush_segment=ram.u32(address+0xC8),brush_progress=ram.f32(address+0xC0),brush_active=ram.u32(address+0xB0),brush_completed=ram.u32(address+0x58),native_words=ram.words(address,0x13C),configuration_words=ram.words(brush_config,0x38));result['doors'].append(obj)
  elif vt in (TRIGGER,SWITCH):
   obj.update(configuration=config,script_enabled=ram.u32(address+0x4C),configuration_words=ram.words(config,0x80))
   refs=[ram.u32(address+0x58+i*4) for i in range(11 if vt==TRIGGER else 1)]
   obj['reference_addresses']=refs;obj['script_references']=[-1 if not x else reverse.get(x) for x in refs]
   if vt==TRIGGER:
    for key,offset in [('trigger_previous_enabled',0x88),('trigger_enabled',0x8C),('trigger_auto_start',0x9C),('trigger_completed',0xA0),('trigger_dispatched',0xA4),('trigger_pulses',0x90),('trigger_firings',0x94)]:obj[key]=ram.u32(address+offset)
    obj['trigger_timer']=ram.f32(address+0x98);obj['native_words']=ram.words(address,0xA8)
   else:obj.update(switch_enabled=ram.u32(address+0x60),switch_available=ram.u32(address+0x64),native_words=ram.words(address,0x68))
   result['scripts'].append(obj)
  elif obj['index'] in indexed:result['other_indexed_objects'].append(obj)
 for key in ('players','doors','scripts','other_indexed_objects'):result[key].sort(key=lambda o:(o.get('index',0),o['address']))
 return result

class CavePeers(Bots):
 def wait(self,predicate,timeout=60):
  until=time.monotonic()+timeout
  while not predicate():
   self.inspect()
   for role,process in self.processes.items():
    if process.poll() is not None:raise RuntimeError(role+' exited '+str(process.returncode))
    if '[port] Stopped:' in self.log(role):raise RuntimeError(role+' guest stopped')
   if time.monotonic()>until:raise TimeoutError('State wait exceeded '+str(timeout)+' seconds')
   self.delay(.15)
 def initial_ready(self,roles,timeout=90):
  until=time.monotonic()+timeout;next_skip=time.monotonic()+3
  while not all(self.performance(role).get('local_ready') for role in roles):
   self.inspect()
   for role,process in self.processes.items():
    if process.poll() is not None:raise RuntimeError(role+' exited during initial startup '+str(process.returncode))
    if '[port] Stopped:' in self.log(role):raise RuntimeError(role+' guest stopped during initial startup')
   if time.monotonic()>until:raise TimeoutError('Initial level startup '+str({r:self.performance(r) for r in roles}))
   # Only initial startup may dismiss the original intro/Netricsca prompt.
   if time.monotonic()>=next_skip:
    pending=tuple(r for r in roles if not self.performance(r).get('local_ready'))
    if pending:self.pulse('B',pending)
    next_skip=time.monotonic()+3
   self.delay(.2)
 def snapshot(self,label):
  if not label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in label):raise ValueError('Use an alphanumeric snapshot label')
  folder=self.folder/'snapshots'/label;folder.mkdir(parents=True,exist_ok=False)
  self.pulse('UISHOT');self.command('dump');sequence=self.command_sequence
  self.wait(lambda:'sequence='+str(sequence)+' action=dump' in self.log('host'),15);self.delay(.4)
  state=self.state()
  if state.get('level')!=self.level:raise RuntimeError('Unexpected level '+str(state.get('level')))
  source=self.folder/'host/campaign-capture.ram';self.wait(lambda:source.exists() and source.stat().st_size==64*1024*1024,10)
  shutil.copy2(source,folder/'campaign-capture.ram');shutil.copy2(self.folder/'host/campaign-context.bin',folder/'campaign-context.bin')
  (folder/'campaign-state.json').write_text(json.dumps(state,indent=2),encoding='utf8')
  parsed=parse_capture(folder/'campaign-capture.ram',state);(folder/'native-gate-state.json').write_text(json.dumps(parsed,indent=2,allow_nan=False),encoding='utf8')
  for role in self.processes:(folder/(role+'-view.log')).write_text(self.log(role),encoding='utf8')
  return {'label':label,'folder':str(folder),'tick':state['tick'],'players':len(parsed['players']),'doors':len(parsed['doors']),'scripts':len(parsed['scripts']),'performance':{r:self.performance(r) for r in self.processes}}
 def plan_step(self,step):
  action=step['action']
  if action=='snapshot':return self.snapshot(step['label'])
  if action=='wait':self.delay(float(step['seconds']))
  elif action=='input':
   roles=tuple(step.get('roles',['client']));tokens=step['tokens'];allowed={'A','B','X','Y','Z','START','UP','DOWN','LEFT','RIGHT','SUP','SDOWN','SLEFT','SRIGHT','CUP','CDOWN','CLEFT','CRIGHT','FIRE','FIRE_SOFT','FIRE_LEFT','KEY_TAB','KEY_ENTER'}
   if not roles or any(r not in self.processes for r in roles) or not tokens or any(t not in allowed for t in tokens):raise ValueError('Unsupported pad input plan')
   self.pulse('@0:100000,'.join(tokens),roles,float(step['seconds']))
  elif action=='place':
   slot=int(step['slot']);position=step['position']
   if slot not in (0,1) or len(position)!=3 or any(not math.isfinite(v) or abs(v)>100000 for v in position):raise ValueError('Invalid native position fixture')
   self.command('player_place '+str(slot)+' '+' '.join(str(v) for v in position));sequence=self.command_sequence
   self.wait(lambda:'sequence='+str(sequence)+' action=player_place slot='+str(slot) in self.log('host'),10)
  else:raise ValueError('Unsupported plan action; no gate/damage/event shortcuts are accepted')
  return dict(step)

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--level',default='Rlevel1_2');ap.add_argument('--port',type=int,default=27882);ap.add_argument('--fps',type=int,default=60)
 ap.add_argument('--afk-host',action='store_true');ap.add_argument('--afk-delay-ms',type=int,default=5000)
 ap.add_argument('--plan-file',type=Path);ap.add_argument('--plan-wait-seconds',type=float,default=60);ap.add_argument('--close-client-netricsca',action='store_true');a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.parent.mkdir(parents=True,exist_ok=True);a.bots=0;a.mode=1;a.capacity=8;a.open_menu=False;a.mod_flags=0
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1'};a.client_test_env={'SSNE_TEST_DEVICE_INPUT':'1'}
 if a.afk_host:a.test_env.update(SSNE_COOP_TEST_AFK='1',SSNE_COOP_TEST_AFK_MASK='1',SSNE_COOP_TEST_AFK_DELAY_MS=str(a.afk_delay_ms),SSNE_COOP_TEST_AFK_RETURN_MS='120000')
 p=CavePeers(a,a.level,a.output);before=sha(a.card);r={'passed':False,'exe_sha256':sha(a.exe),'level':a.level,'mode':'private_initial_scan' if not a.plan_file else 'private_normal_input_file_plan','bots':0,'capacity':8,'afk_host':a.afk_host,'afk_delay_ms':a.afk_delay_ms if a.afk_host else None,'snapshots':[],'plan_steps':[],'scope':'Diagnostic capture; startup success does not establish normal cave-door progression. No door activation, damage, event or level-link shortcut is used.'}
 try:
  p.start();p.initial_ready(('host',));p.start_role('client');p.initial_ready(('host','client'))
  p.wait(lambda:all(p.performance(role).get('local_ready') and p.performance(role).get('ready_players')==2 and p.performance(role).get('level')==a.level for role in ('host','client')),90)
  if a.close_client_netricsca:p.pulse('KEY_TAB',('client',))
  if a.afk_host:p.wait(lambda:any('slot=0 ' in line and 'afk=1 ' in line for line in p.log('host').splitlines() if '[coop-afk-state]' in line),max(20,a.afk_delay_ms/1000+10))
  p.delay(2);r['snapshots'].append(p.snapshot('initial'))
  if a.plan_file:
   accepted=0;deadline=time.monotonic()+a.plan_wait_seconds
   while True:
    try:plan=json.loads(a.plan_file.read_text(encoding='utf8'))
    except (OSError,ValueError):plan={}
    sequence=int(plan.get('sequence',0))
    if sequence>accepted:
     for step in plan.get('steps',[]):r['plan_steps'].append(p.plan_step(step))
     accepted=sequence
     (a.output/'plan-accepted.json').write_text(json.dumps({'sequence':accepted,'steps':r['plan_steps']},indent=2),encoding='utf8')
     if plan.get('done'):break
     deadline=time.monotonic()+a.plan_wait_seconds
    if time.monotonic()>deadline:raise TimeoutError('Waiting for the next external file plan; peers are closing safely')
    p.delay(.2)
  r['passed']=bool(r['snapshots'])
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  p.close();r.update(visible=p.visible,focused=p.focused,exits={role:x.returncode for role,x in p.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not p.visible and not p.focused and r['original_card_unchanged'] and all(code==0 for code in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps(r,indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
