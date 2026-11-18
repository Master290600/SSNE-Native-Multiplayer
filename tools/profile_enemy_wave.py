"""Private two-process native enemy-wave renderer benchmark.

No desktop input or production setting/card writes. Waves use the existing
hidden adapter and original native enemy factory. This is an explicit combat
fixture, not campaign/menu acceptance. Both processes share one PC. Stage spans
overlap; duration quantiles are exact only when raw diagnostic arrays exist.
"""
from pathlib import Path
import argparse, hashlib, json, math, os, shutil, statistics, subprocess, sys, time, traceback
S=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(S/'tools'))
from test_native_cave_gate import CavePeers
from test_coop_campaign import sha
from profile_renderer import summarize as summarize_detail

ROLES=('host','client')
def snapshot_sources(a):
 out={'exe':sha(a.exe),'card':sha(a.card),'dol':sha(a.data/'sys/main.dol')}
 out['original_settings']={str(p):sha(p) if p.exists() else None for root in ([a.exe.parent]+([a.protected_root] if a.protected_root else [])) for name in ('settings.ini','settings-netplay.ini') for p in (root/name,)}
 return out

def pct(values,p):
 values=sorted(values)
 if not values:return None
 at=(len(values)-1)*p;low=int(at);fraction=at-low
 return values[low]*(1-fraction)+values[min(low+1,len(values)-1)]*fraction

def summarize(folder,first,last,level,scale,fps):
 rows=[]
 for line in (folder/'performance.jsonl').read_text(encoding='utf8').splitlines():
  try:r=json.loads(line)
  except ValueError:continue
  if r.get('type')=='sample' and first<=r['elapsed_s']<=last and r.get('level')==level and r.get('scene')==4 and r.get('local_ready') and r.get('ready_players')==2 and not r.get('paused') and r.get('presentation_busy') == 0 and r.get('scale')==scale and r.get('fps_limit')==fps and not r.get('vsync'):rows.append(r)
 if len(rows)<8:raise RuntimeError('Too few valid gameplay windows: '+str(len(rows)))
 result={'windows':len(rows),'first_elapsed_s':rows[0]['elapsed_s'],'last_elapsed_s':rows[-1]['elapsed_s'],'window_seconds':sum(r['window_s'] for r in rows),'stage_spans_overlap':True,'gpu_time_scope':'Asynchronous mean per timed GPU execute; not necessarily total per presented image.'}
 for key in ('render_fps','native_fps','updates_per_s','entities','gpu_submit_cpu_last_ms','gpu_ms_async','ping_ms','last_applied_motion_age_ms','gpu_pending'):
  values=[r[key] for r in rows if key in r and r[key]>=0]
  result[key]={'median':statistics.median(values),'min':min(values),'max':max(values)} if values else None
 result['dropped_frames']=rows[-1].get('dropped_total',0)-rows[0].get('dropped_total',0)
 result['new_shader_programs']=rows[-1]['shader_programs_total']-rows[0]['shader_programs_total']
 result['detail']=summarize_detail(rows,first,last,level,scale,fps,2).get('detail',{})
 result['stages']={}
 for name in sorted({k for r in rows for k in r.get('stages',{})}):
  spans=[r['stages'][name] for r in rows if name in r.get('stages',{})];n=sum(s['calls'] for s in spans);raw=[v for s in spans for v in s.get('durations_ms',[])]
  stage={'calls':n,'avg_ms':sum(s['calls']*s['avg_ms'] for s in spans)/n,'max_ms':max(s['max_ms'] for s in spans),'over33ms':sum(s['over33ms'] for s in spans),'p95_window_upper_max_ms':max(s['p95_upper_ms'] for s in spans),'p99_window_upper_max_ms':max(s['p99_upper_ms'] for s in spans),'quantile_scope':'Raw per-operation samples' if raw else 'Only per-window histogram upper bounds; no exact p50/p95/p99 available'}
  if raw:
   dropped=sum(v.get('raw_dropped',0) for v in spans)
   if dropped:raise RuntimeError('Dropped raw timing samples for '+name)
   worst=sorted(raw,reverse=True)[:max(1,math.ceil(len(raw)*.01))]
   stage.update(raw_samples=len(raw),raw_dropped=dropped,p50_ms=pct(raw,.50),p95_ms=pct(raw,.95),p99_ms=pct(raw,.99))
   if name=='present_interval':stage['one_percent_low_fps']=1000/statistics.mean(worst)
  result['stages'][name]=stage
 return result

