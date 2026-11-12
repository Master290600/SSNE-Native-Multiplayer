"""Compare native third-person animation clocks against wall time in paired peers."""
from test_native_frontend import Menus,sha
from pathlib import Path
import argparse,json,time,traceback,re,math,statistics

def diagnostic_rows(text,marker):
 return [dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in text.splitlines() if marker in line]

def clock_rates(text):
 groups={}
 for row in diagnostic_rows(text,'[animation-clock]'):
  key=(int(row['slot']),int(row['part']),int(row['model']))
  groups.setdefault(key,[]).append(row)
 result=[]
 for key,rows in groups.items():
  if len(rows)<4:continue
  span=float(rows[-1]['wall'])-float(rows[0]['wall'])
  if span<.5:continue
  result.append({'slot':key[0],'part':key[1],'model':key[2],'samples':len(rows),'wall_seconds':span,'model_seconds_per_wall_second':(float(rows[-1]['scaled_dt_total'])-float(rows[0]['scaled_dt_total']))/span,'input_seconds_per_wall_second':(float(rows[-1]['input_dt_total'])-float(rows[0]['input_dt_total']))/span,'speed_median':statistics.median(float(x['speed']) for x in rows),'dt_median':statistics.median(float(x['dt']) for x in rows),'physics_dt_median':statistics.median(float(x['physics_dt']) for x in rows),'render_dt_median':statistics.median(float(x['render_dt']) for x in rows),'skipped':sorted(set(x['skipped'] for x in rows))})
 return result

def active_run_clip(text,slot):
 # Discover the actual running master from its live clip, rather than assuming
 # one owner alias (part2 may be an idle held-weapon leaf).
 groups={}
 for row in diagnostic_rows(text,'[animation-clock]'):
  if int(row['slot'])==slot:groups.setdefault((int(row['part']),int(row['model'])),[]).append(row)
 candidates=[]
 for (part,model),all_rows in groups.items():
  rows=[r for r in all_rows if r['skipped']=='0' and r['command']=='0' and r['clip']=='2' and r['selected']=='2' and 0<float(r['speed'])<10 and float(r['duration'])>0]
  if len(rows)<4 or len(rows)<.8*len(all_rows):continue
  span=float(rows[-1]['wall'])-float(rows[0]['wall'])
  if span<.5:continue
  frames={int(r['selected_frame']) for r in rows};times={float(r['time']) for r in rows}
  if len(frames)<4 or len(times)<4:continue
  phase_advance=0
  for before,after in zip(rows,rows[1:]):
   delta=float(after['time'])-float(before['time'])
   if delta<0:delta+=float(before['duration'])
   phase_advance+=delta
  candidates.append({'verified':True,'slot':slot,'part':part,'model':model,'clip':2,'samples':len(rows),'coverage':len(rows)/len(all_rows),'wall_seconds':span,'distinct_key_frames':len(frames),'native_phase_seconds_per_wall_second':phase_advance/span,'model_seconds_per_wall_second':(float(rows[-1]['scaled_dt_total'])-float(rows[0]['scaled_dt_total']))/span,'speed_median':statistics.median(float(r['speed']) for r in rows),'dt_median':statistics.median(float(r['dt']) for r in rows)})
 return max(candidates,key=lambda x:x['samples']) if candidates else None

def received_run_pose(text,slot,host_clip,viewer_slot=1):
 # Replicas deliberately retain an idle LOCAL clip state: their displayed
 # matrices come from the host. Verify that path instead of requiring a second
 # locally advancing run clip, which would contradict host authority.
 result={'verified':False,'slot':slot,'host_active_run_clip':bool(host_clip),'body_mesh_expected':slot!=viewer_slot}
 if not host_clip:return result
 rows=[r for r in diagnostic_rows(text,'[animation-clock]') if int(r['slot'])==slot and int(r['part'])==host_clip['part']]
 if len(rows)<4:return result
 span=float(rows[-1]['wall'])-float(rows[0]['wall'])
 local_advance=float(rows[-1]['advance_dt_total'])-float(rows[0]['advance_dt_total'])
 probes=[r for r in diagnostic_rows(text,'[coop-mesh-probe]') if int(r['slot'])==slot and int(r['weapon'])==-1]
 mismatched=sum(int(r['changed_bones'])!=0 for r in probes)
 current=[r for r in probes if int(r['frame'])==int(r['skinned'])]
 result.update(part=host_clip['part'],clock_samples=len(rows),wall_seconds=span,local_advance_seconds=local_advance,local_tick_suppressed=all(r['skipped']=='1' for r in rows),body_mesh_samples=len(probes),mismatched_bone_samples=mismatched,current_skinned_samples=len(current),distinct_current_skin_frames=len({int(r['frame']) for r in current}))
 # The client's own body is absent in first-person rendering. Its native run
 # master is verified on the host; here verify the replica cannot advance it
 # a second time. A remote body must additionally reach the current skin pass.
 result['verification_scope']='received_remote_body_draw' if result['body_mesh_expected'] else 'first_person_body_local_tick_suppression'
 body_verified=(len(current)>=4 and result['distinct_current_skin_frames']>=3) if result['body_mesh_expected'] else True
 result['verified']=span>=.5 and result['local_tick_suppressed'] and abs(local_advance)<1e-4 and mismatched==0 and body_verified
 return result

