"""Traverse all owned PAL campaign assets through original WorldLink/results flow.
Exit activation is an explicit integration shortcut, not a normal playthrough.
"""
from test_coop_campaign import Peers,sha
from pathlib import Path
import argparse,json,re,traceback

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--exe',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);ap.add_argument('--card',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--port',type=int,default=27819);ap.add_argument('--roots',nargs='*');ap.add_argument('--stop-outside-roots',action='store_true');ap.add_argument('--start-root');a=ap.parse_args()
 for k in ('exe','data','card','output'):setattr(a,k,getattr(a,k).resolve())
 a.output.mkdir(exist_ok=False);assets={p.stem for p in (a.data/'files/Levels').glob('*.ssw') if 'DM' not in p.stem and p.stem!='Gallery'};remaining=set(a.roots or assets);visited=set();before=sha(a.card);results=[];run=0
 names={name.casefold():name for name in assets}
 def identity(level):return names.get(level.casefold(),level)
 def save():
  (a.output/'result.json').write_text(json.dumps({'passed':not remaining and all(r['passed'] for r in results),'exe_sha256':sha(a.exe),'card_unchanged':sha(a.card)==before,'required_levels':sorted(a.roots or assets),'visited':sorted(visited),'remaining':sorted(remaining),'levels':results},indent=2))
 while remaining:
  root=a.start_root if run==0 and a.start_root in remaining else 'Rlevel0_1' if 'Rlevel0_1' in remaining else min(remaining,key=lambda s:({'R':0,'C':1,'A':2}[s[0]],s));run+=1;p=Peers(a,root,a.output/(str(run)+'-'+root));level=root;row=None
  print('Starting campaign chain '+root,flush=True)
  try:
   p.start()
   while identity(level) not in visited:
    row={'level':level,'passed':False,'chain':run};state=p.stage(level);row['state']=state
    checks=[x for x in p.log('client').splitlines() if '[campaign-verify] level='+level+' ' in x];row['verification']=checks
    row['passed']=bool(checks) and all('differences=0' in x for x in checks)
    if not row['passed']:raise RuntimeError('Native replica readback differs: '+str(checks[-3:]))
    visited.add(identity(level));remaining.discard(identity(level));results.append(row);save();print('Verified '+level+' ('+str(len(visited))+'/'+str(len(a.roots or assets))+')',flush=True)
    links=[x for x in state['objects'] if x.get('target')]
    if not links:row['no_exit']=True;break
    link=next((x for x in links if not x.get('terminal')),links[0]);target=link['target'];row['exit']=link
    p.command('link '+target);seq=p.command_sequence;p.wait(lambda:'sequence='+str(seq)+' action=link' in p.log('host'),15)
    if link.get('terminal'):
     p.wait(lambda:'[coop-client-native-results]' in p.log('client'),30);row['results_received']=True
     # The original profile determines whether a terminal exit opens credits
     # (last campaign level) or returns to the main menu (other terminal maps).
     p.wait(lambda:all(p.performance(r).get('scene')==3 for r in p.processes) or '[coop-credits-playhead] role=client' in p.log('client'),60)
     row['credits_received']='[coop-credits-playhead] role=client' in p.log('client');row['menu_received']=all(p.performance(r).get('scene')==3 for r in p.processes);p.delay(3)
     if 'match=0' in '\n'.join(x for x in p.log('client').splitlines() if '[coop-credits-playhead]' in x):raise RuntimeError('Credits playhead mismatch')
     break
    # Require readiness after this exact transition, even if its target was
    # already covered by another branch. New levels continue in the same peers.
    p.wait(lambda:p.ready(target) and p.state().get('level')==target);row['exit_ready']=True;save()
    if identity(target) in visited or (a.stop_outside_roots and identity(target) not in remaining):break
    level=target
   p.close()
  except Exception:
   if row is None:row={'level':level,'passed':False,'chain':run}
   if not any(item is row for item in results):results.append(row)
   row['passed']=False;row['error']=traceback.format_exc();remaining.discard(identity(level));print(row['error'],flush=True)
   if identity(level) not in visited:visited.add(identity(level))
  finally:
   p.close()
   if row is not None:row['visible']=p.visible;row['focused']=p.focused;row['exits']={r:c.returncode for r,c in p.processes.items()};row['passed']&=not p.visible and not p.focused and all(c.returncode==0 for c in p.processes.values())
   save()
 return 0 if all(r['passed'] for r in results) and sha(a.card)==before else 1
if __name__=='__main__':raise SystemExit(main())
