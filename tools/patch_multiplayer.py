"""Local PAL game translation patches; never distribute translated game bodies."""
from pathlib import Path
import re,json,hashlib,difflib,argparse
parser=argparse.ArgumentParser()
parser.add_argument('--input',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
original=args.input.resolve();out=args.output.resolve()
if original==out:raise SystemExit('Use separate input and output directories')
out.mkdir(parents=True,exist_ok=True)
changes=[];portable=[]
for file in original.iterdir():
    if file.suffix not in ('.cpp','.h'):continue
    text=file.read_text()
    if file.suffix=='.h':(out/file.name).write_text(text);continue
    parts=re.split(r'(?=\nvoid f_)',text);updated=[]
    for body in parts:
        match=re.match(r'\nvoid f_([0-9A-F]{8})',body)
        if not match or '{' not in body:updated.append(body);continue
        address=int(match[1],16);before=body;prefix=[]
        if ' * 1804);' in body:
            body=re.sub(r'uint32_t\(int32_t\((ctx.r\[\d+\])\) \* 1804\)',r'ssne::native_party_controller_offset(\1)',body)
        body=re.sub(r'ctx.r\[\d+\] \+ 0xFFFF9048u',r'ssne::native_party_registry()',body)
        body=re.sub(r'ctx.r\[\d+\] \+ 0xFFFF906Cu',r'ssne::native_party_ranking()',body)
        if address in (0x801173EC,0x801179BC):
            # Only embedded viewport subfields; other engine fields stay in place.
            engine=30 if address==0x801173EC else 31
            body=re.sub(rf'ctx.r\[{engine}\] \+ 0x(DC|E0|E4|E8|EC|F0|F4|F8|FC|100|104|108|10C|110|114|118|11C)u',
                        lambda m:f'ssne::native_party_view_engine(ctx.r[{engine}]) + 0x{m[1]}u',body)
            if address==0x801173EC:
                body=body.replace('a_ < 15, a_ > 15, a_ == 15','a_ < int(ssne::native_party_view_last()), a_ > int(ssne::native_party_view_last()), a_ == int(ssne::native_party_view_last())')
            else:
                body=body.replace('ctx.r[30] = ctx.r[31] + ctx.r[30];','ctx.r[30] = ssne::native_party_view_engine(ctx.r[31]) + ctx.r[30];')
        if address==0x801555F0:
            body=body.replace('ctx.r[25] = mem_read32(ctx.r[9] + 0xFFFFBAD0u);','ctx.r[25] = ssne::native_party_frame_view_mask(mem_read32(ctx.r[9] + 0xFFFFBAD0u));')
            # Both the audio listener search and the camera update pass must
            # visit all relocated physical view indices, not the original 16.
            body=body.replace('a_ < 15, a_ > 15, a_ == 15','a_ < int(ssne::native_party_view_last()), a_ > int(ssne::native_party_view_last()), a_ == int(ssne::native_party_view_last())')
        if address==0x8011A2E8:
            prefix+=['if(ssne::native_party_extended() && (ctx.lr==0x80155A14u || ctx.lr==0x80155B20u)){ctx.r[3]=ssne::native_party_frame_camera(ctx.r[3],ctx.r[4]);return;}']
        if address in (0x800B12BC,0x800B214C,0x800B73BC):
            # The campaign loadout table has four 44-byte records. Never let
            # slots 5-8 read or overwrite its neighbouring native globals.
            body=re.sub(r'ctx.r\[\d+\] \+ 0x6918u',r'ssne::native_party_loadouts()',body)
        if address==0x800B73BC:
            prefix+=['if(!mem_read32(ctx.r[13]-0x7914u))ssne::native_party_reset_extra_loadouts();']
            body=body.replace('a_ < 3, a_ > 3, a_ == 3','a_ < int(ssne::native_party_capacity()-1), a_ > int(ssne::native_party_capacity()-1), a_ == int(ssne::native_party_capacity()-1)')
        if address in range(0x8011A1F8,0x8011A301):
            prefix+=['ctx.r[3]=ssne::native_party_view_engine(ctx.r[3]);','ctx.r[4]=ssne::native_party_view_slot(ctx.r[4]);']
        if address==0x80166D38:
            prefix+=['ssne::native_party_begin(ctx);']
            body=body.replace('0xFFFFFFB8u','0xFFFFFFA8u')
            # Expand only this factory's local frame: eight layout IDs plus saved registers.
            for a,b in [('18','28'),('40','50'),('4C','5C'),('48','58')]:
                body=body.replace(f'ctx.r[1] + 0x{a}u',f'ctx.r[1] + 0x{b}u')
            body=body.replace('a_ < 3, a_ > 3, a_ == 3); }\n    if (!cr_bit(ctx, 1)) {\n    SSNE_POLL();\n    goto L_80166F04;',
                              'a_ < int(ssne::native_party_capacity()-1), a_ > int(ssne::native_party_capacity()-1), a_ == int(ssne::native_party_capacity()-1)); }\n    if (!cr_bit(ctx, 1)) {\n    SSNE_POLL();\n    goto L_80166F04;')
            body=body.replace('ctx.r[26] = 0u + 0x4u;\nL_80167180:',
                              'ctx.r[26] = ssne::native_party_capacity();\n    if(ssne::native_party_extended())for(unsigned i=0;i<8;++i)mem_write32(ctx.r[27]+i*4,0);\nL_80167180:')
            body=body.replace('ctx.r[30] = mem_read32(ctx.r[9] + 0x180u);','ctx.r[30] = mem_read32(ctx.r[9] + 0x180u);\n    ssne::native_party_owner_set(int(ctx.r[30]));')
            body=body.replace('    f_800B11FC(ctx);','    f_800B11FC(ctx);\n    ssne::native_party_trace("constructed",ctx.r[26],ctx.r[3]);')
            anchor='    if (true) { uint32_t t_ = ctx.lr; ctx.lr = 0x80166EF0; ssne_call(ctx, t_); }'
            assert body.count(anchor)==1
            body=body.replace(anchor,anchor+'\n    ssne::native_party_initialize_player_masks(ctx,ctx.r[31]);')
            body=body.replace('    f_801505F8(ctx);','    ssne::native_party_trace("initialize",ctx.r[30],ctx.r[4]);\n    f_801505F8(ctx);\n    ssne::native_party_trace("initialized",ctx.r[30],0);')
            body=body.replace('    ctx.r[0] = mem_read32(ctx.r[1] + 0x5Cu);','    ssne::netplay_register_party_players();\n    ctx.r[0] = mem_read32(ctx.r[1] + 0x5Cu);')
        if address==0x800C279C:
            prefix+=['if(!ssne::native_party_joined(ctx.r[3])){ctx.r[3]=0;return;}']
            prefix+=['if(ssne::native_party_extended() && ctx.r[3]>=4 && ctx.r[3]<ssne::native_party_capacity()){ctx.r[3]=1;return;}']
        if address==0x800C261C:
            prefix+=['if(ssne::native_party_extended() && ctx.r[3]>=4 && ctx.r[3]<ssne::native_party_capacity())return;']
        if address==0x800C22F0:
            prefix+=['if(ssne::native_party_extended() && ctx.r[3]>=4){ctx.r[3]=ssne::native_party_profile(ctx.r[3]);ssne_call(ctx,0x800C23A8);return;}']
        if address==0x800C25A4:
            prefix+=['if(ssne::native_party_extended() && ctx.r[3]>=4){ctx.r[3]=ssne::native_party_profile(ctx.r[3]);return;}']
        if address==0x80166480:
            body=body.replace('    f_80163E6C(ctx);','    ssne::native_party_end(ctx);\n    f_80163E6C(ctx);')
        if address in (0x801502AC,0x801503C4):
            prefix+=['ssne::native_party_controller_channel(ctx);']
        if 'ctx.r[3] = 0u + 0x81780000u;' in body:
            body=body.replace('ctx.r[3] = 0u + 0x81780000u;','ctx.r[3] = ssne::guest_ram_end()-0x80000u;')
        if address==0x8010B0AC:
            prefix+=['ssne::NativePartySpawnScope party_spawn_scope(ctx);']
            body=body.replace('    { int32_t a_ = int32_t(ctx.r[24]); cr_set_cmp(ctx, 0, a_ < 0, a_ > 0, a_ == 0); }',
                              '    if(!party_spawn_scope.prepare())goto L_8010B4F4;\n    { int32_t a_ = int32_t(ctx.r[24]); cr_set_cmp(ctx, 0, a_ < 0, a_ > 0, a_ == 0); }')
            body=body.replace('L_8010B4F4:\n', 'L_8010B4F4:\n    party_spawn_scope.prepare();\n')
        if address==0x800F3BB0:
            # Credits glyph models can omit transform channel 3. The original
            # model getter explicitly returns null for an absent channel.
            # Do not copy a 3x4 matrix into low memory in that case.
            body=body.replace('    f_801B9E78_PSMTXCopy(ctx);',
                              '    if(ctx.r[4])f_801B9E78_PSMTXCopy(ctx);')
        if address==0x801089E0:
            body=body.replace('    ctx.lr = 0x80108E00;\n    f_801B60D0_OSReport(ctx);',
                              '    ctx.lr = 0x80108E00;\n    f_801B60D0_OSReport(ctx);\n    ssne::capture_native_results(ctx.r[25],ctx.r[24]);')
        if address==0x800B2E5C:
            body=body.replace('    f_80080204(ctx);','    if(!ssne::replica_simulation())f_80080204(ctx);')

        if address in (0x800C7658,0x800C7F78):
            body=body.replace('ctx.r[31] + 0x170u','ssne::native_party_pickup_flags(ctx.r[31])')
        if address in (0x800BCD08,0x800C2B88):
            # Native script/UI objects have only four embedded variables/widgets.
            body=body.replace('mem_read32(ctx.r[13] + 0xFFFF9630u)','std::min(mem_read32(ctx.r[13] + 0xFFFF9630u),4u)')
        if address in (0x80107C00,0x80166724):
            body=body.replace('ctx.r[31] + 0xCu','ctx.r[31] + (ssne::native_party_capacity()-1)*4')
        if address==0x80166724:
            body=body.replace('ctx.r[0] = 0u + 0x4u;\n    ctx.r[9] = 0u + 0x802E0000u;',
                              'ctx.r[0] = ssne::native_party_capacity();\n    ctx.r[9] = 0u + 0x802E0000u;')
            body=body.replace('a_ < 3, a_ > 3, a_ == 3); }\n    if (cr_bit(ctx, 1)) {\n    goto L_801668FC;',
                              'a_ < int(ssne::native_party_capacity()-1), a_ > int(ssne::native_party_capacity()-1), a_ == int(ssne::native_party_capacity()-1)); }\n    if (cr_bit(ctx, 1)) {\n    goto L_801668FC;')
        needs_owner=(' * 1804);' in before or '0x178u' in before or '0x17Cu' in before or 0x800B7000<=address<0x800BC000 or address==0x80166D38)
        if needs_owner and address<0x80110000 or address==0x80166D38:
            prefix.insert(0,'ssne::NativePartyOwnerScope party_owner_scope(ctx);')
        if prefix:
            anchor=f'    SSNE_HOOK(0x{address:08X});'
            if anchor in body:body=body.replace(anchor,anchor+'\n    '+'\n    '.join(prefix),1)
            else:
                opening=body.index('{')+1
                body=body[:opening]+'\n    '+'\n    '.join(prefix)+'\n'+body[opening:]
        if before!=body:
            changes.append({'file':file.name,'address':f'{address:08X}','before':hashlib.sha256(before.encode()).hexdigest()})
            assert before.isascii() and body.isascii()
            a=before.splitlines(keepends=True);b=body.splitlines(keepends=True);edits=[]
            for op,i,j,k,l in difflib.SequenceMatcher(None,a,b,autojunk=False).get_opcodes():
                if op!='equal':edits.append({'start':len(''.join(a[:i])),'length':len(''.join(a[i:j])),'text':''.join(b[k:l])})
            portable.append({**changes[-1],'after':hashlib.sha256(body.encode()).hexdigest(),'edits':edits})
        updated.append(body)
    result=''.join(updated)
    if result!=text:result='#include "ssne/native_party.h"\n#include "ssne/replica_simulation.h"\n#include "ssne/native_results.h"\n#include <algorithm>\n'+result
    destination=out/file.name
    if not destination.exists() or destination.read_text()!=result:destination.write_text(result)
(out/'patch-manifest.json').write_text(json.dumps(changes,indent=2))
(out/'patch-edits.json').write_text(json.dumps(portable,indent=2))
print('Patched local game functions:',len(changes),'units:',','.join(sorted({c['file'] for c in changes})))
