"""Private host/client mouse integration through original native menus.
All pointer input uses the real SDL adapter's hidden queue. Initial boot and
opening Pause are explicitly qualified keyboard fixture setup. The authored
shared intro uses mouse Left/A through the normal transport vote path. The results
case invokes an original authored WorldLink to shorten exploration; it is not
a campaign playthrough and never forces a level load or result action.
"""
from test_native_mouse import MouseMenus,Pointer,public_menu,ident,COMMAND_FILE
from native_mouse_inventory import original_inventory
from test_coop_campaign import sha
from pathlib import Path
import argparse,json,os,re,shutil,subprocess,time,traceback


class MultiplayerMenus(MouseMenus):
 def __init__(self,*args):
  super().__init__(*args);self.lobby_cache={};self.campaign_cache={};self.information_cache={};self.campaign_sequence=0
 def lobby(self,role='host'):return self.copied_json(role,'native-lobby-mouse-state.json',self.lobby_cache)
 def campaign(self,role='host'):return self.copied_json(role,'campaign-state.json',self.campaign_cache)
 def information(self,role='host'):return self.copied_json(role,'native-info-mouse-state.json',self.information_cache)
 def start_role(self,role,language):
  # Menus.start_role normally enables an automatic hidden cutscene A vote.
  # This fixture must prove genuine mouse votes, so bootstrap the same private
  # process/card without either automatic-vote environment key.
  folder=self.folder/role;folder.mkdir(parents=True);(folder/'steps').mkdir();shutil.copy2(self.a.exe,folder/'game.exe');shutil.copy2(self.a.card,folder/'card.raw')
  (folder/'input.txt').write_text('');(folder/'command.txt').write_text('');(folder/'settings.ini').write_text(self.a.private_settings)
  env={key:value for key,value in os.environ.items() if not key.startswith('SSNE_')}
  env.update(SSNE_TEST_HIDDEN='1',SSNE_DEV_NATIVE_MENU='1',SSNE_PAD_SCRIPT_FILE=str(folder/'input.txt'),SSNE_TEST_SCREENSHOT_DIR=str(folder/'steps'),SSNE_PERF_LOG=str(folder/'performance.jsonl'),SSNE_NATIVE_MENU_COMMAND=str(folder/'command.txt'),SSNE_NATIVE_MENU_PORT=str(self.a.port),SSNE_NATIVE_MENU_DISCOVERY_PORT=str(self.a.port+1),SSNE_NATIVE_TEST_LANGUAGE=str(language));env.update(self.a.test_env)
  if role=='host':env.update(SSNE_COOP_CAMPAIGN_VERIFY='1',SSNE_COOP_CAMPAIGN_COMMAND='campaign-command.txt')
  assert 'SSNE_COOP_TEST_SKIP_DELAY_MS' not in env and 'SSNE_TEST_SKIP_FIRST_VOTE' not in env
  startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0;stream=(folder/'console.log').open('wb');self.streams.append(stream)
  self.processes[role]=subprocess.Popen([str(folder/'game.exe'),'--data',str(self.a.data),'--memcard',str(folder/'card.raw'),'--mute','--fps',str(self.a.fps)],cwd=folder,env=env,stdout=stream,stderr=stream,startupinfo=startup,creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS)
  deadline=time.monotonic()+60
  while self.state(role).get('scene')!=3:
   if time.monotonic()>deadline:raise TimeoutError('Boot '+str(self.state(role)))
   self.delay(2)
   if self.state(role).get('scene')==2:self.press(role,'B') # Title boot setup only, before any network session.
  self.delay(3)
 def campaign_command(self,action):
  self.campaign_sequence+=1;path=self.folder/'host/campaign-command.txt';pending=path.with_suffix('.pending')
  pending.write_text(str(self.campaign_sequence)+' '+action,encoding='ascii');deadline=time.monotonic()+2
  while True:
   try:os.replace(pending,path);break
   except PermissionError:
    if time.monotonic()>=deadline:raise
    self.delay(.02)
  marker='sequence='+str(self.campaign_sequence)+' action='+action.split()[0]
  self.wait(lambda:marker in self.log('host'),15)


def record(p,action,**values):
 p.report['steps'].append({'action':action,**values});print(action,flush=True)


def screenshot(p,*roles):
 for role in roles:p.m.press(role,'UISHOT')


def setup_key(p,role,token,reason):
 p.m.press(role,token);record(p,'keyboard_fixture_setup',role=role,token=token,reason=reason,scope='Does not count as mouse menu coverage.')


def page(p,number,role='host'):
 p.m.wait(lambda:p.m.state(role).get('page')==number,15);p.m.delay(.35)


def lobby_state(p,role='host'):
 state=p.m.lobby(role)
 if not state.get('active') or len(state.get('models',[]))!=8 or len(state.get('guis',[]))!=8:raise AssertionError('Incomplete copied lobby state '+str(state))
 return state


def assert_no_dormant_frontend(p,role,context):
 state=p.m.state(role);native=p.observe(role)
 assert state.get('page')==0 and not state.get('visible'),('Frontend page remained logically open',role,context,state)
 assert not any(ident(surface['root'])==0x148 for surface in native.get('surfaces',[])),('Dormant frontend root blocks original GUI lifecycle',role,context,native)
 record(p,'no_dormant_frontend_root',role=role,context=context)


