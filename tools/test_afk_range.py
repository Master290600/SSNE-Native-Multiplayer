"""Private native AFK long-range pickup, visibility, firing and damage probe.

Uses an original weapon pickup and enemy factory, never inventory ownership or
weapon damage writes. The old-range mode uses the same executable and actual
far fixture with a hidden30m detection limit. Requested distance alone is not
evidence: acceptance requires measured native positions or far firing records.
"""
from test_native_cave_gate import CavePeers
from test_coop_campaign import sha
from pathlib import Path
import argparse,json,math,re,shutil,time,traceback

def rows(text,marker):return [dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in text.splitlines() if marker in line]

def native_combat_observations(log,samples,weapon):
 # The confirmed-hit journal currently covers pistol/Uzi/shotgun/minigun only.
 # Keep unsupported sniper damage tied to its original source, subtraction and
 # same-entity health transition instead of inventing a confirmed-hit event.
 actor_addresses={str(o['address']) for state in samples for o in state.get('objects',[]) if o.get('slot')==0 and 'address' in o}
 health={}
 for state in samples:
  for enemy in state.get('objects',[]):
   if enemy.get('kind')==1 and 'health' in enemy:health.setdefault((str(enemy['address']),str(enemy['id'])),float(enemy['health']))
 current_fire={};motion={};cue={};source='';previous_fire=None;pending={};journal=[];original=[];ammo=[];queries={}
 for line in log.splitlines():
  entry=dict(re.findall(r'(\w+)=([^\s]+)',line))
  if '[coop-afk-motion]' in line and entry.get('slot')=='0':motion=entry
  elif '[coop-weapon-cue-host]' in line and entry.get('slot')=='0':cue=entry
  elif '[game-bot-fire-trace]' in line and entry.get('slot')=='0':
   current_fire=entry
   if entry.get('weapon')==str(weapon) and entry.get('chosen')==str(weapon):
    if previous_fire and previous_fire['row'].get('target_entity')==entry.get('target_entity') and previous_fire['row'].get('fire')=='1' and 60<=float(previous_fire['row']['distance'])<=100 and int(entry['ammo'])<int(previous_fire['row']['ammo']) and cue.get('weapon')==str(weapon) and int(cue.get('sequence','0'))>previous_fire['cue_sequence']:
     ammo.append({'before':int(previous_fire['row']['ammo']),'after':int(entry['ammo']),'fire_tick':int(previous_fire['row']['tick']),'distance_at_fire':float(previous_fire['row']['distance']),'native_cue_sequence':int(cue['sequence'])})
    previous_fire={'row':entry,'cue_sequence':int(cue.get('sequence','0'))}
  elif '[coop-area-common-entry]' in line:source=entry.get('source','')
  elif '[coop-native-subtract-entry]' in line:
   if entry.get('victim')==current_fire.get('target') and current_fire.get('weapon')==str(weapon) and current_fire.get('chosen')==str(weapon) and current_fire.get('fire')=='1' and source in actor_addresses and cue.get('slot')=='0' and cue.get('weapon')==str(weapon) and int(entry.get('caller','0'))==0x80093C2C and float(entry.get('amount','0'))>0:
    pending[entry['victim']]={'observation':'original_native_subtraction_and_health_transition','source_actor':int(source),'target':int(current_fire['target_entity']),'victim':int(entry['victim']),'weapon':weapon,'amount':float(entry['amount']),'distance_at_last_native_aim_sample':float(current_fire['distance']),'aim_tick':int(current_fire['tick']),'native_cue_sequence':int(cue['sequence'])}
  elif '[coop-host-health-observed]' in line:
   key=(entry['object'],entry['id']);after=float(entry['health']);before=health.get(key);native=pending.pop(entry['object'],None)
   if native and str(native['target'])==entry['id'] and before is not None and after<before:
    native.update(before=before,after=after,health_observed_frame=int(entry['frame']));original.append(native)
   health[key]=after
  elif '[coop-confirmed-hit-host]' in line:
   if entry.get('slot')=='0' and entry.get('weapon')==str(weapon) and entry.get('target')==current_fire.get('target_entity') and current_fire.get('fire')=='1' and current_fire.get('chosen')==str(weapon) and float(entry.get('amount','0'))>0:
    record=dict(entry);record.update(distance_at_last_native_aim_sample=float(current_fire['distance']),aim_tick=int(current_fire['tick']))
    if motion:record['source_position_at_aim']=[float(motion[k]) for k in ('x','y','z')]
    journal.append(record)
  elif '[coop-bullet-query]' in line and entry.get('weapon')==str(weapon):queries[entry.get('shot')]=entry
 far_journal=[]
 for record in journal:
  query=queries.get(record.get('shot'));position=record.get('source_position_at_aim')
  if query and position:
   distance=math.hypot(float(query['x'])-position[0],float(query['z'])-position[2]);record['native_query_hit_horizontal_distance']=distance;record['range_evidence']='matching native bullet intersection and confirmed-hit shot identity'
  else:
   # This fixture uses a8m/s rocketeer and125ms decisions. A3m margin
   # covers motion between decisions and its front collision surface.
   distance=record['distance_at_last_native_aim_sample']-3.;record['conservative_hit_distance_lower_bound']=distance;record['range_evidence']='same-target native aim sample minus3m motion/collision margin'
  if 60<=distance<=100:far_journal.append(record)
 return {'journal_supported':weapon in (1,2,3,4),'far_journal_hits':far_journal,'native_journal_hits':journal,'original_native_damage':original,'far_fire_ammo_consumption':ammo}

