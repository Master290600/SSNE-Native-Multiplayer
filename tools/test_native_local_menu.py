"""Verify local multiplayer's real native focus tree and return paths."""
from test_native_frontend import Menus,sha
from pathlib import Path
import argparse,json,traceback

def main():
    ap=argparse.ArgumentParser()
    for key in ('exe','data','card','output'):ap.add_argument('--'+key,type=Path,required=True)
    ap.add_argument('--languages',default='1')
    ap.add_argument('--port',type=int,default=27840)
    a=ap.parse_args()
    for key in ('exe','data','card','output'):setattr(a,key,getattr(a,key).resolve())
    a.output.mkdir(parents=True,exist_ok=False);before=sha(a.card)
    r={'passed':False,'exe_sha256':sha(a.exe),'steps':[],'cases':[]}
    m=None
    try:
        for language in map(int,a.languages.split(',')):
            folder=a.output/str(language);m=Menus(a,'',folder);m.start_role('host',language)
            for repeat in range(5):
                m.network('host');m.choose('host',2);m.page('host',0);m.delay(1)
                state=m.state().copy();r['cases'].append(state)
                assert state['native_root']==0xa7 and state['native_focus'] in range(0xa9,0xad),state
                assert not state['active'] and not state['visible'],state
                if repeat:
                    for _ in range(4):
                        if m.state()['native_focus']==0xa9:break
                        m.press('host','UP')
                    assert m.state()['native_focus']==0xa9,m.state()
                    for _ in range(repeat-1):m.press('host','DOWN')
                    m.press('host','A');m.delay(1)
                    assert m.state()['native_root']!=0xa7 and not m.state()['active'],m.state()
                # Cancel the original local setup screens back to main.
                for _ in range(6):
                    if m.state().get('native_root')==0x3d:break
                    m.press('host','B');m.delay(.4)
                assert m.state()['native_root']==0x3d,m.state()
                r['steps'].append(f'Language {language}, cycle {repeat}: local modes and native back to main')
            m.network('host');m.choose('host',0);m.page('host',2)
            m.choose('host',0);m.wait(lambda:m.state().get('active'))
            m.wait(lambda:m.state().get('native_root')==0xa7)
            m.press('host','B');m.wait(lambda:not m.state().get('active'))
            m.wait(lambda:m.state().get('native_root')==0x3d and not m.state().get('returning'))
            m.network('host');m.choose('host',3)
            m.wait(lambda:m.state().get('native_root')==0x3d)
            r['steps'].append(f'Language {language}: host and custom menu still reachable after local play')
            m.close();assert all(p.returncode==0 for p in m.processes.values())
            assert not m.visible and not m.focused
        r['passed']=True
    except Exception:r['error']=traceback.format_exc()
    finally:
        if m:
            if any(p.poll() is None for p in m.processes.values()):m.press('host','UISHOT')
            r['last_state']=m.state().copy();m.close();r.update(visible=m.visible,focused=m.focused,exits={role:p.returncode for role,p in m.processes.items()})
        r['original_card_unchanged']=sha(a.card)==before;r['passed'] &= r['original_card_unchanged']
        (a.output/'result.json').write_text(json.dumps(r,indent=2),encoding='utf8');print(json.dumps(r,indent=2),flush=True)
    return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