def wait_lobby(p,role='host'):
 p.m.wait(lambda:p.m.state(role).get('lobby') and p.m.lobby(role).get('active') and len(p.m.lobby(role).get('models',[]))==8 and len(p.m.lobby(role).get('guis',[]))==8,30)
 p.m.delay(.7);assert_no_dormant_frontend(p,role,'stable_original_lobby')


def region(p,wanted,role='host',gui=None,root=None,allowed=True):
 matches=[]
 for surface in p.observe(role).get('surfaces',[]):
  if gui is not None and ident(surface['gui'])!=ident(gui):continue
  if root is not None and ident(surface['root'])!=ident(root):continue
  for widget in surface.get('widgets',[]):
   if widget.get('control')!=wanted:continue
   if allowed and not(widget.get('selectable') and widget.get('allowed')):continue
   if len(widget.get('bounds',[]))==4:matches.append({**widget,'gui':surface['gui'],'root':surface['root']})
 if len(matches)!=1:raise AssertionError('Expected one region '+wanted+' role='+role+' gui='+str(gui)+' matches='+str(matches))
 return matches[0]


def models(p,role='host'):return list(lobby_state(p,role)['models'])


def synchronized_models(p):
 p.m.wait(lambda:models(p,'host')==models(p,'client'),20)


def model_change(p,role):
 state=lobby_state(p,role);own=ident(state['own']);before=models(p,'host');widget=region(p,'character.next',role,gui=state['guis'][own])
 screenshot(p,role);p.click_uv(p.center(widget),role)
 p.m.wait(lambda:models(p,'host')[own]!=before[own],15);synchronized_models(p);after=models(p,'host')
 assert all(before[i]==after[i] for i in range(8) if i!=own),('Model change escaped its owner',own,before,after)
 record(p,'owned_avatar_pointer_change_and_replication',role=role,own=own,before=before,after=after,region=widget)


def host_create(p):
 p.expect_root(0x3d);screenshot(p,'host');p.click(0x3f,root=0x3d);page(p,1);p.click(0x14a);page(p,2);p.click(0x14a)
 p.m.wait(lambda:p.m.state().get('active'),25);p.expect_root(0xa7);screenshot(p,'host');p.click(0xa9,root=0xa7);p.expect_root(0x59);p.m.delay(.6);screenshot(p,'host')
 start=None
 for candidate in (0x5d,'footer.start','footer.confirm'):
  try:p.widget(candidate,root=0x59);start=candidate;break
  except AssertionError:pass
 if start is None:raise AssertionError('No original cooperative Start control is hittable')
 p.click(start,root=0x59)
 try:wait_lobby(p)
 except TimeoutError:
  if not p.m.a.allow_start_key_fallback:raise
  setup_key(p,'host','KEY_ENTER','Original cooperative setup did not open the lobby through its pointer Start; this start path remains unproven.')
  wait_lobby(p);p.report['mouse_rules_start_proven']=False
 else:p.report['mouse_rules_start_proven']=True
 record(p,'host_created_through_original_mouse_menus',rules_start_control=start,state=public_menu(p.m.state()))
 # Exercise authoritatively managed bots, then restore exactly two human slots.
 p.click(0x154,root=0x9a);page(p,6);before=p.m.state().get('bots');p.click(0x14a);p.m.wait(lambda:p.m.state().get('bots')==before+1,15)
 skill=p.m.state().get('skill');p.click(0x14b);p.m.wait(lambda:p.m.state().get('skill')!=skill,10)
 p.click(0x14c);p.m.wait(lambda:p.m.state().get('bots')==before,15);p.click(0x14d);wait_lobby(p)
 assert p.m.state().get('bots')==0,public_menu(p.m.state())
 record(p,'host_lobby_bot_add_skill_remove_by_mouse',bots_restored=0)


def client_join_and_roles(p):
 p.m.start_role('client',p.m.a.client_language);p.m.wait(lambda:p.m.mouse('client').get('surfaces') and p.m.mouse_ack('client').get('image_rect'),20)
 p.command('client','cancel');screenshot(p,'client');p.click(0x3f,'client',root=0x3d);page(p,1,'client');p.click(0x14b,'client');page(p,3,'client');p.click(0x14d,'client');page(p,8,'client')
 p.m.wait(lambda:p.m.state('client').get('rooms',0)>0,20);p.m.delay(.8);screenshot(p,'client');p.click(0x14a,'client')
 p.m.wait(lambda:p.m.state('client').get('active') and p.m.state().get('players')==2,40);wait_lobby(p,'host');wait_lobby(p,'client');synchronized_models(p)
 assert ident(lobby_state(p,'host')['own'])==0
 record(p,'client_joined_discovered_host_by_mouse',host=public_menu(p.m.state()),client=public_menu(p.m.state('client')),scope='LAN room selection supplied the advertised endpoint; no PIN text or join shortcut was injected.')
 model_change(p,'host');model_change(p,'client')
 # The denied remote model still publishes its true native render geometry.
 foreign=lobby_state(p,'client');own=ident(foreign['own']);before=models(p,'host');host_slot=0
 denied=region(p,'character.next','client',gui=foreign['guis'][host_slot],allowed=False)
 assert not denied.get('allowed'),denied
 p.click_uv(p.center(denied),'client');p.m.delay(.6);synchronized_models(p)
 assert models(p,'host')==before and models(p,'client')==before,('Client changed foreign avatar',before,models(p,'host'))
 record(p,'client_foreign_avatar_click_denied',client_own=own,target_slot=host_slot,region=denied,models_unchanged=before)
 host_bots=p.widget(0x154,root=0x9a)
 assert not any(w.get('id')==0x154 and w.get('allowed') and w.get('selectable') for s in p.observe('client').get('surfaces',[]) for w in s.get('widgets',[]))
 p.no_action(p.center(host_bots),'client','host_only_lobby_bots_area');assert p.m.state().get('bots')==0
 record(p,'client_lobby_bot_authority_denied',scope='Host-only Bots is absent or denied on the client; clicking the matching native layout area leaves roster and page unchanged.')


