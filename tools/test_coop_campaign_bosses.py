"""Hidden original boss spawners/receivers and mid-encounter client rejoin.
Damage injection is an integration shortcut, not proof of normal aiming or tactics.
"""
from pathlib import Path
from test_coop_campaign import Peers,sha
import argparse,json,traceback,time

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--exe',type=Path,required=True);ap.add_argument('--data',type=Path,required=True);ap.add_argument('--card',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--port',type=int,default=27816);ap.add_argument('--levels',nargs='*');a=ap.parse_args()
 for k in ('exe','data','card','output'):setattr(a,k,getattr(a,k).resolve())
 a.output.mkdir(exist_ok=False);before=sha(a.card);results=[]
 for level,definition in [('Rlevel4_4b',40),('Clevel8_4',45),('Alevel12_4',42)]:
  if a.levels and level not in a.levels:continue
  p=Peers(a,level,a.output/level);row={'level':level,'boss_type':definition,'exe_sha256':sha(a.exe),'passed':False,'phases':[]};print('Boss '+level,flush=True);prior=''
  def bosses():return [o for o in p.state().get('objects',[]) if o.get('kind')==1 and o.get('definition') in (40,41,42,43,44,45)]
  def hit(command):
   p.command(command);seq=p.command_sequence;p.wait(lambda:'sequence='+str(seq)+' action=damage' in p.log('host'),10);p.delay(.6)
  def record():p.delay(2);p.pulse('SHOT');row['phases'].append(p.state())
  try:
   p.start();state=p.stage(level);row['phases'].append(state);spawner=next(o for o in state['objects'] if o.get('spawn_type')==definition)
   p.command('event '+str(spawner['index'])+' 13');p.wait(lambda:'action=event index='+str(spawner['index']) in p.log('host'),10);p.wait(lambda:any(o['definition']==definition for o in bosses()),25);record();print('Spawned '+str(definition),flush=True)
   if definition==42:
    boss=next(o for o in bosses() if o['definition']==42);points=boss.get('weakpoints',[]);assert len(points)==112 and sum(points[i*7] for i in range(16))==16
    for index in (3,4,5):hit('node_damage '+str(boss['id'])+' '+str(index)+' '+str(max(1,points[index*7+3]*.25)))
    hit('node_damage '+str(boss['id'])+' 3 '+str(max(1,points[3*7+3]*2)))
   else:
    for boss in bosses():
     if boss.get('health',0)>0:hit('damage '+str(boss['id'])+' '+str(max(1,boss['health']*.25)))
   record();prior=p.log('client');p.restart_client();row['phases'].append(p.stage(level));row['late_join_ready']=True;print('Late join ready '+level,flush=True)
   if definition==42:
    # Native phases gate vulnerability, and destroying the outer parts opens
    # the inner regions. Revisit living regions instead of assuming one fixed
    # shot order can bypass those original encounter rules.
    deadline=time.monotonic()+100
    while time.monotonic()<deadline:
     current=next((o for o in bosses() if o['definition']==42),None)
     if not current or not any(current['weakpoints'][i*7] for i in range(16)):break
     for index in [*range(3,16),2,0,1]:
      current=next((o for o in bosses() if o['definition']==42),None)
      if not current:break
      points=current['weakpoints']
      if points[index*7] and points[index*7+1]==1:hit('node_damage '+str(current['id'])+' '+str(index)+' '+str(max(1,points[index*7+3]*2)))
     p.delay(1)
    record();remaining=next((o for o in bosses() if o['definition']==42),None)
    row['remaining_weakpoints']=sum(remaining['weakpoints'][i*7] for i in range(16)) if remaining else 0
    assert row['remaining_weakpoints']==0,'Original boss weakpoints did not retire'
   else:
    for boss in bosses():
     if boss.get('health',0)>0:hit('damage '+str(boss['id'])+' '+str(max(1,boss['health']*2)))
    record();p.wait(lambda:not any(o.get('health',0)>0 for o in bosses()),15)
   p.close();logs=prior+'\n'+p.log('client');checks=[x for x in logs.splitlines() if '[campaign-verify]' in x];row['verification_samples']=checks;row['errors']=[x for x in logs.splitlines() if '[campaign-field-difference]' in x or '[port] Stopped:' in x];row['native_damage']=[x for x in p.log('host').splitlines() if '[campaign-command]' in x];row['passed']=bool(checks) and not row['errors'] and all('differences=0' in x for x in checks)
  except Exception:row['error']=traceback.format_exc();print(row['error'],flush=True)
  finally:
   p.close();row['visible']=p.visible;row['focused']=p.focused;row['exits']={r:c.returncode for r,c in p.processes.items()};row['passed']&=not p.visible and not p.focused and all(c.returncode==0 for c in p.processes.values()) and sha(a.card)==before
   (p.folder/'result.json').write_text(json.dumps(row,indent=2));results.append(row);(a.output/'result.json').write_text(json.dumps({'passed':all(x['passed'] for x in results),'exe_sha256':sha(a.exe),'card_unchanged':sha(a.card)==before,'bosses':results},indent=2));print('Boss completed '+level+' '+str(row['passed']),flush=True)
 return 0 if all(x['passed'] for x in results) else 1
if __name__=='__main__':raise SystemExit(main())
