"""Private hidden PAL bot soak. No desktop input and no objective/weapon grants."""
from test_coop_campaign import Peers,sha
import argparse,json,os,shutil,subprocess,time,traceback,math
from pathlib import Path
class Bots(Peers):
 def pulse(self,token,roles=None,seconds=.18):return super().pulse(token,tuple(self.processes) if roles is None else roles,seconds)
 def start_role(self,role):
  f=self.folder/role;f.mkdir();(f/'steps').mkdir();shutil.copy2(self.a.exe,f/self.a.exe.name);shutil.copy2(self.a.card,f/'card.raw');(f/'input.txt').write_text('');(f/'command.txt').write_text('')
  (f/'settings-netplay.ini').write_text('[graphics]\nrenderer=gpu\nscale=1\nvsync=0\nwindow_w=960\nwindow_h=540\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n')
  env={k:v for k,v in os.environ.items() if not k.startswith('SSNE_')};env.update(SSNE_TEST_HIDDEN='1',SSNE_PAD_SCRIPT_FILE=str(f/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(f/'steps'),SSNE_PERF_LOG=str(f/'performance.jsonl'),SSNE_COOP_TEST_SKIP_DELAY_MS='1500',SSNE_TEST_SKIP_FIRST_VOTE='1',SSNE_COOP_TRACE_LIFETIMES='1',SSNE_COOP_CAMPAIGN_VERIFY='1',SSNE_OPEN_MENU='6')
  if role=='host':env.update(getattr(self.a,'test_env',{}))
  else:env.update(getattr(self.a,'client_test_env',{}))
  if not getattr(self.a,'open_menu',True):env.pop('SSNE_OPEN_MENU',None)
  if role=='host':env.update(SSNE_COOP_CAMPAIGN_COMMAND=str(f/'command.txt'),SSNE_NETPLAY_MOD_FLAGS=str(getattr(self.a,'mod_flags',0)))
  args=[str(f/self.a.exe.name),'--data',str(self.a.data),'--memcard',str(f/'card.raw'),'--lan-port',str(self.a.port),'--lan-pin','123456','--player-name',role,'--mute']+(['--coop-host','--coop-capacity',str(getattr(self.a,'capacity',8)),'--bots',str(self.a.bots),'--bot-skill','2']+(['--coop-level',self.level] if self.a.mode==1 else []) if role=='host' else ['--coop-join','127.0.0.1'])
  if getattr(self.a,'fps',None):args+=['--fps',str(self.a.fps)]
  si=subprocess.STARTUPINFO();si.dwFlags|=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=0;stream=(f/'console.log').open('wb');self.streams.append(stream);self.processes[role]=subprocess.Popen(args,cwd=f,env=env,stdout=stream,stderr=stream,startupinfo=si,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
 def start(self):
  self.folder.mkdir(parents=True,exist_ok=False);self.start_role('host')
 def ready(self,level=None):
  s=self.state();return len([o for o in s.get('objects',[]) if 'slot' in o])>=min(self.a.bots+1,8 if self.a.mode==1 else 4) and '[game-bot-goal]' in self.log('host')
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--exe',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);ap.add_argument('--card',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--mode',type=int,default=1);ap.add_argument('--bots',type=int,default=3);ap.add_argument('--level',default='Rlevel1_1');ap.add_argument('--seconds',type=int,default=60);ap.add_argument('--client',action='store_true');ap.add_argument('--encounter',action='store_true');ap.add_argument('--flag-goal',action='store_true');ap.add_argument('--port',type=int,default=27817);a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 p=Bots(a,a.level,a.output);result={'passed':False,'mode':a.mode,'exe_sha256':sha(a.exe),'samples':[]};before=sha(a.card)
 try:
  p.start();p.delay(2);p.pulse('F12',('host',))
  if a.mode!=1:
   p.wait(lambda:'[native-mode-menu] original mode selection opened' in p.log('host'),40);p.delay(2)
   for _ in range(a.mode-1):p.pulse('DOWN',('host',))
   p.pulse('A',('host',));p.delay(2);p.pulse('A',('host',));p.delay(2);p.pulse('START',('host',));p.delay(2);p.pulse('A',('host',))
  p.wait(lambda:p.ready(),90);print('Bots ready mode='+str(a.mode),flush=True)
  if a.flag_goal:
   flag=next(o for o in p.state()['objects'] if o['vtable']==0x80206C80);q=flag['position'];p.command('player_place 1 '+str(q[0]-6)+' '+str(q[1]+1)+' '+str(q[2]));p.delay(2);result['flag_fixture']='Bot 1 placed six metres from the ground flag via original position setter. Pickup, carrier strategy, combat and scoring run normally.'
  if a.client:p.start_role('client');p.delay(2);p.pulse('F12',('client',));p.wait(lambda:p.performance('client').get('local_ready'),60)
  if a.encounter:
   actor=next(o for o in p.state()['objects'] if o.get('slot')==0);q=actor['position'];p.command('player_place 1 '+str(q[0]-8)+' '+str(q[1])+' '+str(q[2]));p.delay(2);result['encounter_fixture']='Bot 1 placed eight metres beside host via original native position setter; AI combat and damage run normally.'
  began=time.monotonic();until=began+a.seconds;next_skip=0;second_encounter=False
  while time.monotonic()<until:
   p.inspect()
   if any(x.poll() is not None for x in p.processes.values()):raise RuntimeError('Peer exited')
   if '[port] Stopped:' in p.log('host'):raise RuntimeError('Guest stopped')
   result['samples'].append(p.state());p.delay(1)
   if a.encounter and a.mode==2 and not second_encounter and time.monotonic()-began>25:
    q=next(o for o in p.state()['objects'] if o.get('slot')==0)['position'];p.command('player_place 2 '+str(q[0]-7)+' '+str(q[1])+' '+str(q[2]));second_encounter=True
   if time.monotonic()>next_skip:p.pulse('B',tuple(p.processes));next_skip=time.monotonic()+8
  if a.mode==1:
   links=[o for o in p.state()['objects'] if o.get('target') and not o.get('terminal')]
   if not links:raise RuntimeError('Coop level has no nonterminal exit fixture')
   target=links[0]['target'];old_epoch=p.state()['epoch'];p.command('link '+target)
   p.wait(lambda:p.state().get('epoch',0)>old_epoch and p.state().get('level')==target,90)
   if a.client:p.wait(lambda:p.performance('client').get('level')==target and p.performance('client').get('local_ready') and p.performance('client').get('ready_players')==8,90)
   p.delay(6);result['samples'].append(p.state());result['level_transition']={'target':target,'epoch':p.state()['epoch'],'method':'original level exit event shortcut'}
  p.pulse('SHOT',tuple(p.processes));p.pulse('F12',('host',));p.delay(.5);p.pulse('UISHOT',('host',));p.pulse('F12',('host',));p.close()
  poses={};weapons={};health={};owned={};flags=0;bombs=0
  for s in result['samples']:
   for o in s.get('objects',[]):
    if 'slot' not in o or o['slot']==0 or o['slot']>min(a.bots,7 if a.mode==1 else 3):continue
    slot=o['slot'];poses.setdefault(slot,[]).append(o['position']);weapons.setdefault(slot,set()).add(o['weapon']);health.setdefault(slot,set()).add(o['health']);owned.setdefault(slot,set()).add(tuple(o['weapon_owned_counts']));flags+=o['flag'];bombs+=o['bomb']
  result['movement']={slot:max(math.dist(ps[0],x) for x in ps) for slot,ps in poses.items()};result['weapons']={s:sorted(v) for s,v in weapons.items()};result['health_values']={s:sorted(v) for s,v in health.items()};result['inventory_variants']={s:len(v) for s,v in owned.items()};result.update(flag_samples=flags,bomb_samples=bombs)
  result['errors']=[line for role in p.processes for line in p.log(role).splitlines() if '[port] Stopped:' in line or '[campaign-field-difference]' in line]
  result['weapon_choices']=[line for line in p.log('host').splitlines() if '[coop-afk-weapon-choice]' in line];result['bot_respawns']=[line for line in p.log('host').splitlines() if '[coop-afk-native-respawn-observed]' in line];result['fire_decisions']=p.log('host').count('fire=255')
  result['passed']=bool(poses) and max(result['movement'].values())>2 and not result['errors'] and all(x.returncode==0 for x in p.processes.values())
 except Exception:result['error']=traceback.format_exc();print(result['error'],flush=True)
 finally:
  p.close();result.update(exits={r:x.returncode for r,x in p.processes.items()},visible=p.visible,focused=p.focused,card_unchanged=sha(a.card)==before);result['passed']&=not p.visible and not p.focused and result['card_unchanged'];(a.output/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k!='samples'}),flush=True)
 return 0 if result['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