def carousel_debug(p):
 surfaces=p.m.carousel().get('surfaces',[])
 if len(surfaces)!=1:raise AssertionError('Expected one original host carousel '+str(surfaces))
 return surfaces[0]


def begin_campaign(p,exercise_neighbors=True):
 p.click(0x152,root=0x9a);p.expect_root(0x4b);p.m.wait(lambda:len(p.m.carousel().get('surfaces',[]))==1,10);p.m.delay(.8);screenshot(p,'host','client')
 state=carousel_debug(p);count=ident(state['count']);selected=ident(state['selected'])
 if exercise_neighbors and count>1 and selected+1<count:
  p.click('carousel.next',root=0x4b);p.m.wait(lambda:ident(carousel_debug(p)['selected'])==selected+1,8);p.m.delay(.6)
  p.click('carousel.previous',root=0x4b);p.m.wait(lambda:ident(carousel_debug(p)['selected'])==selected,8);p.m.delay(.6)
 state=carousel_debug(p);wanted=[entry for entry in state.get('entries',[]) if entry.get('level')=='Rlevel1_1']
 assert len(wanted)==1,('Intended original campaign entry is unavailable; no index or forced load is substituted',state)
 target=ident(wanted[0]['index'])
 for _ in range(count):
  state=carousel_debug(p);current=ident(state['selected'])
  if current==target:break
  step=1 if current<target else -1;expected=current+step
  widget=p.widget('carousel.confirm',root=0x4b);p.wheel(p.center(widget),-step);p.m.wait(lambda:ident(carousel_debug(p)['selected'])==expected,8);p.m.delay(.5)
 assert carousel_debug(p).get('selected_level')=='Rlevel1_1' and ident(carousel_debug(p)['selected'])==target,carousel_debug(p)
 record(p,'authored_campaign_entry_selected_by_pointer',entry=wanted[0],state=carousel_debug(p))
 p.click('carousel.confirm',root=0x4b);p.expect_root(0x44);p.m.delay(.6);screenshot(p,'host');p.click(0x47,root=0x44)
 p.m.wait(lambda:all(p.m.state(role).get('scene')==4 for role in ('host','client')),75)
 # Runtime readiness is observed without keyboard intro skipping or hidden
 # automatic votes. Retained original Info is closed below with the mouse.
 p.m.wait(lambda:all(p.m.performance(role).get('local_ready') and p.m.performance(role).get('ready_players')==2 for role in ('host','client')),90)
 for role in ('host','client'):record(p,'initial_information_retained_for_mouse_test',role=role,info=bool(p.m.state(role).get('info')),native=p.observe(role))
 close_initial_information_by_mouse(p);settle_original_intro_by_mouse(p)
 record(p,'authoritative_pointer_map_and_difficulty_started_both_peers',level='Rlevel1_1',difficulty_id=0x47,performance={r:p.m.performance(r) for r in ('host','client')})


def afk(p,slot):
 values=re.findall(r'\[coop-afk-state\][^\n]*\bslot='+str(slot)+r'\b[^\n]*\bafk=([01])\b',p.m.log('host'))
 return bool(values and values[-1]=='1')


def open_pause(p,role):
 if not p.m.state(role).get('pause'):setup_key(p,role,'START','Open the original Pause context; subsequent Connection/Bots/AFK/Back actions use only mouse input.')
 p.m.wait(lambda:p.m.state(role).get('pause'),15);p.expect_root(0x129,role);p.m.delay(.4);screenshot(p,role)


def connection(p,role):
 p.click(0x14f,role,root=0x129);page(p,7,role)
 assert any(ident(surface['root'])==0x148 for surface in p.observe(role).get('surfaces',[])),('Real Connection did not expose its native frontend root',role,p.observe(role))
 assert p.m.state(role).get('gui_port')==p.m.state(role).get('local_slot'),public_menu(p.m.state(role))


def toggle_afk(p,role,value):
 slot=ident(p.m.state(role)['local_slot']);assert afk(p,slot)!=value,('AFK precondition',role,slot,value)
 p.click(0x14d,role);p.m.wait(lambda:afk(p,slot)==value,10)
 record(p,'owned_afk_changed_by_mouse',role=role,slot=slot,afk=value)


