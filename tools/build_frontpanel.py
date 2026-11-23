#!/usr/bin/env python3
"""Build and test HH4000 front-panel service against its backed-up ARM32 libc."""
from pathlib import Path
import subprocess,json,shutil,hashlib,tarfile,ast
P=Path(__file__).resolve().parents[1];O=P/'work/frontpanel-v2';O.mkdir(parents=True,exist_ok=True)
S=P/'src/openwrt-hh4000/frontpanel/frontpanel.c';R=P/'src/openwrt-hh4000/files'
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
subprocess.run(['gcc','-O1','-g','-fsanitize=address,undefined',*flags,'-DDIRPATH="'+str(O/'runtime')+'"',str(S),'-o',str(O/'frontpanel-host')],check=True)
subprocess.run([str(O/'frontpanel-host'),'--self-test'],check=True)
cfg=json.loads((P/'work/display/live/build-settings.json').read_text());target=R/'usr/libexec/hh-frontpanel'
subprocess.run([cfg['compiler'],*cfg['base_arguments'],'-D_TIME_BITS=32','-D_FILE_OFFSET_BITS=32',*flags,str(S),*cfg['tail_arguments'],'-o',str(target)],check=True)
strip=shutil.which('arm-linux-gnueabi-strip');assert strip;subprocess.run([strip,str(target)],check=True);target.chmod(0o755)
reset_source=P/'src/openwrt-hh4000/frontpanel/reset.c';reset_target=R/'usr/libexec/hh-reset'
subprocess.run(['gcc','-O1','-g','-fsanitize=address,undefined',*flags,str(reset_source),'-o',str(O/'reset-host')],check=True)
subprocess.run([str(O/'reset-host'),'--self-test'],check=True)
subprocess.run([cfg['compiler'],*cfg['base_arguments'],'-D_TIME_BITS=32','-D_FILE_OFFSET_BITS=32',*flags,str(reset_source),*cfg['tail_arguments'],'-o',str(reset_target)],check=True)
subprocess.run([strip,str(reset_target)],check=True);reset_target.chmod(0o755)

for f in R.rglob('*'):
 if not f.is_file():continue
 head=f.read_bytes()[:80]
 if head.startswith(b'#!/bin/sh') or head.startswith(b'#!/rescue/busybox sh'):subprocess.run(['sh','-n',str(f)],check=True)
 elif f.suffix=='.py' or head.startswith(b'#!/usr/bin/python3'):ast.parse(f.read_text(),filename=str(f))
node=shutil.which('node')
if not node:
 candidates=sorted((Path.home()/'.nvm/versions/node').glob('*/bin/node'))
 if candidates:node=str(candidates[-1])
if node:
 subprocess.run([node,'--check',str(R/'www/luci-static/resources/view/system/hh_frontpanel.js')],check=True)
 if (P/'tools/test_frontpanel_v2_ui.js').exists():subprocess.run([node,str(P/'tools/test_frontpanel_v2_ui.js')],check=True)
files=[x for x in R.rglob('*') if x.is_file() and '__pycache__' not in x.parts and x.suffix!='.pyc' and ('hh-reset' in x.name or x.name=='hh-factory-reset' or 'frontpanel' in x.name or 'hh_frontpanel' in x.name or x.name=='hh-panel-python' or '/hh-frontpanel/' in x.as_posix())]

with tarfile.open(O/'frontpanel-overlay.tar.gz','w:gz') as t:
 for f in files:t.add(f,arcname=str(f.relative_to(R)),recursive=False)
manifest={str(f.relative_to(R)):{'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in files}
(O/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Built panel overlay:',len(files),'files; daemon',target.stat().st_size,'bytes')
