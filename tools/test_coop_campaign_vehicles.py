"""Private native vehicle lifecycle/driver regression; native entry is a shortcut."""
from pathlib import Path
from test_coop_campaign import Peers,sha
import argparse,json,traceback,re

def main():
 ap=argparse.ArgumentParser()
 for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
 ap.add_argument('--port',type=int,default=27828);a=ap.parse_args()
 for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
 a.output.mkdir(exist_ok=False);before=sha(a.card);rows=[]
 for level in ('Clevel5_1','Alevel10_4','Clevel6_1'):
  p=Peers(a,level,a.output/level);row={'level':level,'passed':False,'phases':[]};prior=''
  def command(action):
   count=p.log('client').count('[campaign-verify]');p.command(action);seq=p.command_sequence
   p.wait(lambda:'sequence='+str(seq)+' action=' in p.log('host'),15)
   p.wait(lambda:p.log('client').count('[campaign-verify]')>=count+3,20);row['phases'].append(action);p.pulse('SHOT')
  try:
   p.start();s=p.stage(level);vehicle=next(o for o in s['objects'] if o['vtable']==0x80209BB8);ref=str(vehicle['index'])
   command('event '+ref+' 11')
   command('drive '+ref+' 0');command('drive '+ref+' -1')
   command('event '+ref+' 12');command('event '+ref+' 11')
   prior=p.log('client');p.restart_client();p.stage(level);row['late_join_ready']=True
   command('drive '+ref+' 1');p.pulse('SUP',('client',),seconds=2);command('drive '+ref+' -1')
   p.close();logs=prior+'\n'+p.log('client');checks=[x for x in logs.splitlines() if '[campaign-verify]' in x]
   row['samples']=checks;row['errors']=[x for x in logs.splitlines() if '[campaign-field-difference]' in x or '[port] Stopped:' in x]
   row['passed']=bool(checks) and all('differences=0' in x for x in checks) and not row['errors']
  except Exception:row['error']=traceback.format_exc();print(row['error'],flush=True)
  finally:
   p.close();row['exits']={r:c.returncode for r,c in p.processes.items()};row['visible']=p.visible;row['focused']=p.focused
   row['passed']&=all(c.returncode==0 for c in p.processes.values()) and not p.visible and not p.focused and before==sha(a.card)
   rows.append(row);(p.folder/'result.json').write_text(json.dumps(row,indent=2));(a.output/'result.json').write_text(json.dumps({'passed':all(r['passed'] for r in rows),'exe_sha256':sha(a.exe),'card_unchanged':before==sha(a.card),'vehicles':rows},indent=2));print(level+' '+str(row['passed']),flush=True)
 return 0 if all(r['passed'] for r in rows) else 1
if __name__=='__main__':raise SystemExit(main())