def information_surface(p,role):
 own=ident(p.m.state(role)['local_slot']);surfaces=p.m.information(role).get('surfaces',[])
 matches=[s for s in surfaces if ident(s.get('slot',999))==own]
 if len(matches)!=1:raise AssertionError('Expected one owned information surface '+str({'role':role,'own':own,'surfaces':surfaces}))
 state=matches[0];count=ident(state['count']);selected=ident(state['selected'])
 assert all(ident(state[key])>0 for key in ('gui','node','owner','actor')),state
 assert 0<=count<=512 and (not count or 0<=selected<count),state
 for surface in p.observe(role).get('surfaces',[]):
  for widget in surface.get('widgets',[]):
   if str(widget.get('control','')).startswith('info.') and widget.get('allowed'):
    assert surface['gui']==state['gui'],('Information permission escaped local actor',own,state,surface['gui'],widget)
 return dict(state)


def information_controls(p,role,state):
 controls={}
 for surface in p.observe(role).get('surfaces',[]):
  if surface['gui']!=state['gui'] or ident(surface['root'])!=0xe3:continue
  for widget in surface.get('widgets',[]):
   name=widget.get('control','')
   if not name.startswith('info.') or not widget.get('allowed') or not widget.get('selectable'):continue
   try:p.center(widget)
   except AssertionError:continue
   controls[name]=dict(widget)
 return controls


def wait_information_retired(p,role,reason):
 # The public info bit can clear before its original GUI closing animation.
 # Reopening during that interval measures the old closing surface instead.
 p.m.wait(lambda:not p.m.state(role).get('info') and not p.observe(role).get('has_surface') and not p.m.information(role).get('surfaces'),15)
 record(p,'information_gui_retired',role=role,reason=reason,native_frame=p.observe(role).get('frame'))


def close_initial_information_by_mouse(p):
 for role in ('host','client'):
  other='client' if role=='host' else 'host'
  if not p.m.state(role).get('info'):
   record(p,'initial_information_already_closed',role=role,mouse_back_proven=False,information=p.m.information(role),menu=dict(p.m.state(role)));continue
  own=ident(p.m.state(role)['local_slot'])
  p.m.wait(lambda:not p.m.state(role).get('info') or (p.ack(role).get('active') and any(ident(surface.get('slot',999))==own for surface in p.m.information(role).get('surfaces',[]))),12)
  if not p.m.state(role).get('info'):
   record(p,'initial_information_closed_by_original_transition',role=role,mouse_back_proven=False,information=p.m.information(role),menu=dict(p.m.state(role)));continue
  initial=information_surface(p,role);foreign_before={'menu':dict(p.m.state(other)),'information':p.m.information(other)};body=information_controls(p,role,initial).get('info.body')
  assert body,'Automatic original Info has no owned body region for native mouse Back'
  before=p.observe(role).get('activations',0);screenshot(p,role);p.click_uv(p.center(body),role,button='right')
  assert p.observe(role).get('activations',0)==before+1,('Automatic Info Back was not accepted exactly once',role,before,p.observe(role))
  p.m.wait(lambda:not p.m.state(role).get('info'),12)
  record(p,'automatic_information_original_mouse_back',role=role,slot=own,initial=initial,control='info.body_right_back',own_information_closed=True,foreign_before=foreign_before,foreign_after={'menu':dict(p.m.state(other)),'information':p.m.information(other)},scope='Owned root, accepted Back and own close are proved. The shared original intro may legitimately transition both GUI views; foreign stability is tested separately after it ends.')


def settle_original_intro_by_mouse(p):
 pattern=r'\[coop-cutscene-vote-start\] id=(\d+) movie=0 level_ref=2801\b'
 p.m.wait(lambda:re.search(pattern,p.m.log('host')) is not None,60)
 match=list(re.finditer(pattern,p.m.log('host')))[-1];vote=int(match.group(1));offset=match.start();actions=[];deadline=time.monotonic()+60
 def sequence_log():return p.m.log('host')[offset:]
 def ended():return '[coop-cutscene-vote-end] native sequence ended' in sequence_log()
 initially_ended=ended()
 while not ended():
  if time.monotonic()>deadline:raise TimeoutError('Authored initial intro2801 did not end through its original vote lifecycle')
  for role in ('host','client'):
   if ended():break
   if any(action['role']==role for action in actions):continue
   ack=p.ack(role);native=p.observe(role)
   if not ack.get('presentation') or native.get('has_surface') or p.m.state(role).get('info'):continue
   # An empty corner remains harmless if a native GUI appears after Move.
   uv=[.98,.05];p.move(uv,role);ack=p.ack(role);native=p.observe(role)
   if ended() or not ack.get('presentation') or native.get('has_surface') or p.m.state(role).get('info'):p.cancel(role);continue
   before=dict(ack);p.button(True,uv,role);after_down=dict(p.ack(role));p.m.delay(.06);p.button(False,uv,role)
   assert after_down.get('fallback_sequence',0)>before.get('fallback_sequence',0),('Intro mouse Left/A did not reach the native presentation adapter',role,before,after_down)
   actions.append({'role':role,'uv':uv,'fallback_sequence_before':before.get('fallback_sequence'),'fallback_sequence_after':after_down.get('fallback_sequence')});record(p,'shared_original_intro_mouse_vote',vote=vote,**actions[-1])
  p.m.delay(.1)
 log=sequence_log();commit=re.search(r'\[coop-cutscene-vote-commit\] id='+str(vote)+r' received=(\d+) required=(\d+)',log)
 mouse_skip_proven=len(actions)==2 and commit is not None and commit.group(1)==commit.group(2)=='2'
 if not initially_ended:assert mouse_skip_proven,('The observed active original intro ended without two proved mouse votes and a native2/2 commit; no mouse Skip coverage is claimed',vote,actions,commit.group(0) if commit else None)
 p.m.wait(lambda:all(not p.m.state(role).get('info') and not p.observe(role).get('has_surface') and not p.ack(role).get('presentation') and p.m.performance(role).get('level')=='Rlevel1_1' and p.m.performance(role).get('local_ready') and p.m.performance(role).get('ready_players')==2 for role in ('host','client')),45)
 record(p,'authored_initial_intro_native_vote_end',level_ref=2801,vote=vote,initially_ended=initially_ended,mouse_skip_proven=mouse_skip_proven,mouse_actions=actions,commit=commit.group(0) if commit else None,native_vote_end=True,automatic_vote_environment_absent=True,scope='Original sequence end and both ready peers are required. If the authored intro ended before mouse voting, no mouse Skip proof is claimed; no keyboard vote or script mutation is substituted.')


