#!/usr/bin/env python3
"""Probe stock libScreenApi ARM instructions with mocked SDL calls; never touches hardware."""
from pathlib import Path
import sys,struct,json
from io import BytesIO
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'work/boot/emulation-deps'))
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
base=0x10000000;u=Uc(UC_ARCH_ARM,UC_MODE_ARM)
for address,size in [(base,0x40000),(0x3000000,0x10000),(0x4000000,0x10000),(0x5000000,0x10000),(0x6000000,0x10000),(0x7000000,0x10000),(0x8000000,0x1000)]:u.mem_map(address,size)
elf=ELFFile(BytesIO((root/'work/rootfs/usr/lib/libScreenApi.so.1.0.0').read_bytes()))
for seg in elf.iter_segments():
 if seg['p_type']=='PT_LOAD':u.mem_write(base+seg['p_vaddr'],seg.data())
external={};export={sym.name:base+sym['st_value'] for sym in elf.get_section_by_name('.dynsym').iter_symbols() if sym['st_shndx']!='SHN_UNDEF'}
def resolve(sym):
 if sym['st_shndx']!='SHN_UNDEF':return sym['st_value']+(0 if sym['st_shndx']=='SHN_ABS' else base)
 if sym.name not in external:external[sym.name]=0x7000100+4*len(external)
 return external[sym.name]
for section in elf.iter_sections():
 if not isinstance(section,RelocationSection):continue
 symbols=elf.get_section(section['sh_link'])
 for rel in section.iter_relocations():
  target=base+rel['r_offset'];addend=struct.unpack('<I',u.mem_read(target,4))[0];kind=rel['r_info_type']
  if kind==23:value=base+addend
  elif kind in (2,21,22):value=resolve(symbols.get_symbol(rel['r_info_sym']))+(addend if kind==2 else 0)
  elif kind==0:continue
  else:raise RuntimeError('Unhandled ARM relocation '+str(kind))
  u.mem_write(target,struct.pack('<I',value&0xffffffff))
reverse={address:name for name,address in external.items()};events=[]
registers=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3]
def cstring(pointer):
 value=bytes(u.mem_read(pointer,256));return value.split(b'\0',1)[0].decode()
def hook(u,pc,size,data):
 if pc==0x8000000:u.emu_stop();return
 if pc not in reverse:return
 name=reverse[pc];args=[u.reg_read(reg) for reg in registers];ret=0
 if name=='SDL_MapRGB':events.append([name,args]);ret=args[1]
 elif name=='SDL_FillRect':events.append([name,args[0],list(struct.unpack('<hhHH',u.mem_read(args[1],8))) if args[1] else None,args[2]])
 elif name=='SDL_UpperBlit':events.append([name,args[0],args[2],list(struct.unpack('<hhHH',u.mem_read(args[3],8)))])
 elif name=='SDL_FreeSurface':events.append([name,args[0]])
 elif name=='IMG_Load':events.append([name,cstring(args[0])]);ret=0x5000200
 elif name=='SDL_ConvertSurface':events.append([name,args[0],args[1],args[2]]);ret=0x5000600
 elif name=='TTF_RenderUTF8_Solid':events.append([name,args[0],cstring(args[1]),args[2]]);ret=0x5000300
 elif name in ['SDL_Init','TTF_Init','SDL_ShowCursor']:events.append([name,args[0]])
 elif name=='IMG_Init':events.append([name,args[0]]);ret=2
 elif name=='SDL_SetVideoMode':events.append([name,args]);ret=0x5000000
 elif name=='SDL_CreateRGBSurface':events.append([name,args]);ret=0x5000100
 elif name=='SDL_SetPalette':events.append([name,args,struct.unpack('<I',u.mem_read(u.reg_read(UC_ARM_REG_SP),4))[0]]);ret=1
 elif name=='syslog':pass
 else:raise RuntimeError('Unexpected external call '+name)
 u.reg_write(UC_ARM_REG_R0,ret);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(symbol,*args):
 for n,value in enumerate(args):u.reg_write(registers[n],value)
 u.reg_write(UC_ARM_REG_SP,0x400f000);u.reg_write(UC_ARM_REG_LR,0x8000000);u.emu_start(export[symbol],0x8000004,count=100000)
 assert u.reg_read(UC_ARM_REG_PC)==0x8000000,hex(u.reg_read(UC_ARM_REG_PC))
 return u.reg_read(UC_ARM_REG_R0)
for surface in [0x5000000,0x5000100,0x5000200,0x5000300,0x5000600]:u.mem_write(surface+4,struct.pack('<I',0x5000400))
painter=bytearray(48);struct.pack_into('<IhhHH',painter,0,0x5000000,3,4,168,42);painter[44:48]=bytes([160,160,160,0]);u.mem_write(0x3000000,bytes(painter));u.mem_write(0x3000100,struct.pack('<hhHH',10,20,30,5));u.mem_write(0x3000200,struct.pack('<hh',9,-2));u.mem_write(0x3000300,b'hello\0');u.mem_write(0x3000400,b'/example/icon.png\0')
results={}
call('_ZN6screen7Painter14drawFilledRectERKNS_4RectE',0x3000000,0x3000100);assert events[-1]==['SDL_FillRect',0x5000000,[13,24,30,5],160];results['filled_rect_origin_and_color']=events[:];events.clear()
call('_ZN6screen7Painter14drawSdlSurfaceEP11SDL_SurfaceRKNS_5PointE',0x3000000,0x5000200,0x3000200);assert events[-1]==['SDL_UpperBlit',0x5000200,0x5000000,[12,2,0,0]];results['surface_blit_origin']=events[:];events.clear()
call('_ZN6screen7Painter9drawImageEPKcRKNS_5PointE',0x3000000,0x3000400,0x3000200);assert events[0]==['IMG_Load','/example/icon.png'];assert any(e[0]=='SDL_ConvertSurface' for e in events);results['image_load_convert_blit_lifecycle']=events[:];events.clear()
call('_ZN6screen7Painter8drawTextEPKcRKNS_5PointE',0x3000000,0x3000300,0x3000200);assert not events;results['text_without_font']='returns without drawing'
u.mem_write(0x3000028,struct.pack('<I',0x5000500));call('_ZN6screen7Painter8drawTextEPKcRKNS_5PointE',0x3000000,0x3000300,0x3000200);assert events[0][0]=='TTF_RenderUTF8_Solid' and events[0][2]=='hello';assert events[-1]==['SDL_FreeSurface',0x5000300];results['text_render_blit_free']=events[:];events.clear()
system=bytearray(0x300);struct.pack_into('<HHI',system,0,167,42,8);struct.pack_into('<I',system,16,0x6000000);u.mem_write(0x3001000,bytes(system));u.mem_write(0x6000000,b''.join(bytes([i,i,i,0]) for i in range(256)))
ret=call('_ZN6screen6System5startEv',0x3001000);assert ret==1;assert ['SDL_SetVideoMode',[167,42,8,0x20000000]] in events;assert bytes(u.mem_read(0x3001020,1))==b'\1';results['system_start_8bit_palette']=events[:]
results['scope']='Original ARM library instructions with mocked SDL, font, image, and logging calls; no live panel test and no image/font decoding validation'
(root/'work/display/probe-results.json').write_text(json.dumps(results,indent=2)+'\n')
print('Passed six off-device API checks: rectangle, blit, image lifecycle, missing-font behavior, text lifecycle, and system initialization')