def evaluate(log,client_log,samples,weapon,old_range,expect_occluded=False):
 afk_events=[r for r in rows(log,'[coop-afk-state]') if r.get('slot')=='0' and r.get('afk')=='1']
 afk_start_tick=min((int(r['tick']) for r in afk_events),default=0)
 trace_log=log;repeat_frame=0
 for index,line in enumerate(log.splitlines()):
  entries=rows(line,'[coop-test-enemy-wave]')
  if entries and entries[0].get('repeat')=='1' and entries[0].get('source_slot')=='1':
   repeat_frame=int(entries[0]['frame']);trace_log='\n'.join(log.splitlines()[index:])
 measurement_tick=max(afk_start_tick,repeat_frame)
 sights=[r for r in rows(trace_log,'[coop-afk-sight]') if r.get('slot')=='0']
 fires=[r for r in rows(trace_log,'[game-bot-fire-trace]') if r.get('slot')=='0']
 all_far_fires=[r for r in fires if r.get('fire')=='1' and 60<=float(r['distance'])<=100]
 far_fires=[r for r in all_far_fires if r.get('chosen')==str(weapon) and r.get('friend_blocks')=='0' and int(r.get('ammo','0'))>0]
 far_sights=[r for r in sights if 60<=float(r['distance'])<=100]
 clear=[r for r in far_sights if r.get('visible')=='1' and r.get('query_valid')=='1' and r.get('obstructed')=='0']
 blocked=[r for r in far_sights if r.get('visible')=='0' and r.get('query_valid')=='1' and r.get('obstructed')=='1']
 blocked_targets={r['target'] for r in blocked};distances=[];owned=False;blocked_entities=set()
 for state in samples:
  actor=next((o for o in state.get('objects',[]) if o.get('slot')==0),None)
  if not actor:continue
  inventory=actor.get('weapon_owned_counts',[]);owned |= len(inventory)>weapon and inventory[weapon]>0
  for enemy in state.get('objects',[]):
   if str(enemy.get('address')) in blocked_targets:blocked_entities.add(str(enemy['id']))
   if enemy.get('kind')==1 and enemy.get('definition')==12 and enemy.get('health',0)>0 and afk_start_tick and state.get('tick',0)>=measurement_tick:distances.append(math.dist(actor['position'],enemy['position']))
 combat=native_combat_observations(trace_log,samples,weapon)
 blocked_hits=[r for r in rows(trace_log,'[coop-confirmed-hit-host]') if r.get('slot')=='0' and r.get('target') in blocked_entities and float(r.get('amount','0'))>0]
 blocked_hits.extend(r for r in combat['original_native_damage'] if str(r['target']) in blocked_entities)
 measured=max(distances,default=0);pickup='[coop-test-pickup-player] type='+str(weapon+35)+' slot=0' in log
 choices=[r for r in rows(log,'[coop-afk-weapon-choice]') if r.get('slot')=='0' and r.get('weapon')==str(weapon) and r.get('owned')=='1' and int(r.get('primary_ammo','0'))>0]
 errors=[line for text in (log,client_log) for line in text.splitlines() if '[port] Stopped:' in line or '[campaign-field-difference]' in line]
 afk=bool(afk_events);far_position_samples=[distance for distance in distances if 60<=distance<=100];actual_far=bool(far_position_samples) or bool(far_fires)
 passed=pickup and (owned or bool(choices)) and afk and actual_far and not errors
 if expect_occluded:passed &= bool(blocked) and bool(blocked_entities) and not clear and not all_far_fires and not blocked_hits
 elif old_range:passed &= not all_far_fires and not clear
 elif combat['journal_supported']:passed &= bool(clear) and bool(far_fires) and bool(combat['far_journal_hits'])
 else:passed &= bool(clear) and bool(far_fires) and bool(combat['far_fire_ammo_consumption']) and bool(combat['original_native_damage'])
 damage_scope='This weapon supports the native confirmed-hit journal; the positive case requires an observed far-range hit, while blocked and old-range cases require no far firing.' if combat['journal_supported'] else 'Sniper8 has no confirmed-hit journal hook. Far visibility, native firing and ammunition consumption are required separately. Original native subtraction/health transitions report their observed aim distance; a later near kill is not called a far confirmed hit.'
 return {'passed':bool(passed),'old_range':old_range,'expect_occluded':expect_occluded,'far_blocked_native_sight_samples':blocked,'blocked_target_entity_ids':sorted(blocked_entities),'blocked_target_native_damage':blocked_hits,'all_far_native_firing_samples':all_far_fires,'afk_host_observed':afk,'afk_start_tick':afk_start_tick,'measurement_repeat_frame':repeat_frame,'original_pickup_observed':pickup,'native_owned_weapon_observed':owned or bool(choices),'maximum_observed_native_enemy_distance':measured,'native_position_samples_in_60_to_100m':len(far_position_samples),'actual_far_fixture_observed':actual_far,'far_clear_native_sight_samples':clear,'far_native_firing_samples':far_fires,'matching_native_damage':combat['far_journal_hits'] if combat['journal_supported'] else combat['original_native_damage'],'native_damage_observation_scope':damage_scope,'confirmed_hit_journal_supported':combat['journal_supported'],'far_confirmed_hit_proven':bool(combat['far_journal_hits']),'far_fire_ammo_consumption':combat['far_fire_ammo_consumption'],'original_native_subtraction_observations':combat['original_native_damage'],'weapon_choices':choices,'visibility_samples':far_sights,'errors':errors}

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 modes=ap.add_mutually_exclusive_group();modes.add_argument('--old-range',action='store_true');modes.add_argument('--expect-occluded',action='store_true');ap.add_argument('--pickup',type=int,choices=(39,43),default=43);ap.add_argument('--distance',type=float,default=80);ap.add_argument('--afk-delay-ms',type=int,default=20000);ap.add_argument('--repeat-frame',type=int,default=2000);ap.add_argument('--capture-geometry',action='store_true');ap.add_argument('--seconds',type=float,default=12);ap.add_argument('--port',type=int,default=27884);ap.add_argument('--fps',type=int,default=60)
 a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 if not 60<=a.distance<=100:raise ValueError('Long-range fixture must request60..100m')
 a.output.parent.mkdir(parents=True,exist_ok=True);a.bots=0;a.mode=1;a.capacity=8;a.open_menu=False;a.mod_flags=1
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_COOP_TEST_AFK':'1','SSNE_COOP_TEST_AFK_MASK':'1','SSNE_COOP_TEST_AFK_DELAY_MS':str(a.afk_delay_ms),'SSNE_COOP_TEST_AFK_RETURN_MS':'120000','SSNE_COOP_TEST_PICKUP':str(a.pickup),'SSNE_COOP_TEST_PICKUP_AT_PLAYERS':'1','SSNE_COOP_TEST_PICKUP_MASK':'1','SSNE_COOP_TEST_ENEMY_TYPE':'12','SSNE_COOP_TEST_ENEMY_COUNT':'1','SSNE_COOP_TEST_ENEMY_DISTANCE':str(a.distance),'SSNE_COOP_TEST_ENEMY_SLOT':'1','SSNE_COOP_TEST_ENEMY_AGAIN_FRAME':str(a.repeat_frame),'SSNE_COOP_TEST_BOT_FIRE_TRACE':'1'}
 if a.old_range:a.test_env['SSNE_TEST_AFK_RANGE_LIMIT']='30'
 a.client_test_env={'SSNE_TEST_DEVICE_INPUT':'1'}
 p=CavePeers(a,'Rlevel1_3',a.output);before=sha(a.card);r={'passed':False,'exe_sha256':sha(a.exe),'distance_requested':a.distance,'pickup':a.pickup,'weapon':a.pickup-35,'enemy_source_slot':1,'enemy_repeat_frame':a.repeat_frame,'afk_delay_ms':a.afk_delay_ms,'old_range':a.old_range,'expect_occluded':a.expect_occluded,'scope':'Native AFK-host long-range target acquisition, native geometry visibility, original weapon pickup, ordinary native firing and weapon-specific native damage observation in a private Rlevel1_3 fixture. God mode protects the stationary actors; no direct inventory, ammo or damage writes. The blocked mode requires a real obstructed native query and no far trigger or damage to those blocked entities; CPU tests additionally cover short weapon restrictions. This is not a full campaign or arbitrary map visibility claim.'};samples=[]
 try:
  p.start();p.initial_ready(('host',))
  # Move the private fixture through the original native position setter before
  # the normal host-side factory requests start. The measured second wave is requested after both players have been anchored. These anchors
  # have real native floor+ray CPU evidence; no visibility state is overridden.
  start_position=[0,2.27,1550] if a.expect_occluded else [-120,2.27,1450]
  def place_actor(slot,position):
   p.command('player_place '+str(slot)+' '+' '.join(str(v) for v in position));sequence=p.command_sequence
   p.wait(lambda:'sequence='+str(sequence)+' action=player_place slot='+str(slot) in p.log('host'),15)
   def placed():
    actor=next((o for o in p.state().get('objects',[]) if o.get('slot')==slot),None)
    return actor and math.dist(actor['position'],position)<1.5
   try:p.wait(placed,15)
   except TimeoutError:
    state=p.state();actor=next((o for o in state.get('objects',[]) if o.get('slot')==slot),{})
    raise TimeoutError('Native position fixture slot='+str(slot)+' wanted='+str(position)+' last_authoritative_position='+str(actor.get('position'))+' tick='+str(state.get('tick'))+' command_sequence='+str(sequence))
   return next(o['position'] for o in p.state()['objects'] if o.get('slot')==slot)
  r['start_position_requested']=start_position;r['host_position_before_client_join']=place_actor(0,start_position)
  samples.append(p.state());p.start_role('client');p.initial_ready(('host','client'))
  p.wait(lambda:all(p.performance(role).get('local_ready') and p.performance(role).get('ready_players')==2 for role in ('host','client')),90)
  # Native join uses the original checkpoint, not the host's current position.
  # Keep the human three metres behind the host: inside the follow stop
  # radius and behind the firing segment. No bot drift/jump is needed.
  client_position=list(start_position);client_position[2]-=3
  r['positions_before_measurement']={'client':place_actor(1,client_position),'host':place_actor(0,start_position)}
  samples.append(p.state())
  if a.capture_geometry:
   p.delay(.6);p.command('dump');sequence=p.command_sequence
   p.wait(lambda:'sequence='+str(sequence)+' action=dump' in p.log('host'),15);p.delay(.4)
   folder=a.output/'geometry-capture';folder.mkdir()
   ram=a.output/'host/campaign-capture.ram';p.wait(lambda:ram.exists() and ram.stat().st_size==64*1024*1024,10)
   captured={}
   for name in ('campaign-capture.ram','campaign-context.bin','native-capture-locked-cache.bin'):
    source=a.output/'host'/name
    if source.exists():shutil.copy2(source,folder/name);captured[name]={'sha256':sha(folder/name),'size':(folder/name).stat().st_size}
   r['geometry_capture']={'folder':str(folder),'authoritative_tick':p.state().get('tick'),'files':captured,'scope':'RAM and PPC context from the same original campaign dump after both placements and streaming frames. Optional locked cache is copied only if already available; its capture time is separate.'}
   (folder/'capture-info.json').write_text(json.dumps(r['geometry_capture'],indent=2),encoding='utf8')
  if any(r.get('slot')=='0' and r.get('afk')=='1' for r in rows(p.log('host'),'[coop-afk-state]')):
   raise RuntimeError('Fixture setup exceeded the human-only positioning interval; increase --afk-delay-ms and --repeat-frame rather than moving an already-following AFK bot')
  p.wait(lambda:any(r.get('slot')=='0' and r.get('afk')=='1' for r in rows(p.log('host'),'[coop-afk-state]')),max(30,a.afk_delay_ms/1000+5))
  # The initial checkpoint-source wave can be below terrain and is excluded
  # from measurement. The unchanged client yaw supplies a fresh80m second
  # wave after both players are placed and the host is already automated.
  p.wait(lambda:any(r.get('source_slot')=='1' and r.get('repeat')=='1' for r in rows(p.log('host'),'[coop-test-enemy-wave]')),max(35,a.repeat_frame/a.fps+5))
  r['measurement_started_after_repeat_wave']=True
  r['positions_at_repeat_wave']=[{'slot':o['slot'],'position':o['position']} for o in p.state().get('objects',[]) if o.get('slot') in (0,1)]
  samples.append(p.state());start=time.monotonic()
  while time.monotonic()-start<a.seconds:
   samples.append(p.state());p.wait(lambda:all(process.poll() is None for process in p.processes.values()),1);p.delay(.15)
  p.pulse('UISHOT');r.update(evaluate(p.log('host'),p.log('client'),samples,a.pickup-35,a.old_range,a.expect_occluded));r['samples']=samples
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  p.close();r.update(visible=p.visible,focused=p.focused,exits={role:x.returncode for role,x in p.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not p.visible and not p.focused and r['original_card_unchanged'] and all(code==0 for code in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps({k:v for k,v in r.items() if k!='samples'},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