def information_context(p,role):
 other='client' if role=='host' else 'host';foreign_slot=ident(p.m.state(other)['local_slot'])
 def foreign_snapshot():
  return {'info':bool(p.m.state(other).get('info')),'owned_surfaces':[dict(surface) for surface in p.m.information(other).get('surfaces',[]) if ident(surface.get('slot',999))==foreign_slot]}
 if p.m.state(other).get('info'):p.m.wait(lambda:len(foreign_snapshot()['owned_surfaces'])==1,12)
 foreign_baseline=foreign_snapshot();foreign_checks=[]
 def assert_foreign_unchanged(action):
  current=foreign_snapshot();assert current==foreign_baseline,('Local information action changed the foreign view',role,action,foreign_baseline,current)
  foreign_checks.append(action)
 automatic=bool(p.m.state(role).get('info'))
 if not automatic:
  wait_information_retired(p,role,'before_manual_information_open')
  setup_key(p,role,'KEY_TAB','Open the original local information context; opening is keyboard fixture setup only.')
 record(p,'information_opening_context',role=role,automatic_initial_information=automatic)
 try:p.m.wait(lambda:p.m.state(role).get('info'),12)
 except TimeoutError:
  gap={'context':'information','role':role,'reason':'Original information context did not open in this authored level state.'}
  p.report.setdefault('coverage_gaps',[]).append(gap);record(p,'information_mouse_gap',**gap);return
 # Wait for the newly opened local GUI and adapter snapshot, rather than
 # treating their asynchronous publication or an opening animation as a gap.
 own=ident(p.m.state(role)['local_slot'])
 try:p.m.wait(lambda:p.observe(role).get('has_surface') and p.ack(role).get('active') and any(ident(s.get('slot',999))==own for s in p.m.information(role).get('surfaces',[])),12)
 except TimeoutError:pass
 screenshot(p,role);native=p.observe(role);ack=p.ack(role);roots=sorted({s['root'] for s in native.get('surfaces',[])})
 if not native.get('has_surface') or not ack.get('active') or not any(ident(s.get('slot',999))==own for s in p.m.information(role).get('surfaces',[])):
  gap={'context':'information','role':role,'reason':'Information is open but no active native GUI pointer surface is exposed. No blind confirm was sent.','native_roots':roots,'has_surface':native.get('has_surface'),'pointer_active':ack.get('active')}
  p.report.setdefault('coverage_gaps',[]).append(gap);record(p,'information_mouse_gap',**gap)
  setup_key(p,role,'KEY_TAB','Close the unproven information context so other independent menu checks can continue.')
  p.m.wait(lambda:not p.m.state(role).get('info'),12);return
 p.m.wait(lambda:any(ident(s.get('slot',999))==ident(p.m.state(role)['local_slot']) for s in p.m.information(role).get('surfaces',[])),10)
 initial=information_surface(p,role);identity={key:initial[key] for key in ('gui','node','owner','actor','slot')}
 assert_foreign_unchanged('original_open_or_retained_automatic_view')
 record(p,'information_foreign_view_baseline',role=role,foreign_role=other,baseline=foreign_baseline,scope='Both automatic native Info views may initially be open. Local actions must preserve the other view boolean, native identity, selected page and scroll.')
 page_changes=[];scroll_observations=[];tested=set();tab_click_proven=False;scroll_proven=False;visible_scroll=False
 # Inspect available original entries only. No inventory, knowledge or page
 # state is granted/written by the fixture to manufacture a longer page.
 for _ in range(min(max(1,ident(initial['count'])),8)):
  current=information_surface(p,role);assert {key:current[key] for key in identity}==identity
  index=ident(current['selected'])
  if index in tested:break
  tested.add(index);controls=information_controls(p,role,current);arrows=[name for name in controls if name.startswith('info.scroll.')];visible_scroll|=bool(arrows)
  body=controls.get('info.body')
  if body:
   p.m.delay(.25);before=information_surface(p,role);start=float(before['scroll'])
   for amount in (-3,3):
    p.wheel(p.center(body),amount,role);after=information_surface(p,role);assert_foreign_unchanged('body_wheel_'+str(amount))
    assert ident(after['selected'])==index and {key:after[key] for key in identity}==identity
    observation={'page':index,'wheel':amount,'before':start,'after':float(after['scroll']),'visible_native_arrows':arrows};scroll_observations.append(observation)
    if abs(float(after['scroll'])-start)>1e-4:
     mid=float(after['scroll']);p.wheel(p.center(body),-amount,role);restored=information_surface(p,role);assert_foreign_unchanged('body_wheel_reverse_'+str(-amount))
     assert abs(float(restored['scroll'])-mid)>1e-4,('Native information scroll did not respond in reverse',role,observation,restored)
     scroll_observations.append({'page':index,'wheel':-amount,'before':mid,'after':float(restored['scroll']),'reverse':True});scroll_proven=True;break
    start=float(after['scroll'])
  if ident(initial['count'])<=1:break
  # Click a genuinely visible retained tab. Offscreen or hidden entries are
  # never selected by a diagnostic command or a guessed rectangle.
  candidates=[]
  for name,widget in information_controls(p,role,current).items():
   if name.startswith('info.tab.'):
    target=int(name.rsplit('.',1)[1])
    if target!=index and target not in tested:candidates.append((target,name,widget))
  if not candidates:break
  target,name,widget=sorted(candidates)[0]
  record(p,'information_tab_before',role=role,target=target,control=name,information=p.m.information(role),mouse=p.observe(role),ack=p.ack(role),menu=dict(p.m.state(role)))
  p.click(name,role,root=0xe3)
  record(p,'information_tab_after_click',role=role,target=target,control=name,information=p.m.information(role),mouse=p.observe(role),ack=p.ack(role),menu=dict(p.m.state(role)))
  selected_observation={}
  def selected_target_ready():
   owned=[surface for surface in p.m.information(role).get('surfaces',[]) if ident(surface.get('slot',999))==own]
   valid=len(owned)==1 and ident(owned[0].get('selected',999))==target and all(owned[0].get(key)==value for key,value in identity.items())
   if valid:selected_observation['state']=dict(owned[0])
   return valid
  # Copied diagnostics are published on their own frame boundary. A temporary
  # empty copy is not success; the same owner/GUI must expose the target page
  # within the original strict timeout, then survive the final validation.
  p.m.wait(selected_target_ready,10);changed=selected_observation['state']
  assert ident(changed['selected'])==target and {key:changed[key] for key in identity}==identity;assert_foreign_unchanged('tab_click_'+str(target))
  page_changes.append({'control':name,'before':index,'after':ident(changed['selected']),'region':widget});tab_click_proven=True
  if scroll_proven:break
 if ident(initial['count'])>1 and not tab_click_proven:
  record(p,'information_multi_page_unobserved',role=role,count=initial['count'],reason='No other retained tab was visible and hittable in the observed original viewport; no hidden tab was forced.')
 if visible_scroll and not scroll_proven:
  gap={'context':'information_scroll','role':role,'reason':'Visible original scroll arrows were exposed, but wheel input did not change the copied native scroll value.','tested_pages':sorted(tested)}
  p.report.setdefault('coverage_gaps',[]).append(gap);record(p,'information_mouse_gap',**gap)
 record(p,'owned_information_pages_and_native_scroll',role=role,initial=initial,final=information_surface(p,role),actual_tab_click_proven=tab_click_proven,page_changes=page_changes,scroll_proven=scroll_proven,scroll_observations=scroll_observations,tested_pages=sorted(tested),scope='Only observed original entries and native scroll changes count. One entry or no visible scrolling controls does not imply multipage/scroll coverage.')
 screenshot(p,role)
 try:back=p.widget('footer.back',role,root=0xe3)
 except AssertionError:back=None
 if back:p.click('footer.back',role,root=0xe3)
 else:
  controls=information_controls(p,role,information_surface(p,role));body=controls.get('info.body')
  if body:p.click_uv(p.center(body),role,button='right')
  else:p.back(role)
 p.m.wait(lambda:not p.m.state(role).get('info'),12)
 assert_foreign_unchanged('mouse_back')
 record(p,'original_information_native_mouse_back',role=role,native_roots=roots,slot=initial['slot'],control='footer.back' if back else 'info.body_right_back',foreign_role=other,foreign_baseline=foreign_baseline,foreign_final=foreign_snapshot(),foreign_unchanged_checks=foreign_checks)


