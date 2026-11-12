"""Read original PAL menu descriptors and language labels for coverage reports.

Descriptor width/height are retained as layout metadata, never as hit boxes.
Runtime draw bounds and native selectability remain the authority for input.
"""
from pathlib import Path
from collections import Counter
import argparse,hashlib,json,struct

GROUP_NAMES={0:'name_keyboard',0x31:'original_cheats',0x3d:'main',0x44:'difficulty',0x4b:'level_carousel',0x59:'cooperative_setup',0x64:'deathmatch_setup',0x76:'flag_setup',0x86:'bomb_setup',0x9a:'players_lobby',0x9c:'no_profiles',0xa0:'player_card',0xa4:'player_card_nested',0xa7:'game_mode',0xaf:'select_player',0xb7:'language',0xbc:'level_results',0xdd:'memory_card_access',0xe3:'custom_information_viewer',0xef:'custom_viewer_nested',0xf0:'game_options',0x100:'character',0x106:'player_settings',0x10a:'misc_settings',0x115:'control_settings',0x123:'profile_settings',0x129:'pause',0x134:'paused_notice',0x138:'score_table'}

class Resource:
 def __init__(self,path):self.path=Path(path);self.data=self.path.read_bytes()
 def check(self,offset,size):
  if offset<0 or offset+size>len(self.data):raise ValueError('Resource offset outside '+str(self.path))
 def u16(self,offset):self.check(offset,2);return struct.unpack_from('>H',self.data,offset)[0]
 def u32(self,offset):self.check(offset,4);return struct.unpack_from('>I',self.data,offset)[0]
 def f32(self,offset):self.check(offset,4);return struct.unpack_from('>f',self.data,offset)[0]
 def sha(self):return hashlib.sha256(self.data).hexdigest()


def original_inventory(data,language=0):
 data=Path(data);folder=data/'files';gui=Resource(folder/'Game.Gui')
 if gui.data[:4]!=b'DXFF':raise ValueError('Expected original DXFF Game.Gui')
 name={0:'EnglishEUR.tdb',1:'German.tdb',2:'french.tdb'}[language];text=Resource(folder/name);count=gui.u32(16)
 if count>4096:raise ValueError('Unexpected original descriptor count')
 def label(index):
  if index>=text.u32(0):return None
  start=text.u32(8+index*12);text.check(start,1);end=text.data.find(b'\0',start)
  if end<0:raise ValueError('Unterminated original language text')
  return text.data[start:end].decode('latin1')
 widgets=[]
 for ident in range(count):
  offset=16+gui.u32(20+ident*4);gui.check(offset,76)
  kind=gui.u16(offset);parent=gui.u16(offset+4);children=gui.u16(offset+6);start=16+gui.u32(offset+0x44)
  child_ids=[gui.u32(start+i*4) for i in range(children)] if children else []
  if any(value>=count for value in child_ids):raise ValueError('Invalid original child descriptor')
  entry={'id':ident,'id_hex':hex(ident),'type':kind,'parent':None if parent==0xffff else parent,'children':child_ids,'flags':gui.u32(offset+12),'anchor':gui.u32(offset+0x28),'layout_xy':[gui.f32(offset+0x2c),gui.f32(offset+0x30)],'layout_canvas_wh':[gui.f32(offset+0x34),gui.f32(offset+0x38)]}
  if kind==1:
   text_id=gui.u32(offset+0x4c);entry.update(text_id=text_id,label=label(text_id))
  widgets.append(entry)
 groups=[]
 for widget in widgets:
  if widget['type']!=0:continue
  groups.append({**widget,'name':GROUP_NAMES.get(widget['id'],'group_'+widget['id_hex']),'child_labels':[{'id':ident,'type':widgets[ident]['type'],'label':widgets[ident].get('label')} for ident in widget['children']]})
 return {'descriptor_count':count,'group_count':len(groups),'type_counts':dict(sorted(Counter(w['type'] for w in widgets).items())),'source_files':{'Game.Gui':gui.sha(),name:text.sha()},'language':language,'groups':groups,'widgets':widgets,'scope':'Original descriptor inventory only. Layout canvas dimensions are not pointer hit bounds. Runtime visibility, authority and draw geometry decide interactive coverage.'}


def main():
 ap=argparse.ArgumentParser();ap.add_argument('--data',type=Path,required=True);ap.add_argument('--language',type=int,choices=(0,1,2),default=0);ap.add_argument('--output',type=Path);a=ap.parse_args();value=original_inventory(a.data,a.language)
 if a.output:a.output.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf8')
 print(json.dumps({k:value[k] for k in ('descriptor_count','group_count','type_counts','source_files')},indent=2));return 0
if __name__=='__main__':raise SystemExit(main())