class WavePeers(CavePeers):
 def start_role(self,role):
  f=self.folder/role;f.mkdir();(f/'steps').mkdir();shutil.copy2(self.a.exe,f/'game.exe');shutil.copy2(self.a.card,f/'card.raw')
  (f/'input.txt').write_text('');(f/'command.txt').write_text('')
  settings=f'[graphics]\nrenderer=gpu\nscale={self.a.scale}\nantialiasing=off\nanisotropy=1\nfps_limit={self.a.fps}\nvsync=0\nsync_to_display=0\nwindow_w=1100\nwindow_h=850\nwindow_maximized=0\nfullscreen=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'
  (f/'settings-netplay.ini').write_text(settings)
  if self.a.shader_cache:shutil.copytree(self.a.shader_cache,f/'shadercache')
  env={k:v for k,v in os.environ.items() if not k.startswith('SSNE_')}
  env.update(SSNE_TEST_HIDDEN='1',SSNE_PAD_SCRIPT_FILE=str(f/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(f/'steps'),SSNE_PERF_LOG=str(f/'performance.jsonl'),SSNE_PERF_RAW_FRAMES='1',SSNE_PERF_DETAIL_EVERY='16',SSNE_COOP_TEST_SKIP_DELAY_MS='1500',SSNE_TEST_SKIP_FIRST_VOTE='1')
  if role=='host':env.update(SSNE_COOP_CAMPAIGN_COMMAND=str(f/'command.txt'),SSNE_NETPLAY_MOD_FLAGS='1',SSNE_COOP_TEST_ENEMY_TYPE=str(self.a.enemy_type),SSNE_COOP_TEST_ENEMY_COUNT='10',SSNE_COOP_TEST_ENEMY_SLOT='1',SSNE_COOP_TEST_ENEMY_DISTANCE=str(self.a.distance),SSNE_COOP_TEST_ENEMY_REPEAT_MS=str(self.a.repeat_ms))
  env.update(self.a.env);env.update(self.a.host_env if role=='host' else self.a.client_env)
  cmd=[str(f/'game.exe'),'--data',str(self.a.data),'--memcard',str(f/'card.raw'),'--lan-port',str(self.a.port),'--lan-pin','123456','--player-name','Sam' if role=='host' else 'Alex','--mute','--fps',str(self.a.fps),'--scale',str(self.a.scale)]
  cmd+=(['--coop-host','--coop-capacity','8','--coop-level',self.level] if role=='host' else ['--coop-join','127.0.0.1'])
  startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
  stream=(f/'console.log').open('wb');self.streams.append(stream)
  self.processes[role]=subprocess.Popen(cmd,cwd=f,env=env,stdout=stream,stderr=stream,startupinfo=startup,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
  self.a.report['launches'][role]={'argv':cmd,'test_env':{k:v for k,v in env.items() if k.startswith('SSNE_')},'pid':self.processes[role].pid}
 def target_enemies(self):
  return [o for o in self.state().get('objects',[]) if o.get('kind')==1 and o.get('definition')==self.a.enemy_type and o.get('health',0)>0 and o.get('id') not in self.a.preexisting]

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 for name in ('exe','data','card','output'):ap.add_argument('--'+name,type=Path,required=True)
 ap.add_argument('--shader-cache',type=Path,help='Seed identical private warm caches from this read-only directory')
 ap.add_argument('--protected-root',type=Path,help='Optional installed directory whose settings bytes must remain unchanged')
 ap.add_argument('--max-waves',type=int,default=8,help='Bound the hidden original-factory fixture before timing')
 ap.add_argument('--variant',choices=('baseline','legacy','optimized'),default='baseline')
 ap.add_argument('--level',default='Rlevel1_1');ap.add_argument('--fps',type=int,default=240);ap.add_argument('--scale',type=int,default=1);ap.add_argument('--port',type=int,default=28940)
 ap.add_argument('--enemy-type',type=int,default=12);ap.add_argument('--minimum-enemies',type=int,default=60);ap.add_argument('--distance',type=float,default=7,help='First native wave row distance in metres');ap.add_argument('--wave-spacing',type=float,default=1.5,help='Additional distance per wave row (0..5 metres);0 reproduces dense overlapping rows');ap.add_argument('--repeat-ms',type=int,default=1000);ap.add_argument('--seconds',type=float,default=18);ap.add_argument('--setup-timeout',type=float,default=145)
 ap.add_argument('--env',action='append',default=[],help='Private per-run SSNE_KEY=value; no other inherited SSNE variables')
 ap.add_argument('--client-env',action='append',default=[]);ap.add_argument('--host-env',action='append',default=[])
 a=ap.parse_args()
 for name in ('exe','data','card'):setattr(a,name,getattr(a,name).resolve(strict=True))
 if a.shader_cache:a.shader_cache=a.shader_cache.resolve(strict=True)
 if a.protected_root:a.protected_root=a.protected_root.resolve(strict=True)
 if not 1<=a.max_waves<=100:raise ValueError('Use 1..100 waves')
 if not math.isfinite(a.wave_spacing) or not 0<=a.wave_spacing<=5:raise ValueError('Wave spacing must be finite and within 0..5 metres')
 if a.fps!=0 and not 30<=a.fps<=240:raise ValueError('Use unlimited (0) or a frame cap of 30..240 FPS')
 if a.output.exists():raise FileExistsError('Use a fresh output directory; existing evidence is preserved')
 a.output=a.output.resolve();a.env=dict(x.split('=',1) for x in a.env);a.client_env=dict(x.split('=',1) for x in a.client_env);a.host_env=dict(x.split('=',1) for x in a.host_env)
 a.env.setdefault('SSNE_COOP_TEST_ENEMY_MAX_WAVES',str(a.max_waves))
 a.host_env.setdefault('SSNE_COOP_TEST_ENEMY_WAVE_SPACING',str(a.wave_spacing))
 if a.variant=='legacy':
  a.env.setdefault('SSNE_GPU_TEXTURE_BINDINGS','legacy');a.env.setdefault('SSNE_PRESENTATION_VECTOR_REUSE','0')
 if any(not k.startswith('SSNE_') for k in list(a.env)+list(a.client_env)+list(a.host_env)):raise ValueError('Only SSNE test settings allowed')
 a.bots=0;a.mode=1;a.capacity=8;a.open_menu=False;a.mod_flags=1;a.preexisting=set()
 r=a.report={'passed':False,'driver_sha256':sha(Path(__file__)),'capture_io_enabled':any('SSNE_GPU_CAPTURE' in env for env in (a.env,a.client_env,a.host_env)),'performance_comparison_eligible':not any('SSNE_GPU_CAPTURE' in env for env in (a.env,a.client_env,a.host_env)),'variant':a.variant,'executable_sha256':sha(a.exe),'target_fps':a.fps,'scale':a.scale,'minimum_enemy_target':a.minimum_enemies,'enemy_type':a.enemy_type,'first_wave_distance':a.distance,'wave_spacing':a.wave_spacing,'max_waves':a.max_waves,'launches':{},'scope':__doc__,'limits':['Two processes share this PC, hidden/offscreen presentation; no display/vsync pacing claim','Synthetic native enemy-factory stress fixture; not an authored campaign wave or campaign completion','God mode keeps both stationary test players alive; all enemy AI/animation/projectiles run normally','Hidden adapter requests ten enemies per wave at the declared interval; actual living population is counted','Population counts native same-type living enemies absent from the initial snapshot, including any authored encounter arrivals; debug wave requests are proved separately','Diagnostic state/trace overhead is equal between variants; stage spans overlap'],'population_samples':[]}
 before=snapshot_sources(a);p=WavePeers(a,a.level,a.output)
 def population(label):
  state=p.state();enemies=p.target_enemies();row={'at_monotonic_s':time.monotonic(),'label':label,'tick':state.get('tick'),'living_fixture_enemies':len(enemies),'ids':[o['id'] for o in enemies],'positions':[o['position'] for o in enemies],'players':[{'slot':o.get('slot'),'position':o['position']} for o in state.get('objects',[]) if 'slot' in o],'presentation_busy':{role:p.performance(role).get('presentation_busy') for role in p.processes}};r['population_samples'].append(row);return row
 try:
  p.start();p.initial_ready(('host',));a.preexisting={o['id'] for o in p.state().get('objects',[]) if o.get('kind')==1};r['preexisting_enemy_ids']=sorted(a.preexisting)
  p.start_role('client');p.initial_ready(ROLES)
  p.wait(lambda:all(p.performance(role).get('local_ready') and p.performance(role).get('ready_players')==2 for role in ROLES),90)
  # The shared world keeps running under Netricsa; scene4/local_ready alone do not prove visible gameplay.
  p.wait(lambda:all('presentation_busy' in p.performance(role) for role in ROLES),8)
  r['presentation_setup_actions']=[];clear_since=None;deadline=time.monotonic()+70
  while True:
   busy=tuple(role for role in ROLES if p.performance(role).get('presentation_busy'))
   if busy:
    clear_since=None;r['presentation_setup_actions'].append({'roles':busy,'input':'ordinary native B/Back','at_s':time.monotonic()});p.pulse('B',busy);p.delay(1.1)
   else:
    if clear_since is None:clear_since=time.monotonic()
    if time.monotonic()-clear_since>=2:break
    p.delay(.2)
   if time.monotonic()>deadline:raise TimeoutError('Original info/movie/cutscene did not retire through ordinary input/voting')
  r['setup_ready']={role:p.performance(role) for role in ROLES};start=time.monotonic();last_report=0
  while True:
   for role,process in p.processes.items():
    if process.poll() is not None:raise RuntimeError(role+' exited during fixture accumulation '+str(process.returncode))
   if 'Enemy model absent from this level' in p.log('host'):raise RuntimeError('Requested enemy model is not loaded by this level')
   if '[port] Stopped:' in p.log('host') or '[port] Stopped:' in p.log('client'):raise RuntimeError('Native game stopped during fixture accumulation')
   count=population('accumulating')['living_fixture_enemies']
   max_waves=int(a.env.get('SSNE_COOP_TEST_ENEMY_MAX_WAVES','0'))
   waves=p.log('host').count('[coop-test-enemy-wave]')
   if count>=a.minimum_enemies and (not max_waves or waves>=max_waves):break
   if time.monotonic()-start>a.setup_timeout:raise TimeoutError('Failed to accumulate living native enemies; max='+str(max(s['living_fixture_enemies'] for s in r['population_samples'])))
   if count!=last_report:print('Native living fixture enemies: '+str(count),flush=True);last_report=count
   p.delay(.6)
  # Capture visible native combat BEFORE timing. The entire measured interval must remain free of presentations.
  p.wait(lambda:all(p.performance(role).get('presentation_busy') == 0 for role in ROLES),15)
  p.pulse('UISHOT');r['before_measurement_screenshots']={role:[str(x) for x in sorted((a.output/role/'steps').glob('*.png'))] for role in ROLES}
  p.delay(1.2);r['spawn_waves_before_measurement']=p.log('host').count('[coop-test-enemy-wave]');r['measurement_first']={role:p.performance(role)['elapsed_s']+1.0 for role in ROLES}
  print('Measuring real host/client native wave at cap '+str(a.fps)+'; alive='+str(len(p.target_enemies())),flush=True)
  start=time.monotonic()
  while time.monotonic()-start<a.seconds:
   for role,process in p.processes.items():
    if process.poll() is not None:raise RuntimeError(role+' exited during measurement '+str(process.returncode))
   if any(p.performance(role).get('presentation_busy') != 0 for role in ROLES):raise RuntimeError('A menu/info/movie/cutscene obscured measured gameplay')
   population('measurement');p.delay(.35)
  r['measurement_last']={role:p.performance(role)['elapsed_s'] for role in ROLES}
  r['measurement_population_min']=min(s['living_fixture_enemies'] for s in r['population_samples'] if s['label']=='measurement')
  r['measurement_population_max']=max(s['living_fixture_enemies'] for s in r['population_samples'] if s['label']=='measurement')
  if r['measurement_population_min']<a.minimum_enemies:raise RuntimeError('Population fell below requested minimum during measurement')
  r['reports']={role:summarize(a.output/role,r['measurement_first'][role],r['measurement_last'][role],a.level,a.scale,a.fps) for role in ROLES}
  p.pulse('UISHOT');r['screenshots']={role:[str(x) for x in sorted((a.output/role/'steps').glob('*'))] for role in ROLES}
  r['final_native_state']=p.state()
  expected='legacy' if a.env.get('SSNE_GPU_TEXTURE_BINDINGS')=='legacy' else 'used-units'
  for role in ROLES:
   console=(a.output/role/'console.log').read_text(errors='replace')
   if '[gpu] texture bindings: '+expected not in console:raise RuntimeError(role+' did not confirm requested texture-binding mode')
  errors=[line for role in ROLES for line in p.log(role).splitlines() if '[port] Stopped:' in line or '[campaign-field-difference]' in line or '[coop-native-error]' in line]
  r['errors']=errors
  assert not errors
  r['passed']=True
 except Exception:r['error']=traceback.format_exc();print(r['error'],flush=True)
 finally:
  p.close();after=snapshot_sources(a);r.update(visible=p.visible,focused=p.focused,exits={role:v.returncode for role,v in p.processes.items()},protected_before=before,protected_after=after,protected_unchanged=before==after)
  r['passed']=r['passed'] and not p.visible and not p.focused and before==after and all(v.returncode==0 for v in p.processes.values())
  a.output.mkdir(parents=True,exist_ok=True);(a.output/'result.json').write_text(json.dumps(r,indent=2)+'\n')
  print(json.dumps({k:v for k,v in r.items() if k in ('passed','error','variant','executable_sha256','target_fps','scale','measurement_population_min','measurement_population_max','reports','exits','protected_unchanged')},indent=2),flush=True)
 return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