def gameplay_menus(p,keep_host_pause=False):
 information_context(p,'host');information_context(p,'client')
 open_pause(p,'host');connection(p,'host');host_bots=p.widget(0x14b,root=0x148)
 toggle_afk(p,'host',True);toggle_afk(p,'host',False)
 p.click(0x14b);page(p,6);p.click(0x14a);p.m.wait(lambda:p.m.state().get('bots')==1,15);p.click(0x14c);p.m.wait(lambda:p.m.state().get('bots')==0,15);p.click(0x14d);p.expect_root(0x129);p.m.delay(.7);assert_no_dormant_frontend(p,'host','returned_to_original_pause')
 open_pause(p,'client');connection(p,'client');client_before=public_menu(p.m.state('client'));p.no_action(p.center(host_bots),'client','host_only_connection_bots_area')
 assert p.m.state('client').get('page')==7 and p.m.state().get('bots')==0
 toggle_afk(p,'client',True);toggle_afk(p,'client',False);p.click(0x14a,'client');p.expect_root(0x129,'client');p.back('client');p.m.wait(lambda:not p.m.state('client').get('pause'),10);p.m.delay(.7);assert_no_dormant_frontend(p,'client','pause_closed')
 if not keep_host_pause:p.back();p.m.wait(lambda:not p.m.state().get('pause'),10);p.m.delay(.7);assert_no_dormant_frontend(p,'host','pause_closed')
 record(p,'both_owned_pause_connection_menus_mouse_operated',client_before=client_before,host_pause_left_for_results=keep_host_pause)