def phase_checks(tails,moving):
 slot=0 if moving=='host' else 1
 finishes={role:sum(int(r['slot'])==slot and math.isclose(float(r['speed']),999.,rel_tol=0,abs_tol=1e-4) for r in diagnostic_rows(text,'[animation-speed]')) for role,text in tails.items()}
 finish=sum(finishes.values())
 host_clip=active_run_clip(tails['host'],slot);client_pose=received_run_pose(tails['client'],slot,host_clip)
 return {'steady_finish_spikes':finish,'steady_finish_spikes_by_peer':finishes,'active_run_clip':host_clip,'received_run_pose':client_pose,'active_gait_verified':bool(host_clip) and client_pose['verified']}

def report_passed(report):
 phases=report['phases']
 return not report.get('error') and len(phases)==2 and all(p['movement']>.5 and p['active_gait_verified'] and all(v['rates'] for v in p['peers'].values()) and (not report.get('steady_required') or p['steady_finish_spikes']<=1) for p in phases) and not report.get('visible') and not report.get('focused') and report.get('original_card_unchanged') is True and bool(report.get('exits')) and all(code==0 for code in report['exits'].values())

def recompute_report(folder):
 folder=Path(folder);path=folder/'result.json';report=json.loads(path.read_text(encoding='utf8'))
 for phase in report['phases']:
  tails={role:(folder/(phase['moving']+'-moving-'+role+'-view.log')).read_text(encoding='utf8') for role in ('host','client')}
  for role in tails:phase['peers'][role]['rates']=clock_rates(tails[role])
  phase.update(phase_checks(tails,phase['moving']))
 report['phase_checks_version']=2;report['passed']=report_passed(report)
 return report

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--require-steady',action='store_true');ap.add_argument('--fps',type=int,default=60);ap.add_argument('--seconds',type=float,default=5);ap.add_argument('--port',type=int,default=27870)
 a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.test_env={'SSNE_TEST_ANIMATION_TRACE':'1'};a.output.mkdir(parents=True,exist_ok=False);m=Menus(a,'',a.output);before=sha(a.card)
 r={'passed':False,'exe_sha256':sha(a.exe),'fps_requested':a.fps,'phases':[]}
 try:
  m.start_role('host',1);m.network('host');m.choose('host',0);m.page('host',2);pin=m.state()['pin'];m.choose('host',0)
  m.wait(lambda:m.state().get('active') and m.state().get('native_root')==0xa7);m.press('host','A');m.delay(1);m.press('host','START');m.wait(lambda:m.state().get('lobby'))
  m.start_role('client',1);m.network('client');m.choose('client',1);m.page('client',3);m.field('client',1,2,pin);m.choose('client',2);m.wait(lambda:m.state('client').get('lobby'))
  m.press('host','START');m.delay(1);m.press('host','A');m.wait(lambda:'original difficulty selection opened' in m.log('host'));m.press('host','A')
  m.wait(lambda:all(m.state(role).get('scene')==4 and m.performance(role).get('local_ready') and m.performance(role).get('ready_players')==2 for role in ('host','client')),90);m.delay(4)
  m.press('host','GODON')
  for moving in ('host','client'):
   m.sequence+=1;input_file=a.output/moving/'input.txt';input_file.write_text(','*m.sequence+'SUP@0:100000');m.delay(.4)
   cuts={role:len(m.log(role)) for role in ('host','client')};p=m.performance(moving)['position'];start=time.monotonic();m.delay(a.seconds);duration=time.monotonic()-start;q=m.performance(moving)['position']
   tails={role:m.log(role)[cuts[role]:] for role in ('host','client')};input_file.write_text('');m.delay(.5)
   phase={'moving':moving,'seconds':duration,'movement':math.dist(p,q),'peers':{}}
   for role in ('host','client'):
    tail=tails[role];(a.output/(moving+'-moving-'+role+'-view.log')).write_text(tail,encoding='utf8');phase['peers'][role]={'rates':clock_rates(tail),'performance':m.performance(role)}
   phase.update(phase_checks(tails,moving))
   r['phases'].append(phase);print('Measured '+moving+' movement at'+str(a.fps)+' FPS',flush=True)
  m.pulse('UISHOT');r['phase_checks_version']=2;r['passed']=all(p['movement']>.5 and p['active_gait_verified'] and all(v['rates'] for v in p['peers'].values()) for p in r['phases'])
  r['steady_required']=a.require_steady
  if a.require_steady:r['passed'] &= all(p['steady_finish_spikes']<=1 for p in r['phases'])
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  m.close();r.update(visible=m.visible,focused=m.focused,exits={role:p.returncode for role,p in m.processes.items()},original_card_unchanged=sha(a.card)==before)
  r['passed'] &= not m.visible and not m.focused and r['original_card_unchanged'] and all(code==0 for code in r['exits'].values())
  (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps({'passed':r['passed'],'fps':a.fps,'error':r.get('error')},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