def result_sequences(p,role):
 marker='[coop-native-results]' if role=='host' else '[coop-client-native-results]'
 return re.findall(re.escape(marker)+r' sequence=(\d+)',p.m.log(role))


def results_menus(p):
 open_pause(p,'host');connection(p,'host');toggle_afk(p,'host',True);p.click(0x14a);p.expect_root(0x129);p.back();p.m.wait(lambda:not p.m.state().get('pause'),10)
 p.m.wait(lambda:p.m.campaign().get('level')=='Rlevel1_1',20);state=p.m.campaign();links=[x for x in state.get('objects',[]) if x.get('target')=='Rlevel1_2' and not x.get('terminal')]
 assert links,'No original authored Rlevel1_1→Rlevel1_2 WorldLink'
 cut={role:len(p.m.log(role)) for role in ('host','client')};p.m.campaign_command('link Rlevel1_2')
 p.m.wait(lambda:result_sequences(p,'host') and result_sequences(p,'client') and p.m.state('client').get('results_root')==0xbc,30)
 sequence=result_sequences(p,'host')[-1];assert result_sequences(p,'client')[-1]==sequence
 screenshot(p,'host','client')
 denied=p.widget('footer.confirm','client',root=0xbc,allowed=False);assert not denied.get('allowed'),denied
 p.click_uv(p.center(denied),'client');p.m.delay(.3)
 assert p.m.state('client').get('results_root')==0xbc and p.m.performance('host').get('level')=='Rlevel1_1'
 assert result_sequences(p,'host')[-1]==sequence
 record(p,'client_result_continue_pointer_denied',sequence=sequence,region=denied)
 # Change AFK through the real Connection controls. The Build196 results
 # deadline remains unconditional; the following manual actions must beat it.
 modal_cut=len(p.m.log('host'));p.click(0x151,root=0xbc);page(p,7);toggle_afk(p,'host',False);p.click(0x14a)
 p.m.wait(lambda:p.m.state().get('page')==0 and p.m.state().get('results_root')==0xbc and p.m.state().get('native_focus')==0xbc,10)
 assert result_sequences(p,'host')[-1]==sequence
 modal=p.m.log('host')[modal_cut:];assert '[coop-results-setup]' not in modal and '[coop-results-cleanup]' not in modal,'Connection changed the original result lifetime'
 screenshot(p,'host')
 manual_actions=[]
 for attempt in range(2):
  assert '[coop-results-auto-continue]' not in p.m.log('host')[cut['host']:],'Manual Continue lost to the results deadline'
  assert result_sequences(p,'host')[-1]==sequence,'Continue escaped its original result lifetime'
  before=p.observe().get('activations',0)
  if p.m.a.continue_method=='keyboard':setup_key(p,'host','KEY_ENTER','Regression: Enter after closing result Connection must continue original BC; this is not mouse Continue proof.')
  else:p.click('footer.confirm',root=0xbc)
  after=p.observe().get('activations',0)
  if p.m.a.continue_method=='mouse':assert after==before+1,('Result Continue did not activate exactly once',attempt,before,after)
  current=public_menu(p.m.state());log_now=p.m.log('host')[cut['host']:]
  manual_actions.append({'attempt':attempt+1,'method':p.m.a.continue_method,'activations_before':before,'activations_after':after,'menu_after':current,'save_requested':'[coop-native-progress-save-request]' in log_now})
  # Original 801091A4 first completes statistics when its phase is not2;
  # a later Continue exits phase2. Infer that first stage only from a
  # successful action and a still-hittable BC footer, never a phase readback.
  if attempt or '[coop-native-progress-save-request]' in log_now or p.m.state().get('results_root')!=0xbc or p.m.performance('host').get('level')!='Rlevel1_1':break
  try:p.widget('footer.confirm',root=0xbc)
  except AssertionError:break
 record(p,'manual_original_result_continue_actions',sequence=sequence,actions=manual_actions,two_stage_inferred=len(manual_actions)==2,phase_readback=False,source='Original PAL 801091A4 completes statistics before its phase2 exit path.')
 p.m.wait(lambda:all(p.m.performance(role).get('level')=='Rlevel1_2' and p.m.performance(role).get('local_ready') and p.m.performance(role).get('ready_players')==2 for role in ('host','client')),90)
 log=p.m.log('host')[cut['host']:];assert '[coop-results-auto-continue]' not in log,'Manual mouse Continue was replaced by timeout'
 saves=[line for line in log.splitlines() if '[coop-native-progress-save-request]' in line];assert len(saves)==1,saves
 assert not afk(p,0)
 record(p,'host_result_connection_afk_off_and_continue',continue_method=p.m.a.continue_method,result_focus_after_mouse_back=0xbc,sequence=sequence,progress_saves=saves,target='Rlevel1_2',exit_fixture='Original authored WorldLink handler, shortened exploration only.',performance={r:p.m.performance(r) for r in ('host','client')})
 screenshot(p,'host','client')


def leave_sessions(p):
 for role in ('client','host'):
  if not p.m.state(role).get('active'):raise AssertionError('Peer ended before its explicit mouse Leave '+role)
  if p.m.state(role).get('scene')==4:
   if p.m.state(role).get('info'):
    own=ident(p.m.state(role)['local_slot']);p.m.wait(lambda:p.ack(role).get('active') and any(ident(surface.get('slot',999))==own for surface in p.m.information(role).get('surfaces',[])),12)
    initial=information_surface(p,role);body=information_controls(p,role,initial).get('info.body');assert body,('Next-level Info lacks its owned native mouse Back region',role,initial)
    before=p.observe(role).get('activations',0);p.click_uv(p.center(body),role,button='right')
    assert p.observe(role).get('activations',0)==before+1,('Next-level Info Back did not activate exactly once',role,before,p.observe(role))
    p.m.wait(lambda:not p.m.state(role).get('info'),12)
    record(p,'next_level_information_mouse_back_before_leave',role=role,slot=own,initial=initial,own_information_closed=True,scope='Original owned mouse Back only; no Tab/key skip or automatic Skip vote is used to close the new level Info.')
   if not p.m.state(role).get('pause'):wait_information_retired(p,role,'next_level_information_retired_before_pause_leave')
   open_pause(p,role);connection(p,role);context='original_pause_connection'
  else:
   wait_lobby(p,role);p.click(0x153,role,root=0x9a);page(p,7,role);context='original_lobby_connection'
  screenshot(p,role);p.click(0x14c,role)
  p.m.wait(lambda:not p.m.state(role).get('active') and p.m.state(role).get('scene')==3,30);p.expect_root(0x3d,role)
  if role=='client':p.m.wait(lambda:p.m.state().get('players')==1,20)
  record(p,'native_connection_leave_by_mouse',role=role,context=context,state=public_menu(p.m.state(role)))
  screenshot(p,role)


def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--case',choices=('lobby','gameplay','results','all'),default='all');ap.add_argument('--language',type=int,choices=(0,1,2),default=1);ap.add_argument('--client-language',type=int,choices=(0,1,2),default=2)
 ap.add_argument('--continue-method',choices=('mouse','keyboard'),default='mouse')
 ap.add_argument('--aspect',choices=('4x3','16x9','21x9'),default='16x9');ap.add_argument('--fps',type=int,default=60);ap.add_argument('--port',type=int,default=27940);ap.add_argument('--allow-start-key-fallback',action='store_true');a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.mkdir(parents=True,exist_ok=False);original=sha(a.card);aspect,width={'4x3':(0,960),'16x9':(3,960),'21x9':(3,1260)}[a.aspect]
 a.private_settings='[graphics]\nrenderer=gpu\nscale=1\naspect='+str(aspect)+'\nhud_edges=1\nside_fill=1\nvsync=0\nwindow_w='+str(width)+'\nwindow_h=540\ntop_bar=0\nfullscreen=0\n[audio]\nmuted=1\n[general]\npause_unfocused=0\n'
 a.test_env={'SSNE_TEST_DEVICE_INPUT':'1','SSNE_NATIVE_MOUSE_COMMAND':COMMAND_FILE,'SSNE_NATIVE_INFO_MOUSE_TRACE':'1'};m=MultiplayerMenus(a,'',a.output)
 report={'passed':False,'case':a.case,'continue_method':a.continue_method,'exe_sha256':sha(a.exe),'driver_sha256':sha(Path(__file__)),'language':a.language,'client_language':a.client_language,'aspect':a.aspect,'fps':a.fps,'steps':[],'scope':__doc__};p=Pointer(m,report)
 try:
  m.start_role('host',a.language);m.wait(lambda:m.mouse().get('surfaces') and m.mouse_ack().get('image_rect'),20);p.command('host','cancel');host_create(p);client_join_and_roles(p)
  if a.case!='lobby':
   begin_campaign(p)
   if a.case in ('gameplay','all'):gameplay_menus(p,keep_host_pause=a.case=='all')
   if a.case in ('results','all'):results_menus(p)
  leave_sessions(p)
  report['passed']=not report.get('coverage_gaps')
 except Exception:
  report['error']=traceback.format_exc();print(report['error'],flush=True)
  # Preserve the last copied guest state before screenshots/Cancel/Quit can
  # retire an original Info root and overwrite its diagnostic JSON.
  report['failure_diagnostics_before_cleanup']={role:{'information':m.information(role),'mouse':p.observe(role),'ack':p.ack(role),'menu':dict(m.state(role))} for role in m.processes}
  for role,process in m.processes.items():
   if process.poll() is None:
    try:screenshot(p,role)
    except Exception:pass
 finally:
  p.save_coverage(original_inventory(a.data,a.language));report['final_states']={r:public_menu(m.state(r)) for r in m.processes}
  for role,process in m.processes.items():
   if process.poll() is None:
    try:p.cancel(role)
    except Exception:pass
  m.close();report.update(visible=m.visible,focused=m.focused,exits={r:x.returncode for r,x in m.processes.items()},original_card_unchanged=sha(a.card)==original)
  report['passed'] &= not m.visible and not m.focused and report['original_card_unchanged'] and all(x==0 for x in report['exits'].values())
  (a.output/'result.json').write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps({k:report.get(k) for k in ('passed','case','error','mouse_rules_start_proven')},indent=2),flush=True)
 return 0 if report['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
