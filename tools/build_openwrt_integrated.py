#!/usr/bin/env python3
"""Build an HH4000 RAM initrd with automatic vendor network integration.

Base: previously tested OpenWrt network rootfs. No router access or flash writes.
--lab enables the private work/openwrt-integrate/wireless.lab test configuration.
Without --lab all radios default to disabled until WPA2 credentials are supplied.
"""
from pathlib import Path
from local_build_inputs import require_overlay_binaries
import argparse,shutil,subprocess,gzip,json,hashlib,os
P=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--lab',action='store_true');args=parser.parse_args()
O=P/'work/openwrt-integrate';R=O/'rootfs';O.mkdir(parents=True,exist_ok=True)
seed=R if R.exists() else P/'work/openwrt-net/rootfs'
if not seed.is_dir():
 raise SystemExit('Run the network rootfs builder before assembling the integrated image.')
require_overlay_binaries(P/'src/openwrt-hh4000',seed)
if seed!=R:shutil.copytree(seed,R,symlinks=True)
shutil.copytree(P/'src/openwrt-hh4000/files',R,dirs_exist_ok=True,symlinks=True)
if args.lab:shutil.copy2(O/'wireless.lab',R/'etc/config/wireless')
(R/'etc/config/wireless').chmod(0o600)
base=(P/'work/openwrt-net/rootfs/init').read_text()
base=base.replace("while :; do\n    echo 'OpenWrt RAM shell; networking and writable NAND mounts not initialized.'", "/usr/sbin/hh-network-start >/tmp/hh-network-start.log 2>&1 &\necho HH_NETWORK_START_PID=$!\nwhile :; do\n    echo 'OpenWrt RAM shell; automatic network setup is in progress; stock flash is not mounted.'")
assert 'HH_NETWORK_START_PID' in base
(R/'init').write_text(base);(R/'init').chmod(0o755)
for f in (P/'src/openwrt-hh4000/files').rglob('*'):
 if f.is_file() and (f.suffix=='.sh' or os.access(f,os.X_OK)):
  if f.read_bytes().startswith(b'#!'):subprocess.run(['sh','-n',str(f)],check=True)
paths=['.']+sorted('./'+str(f.relative_to(R)) for f in R.rglob('*'))
cpio=subprocess.run(['cpio','--null','-o','--format=newc','--owner=0:0'],cwd=R,input=('\0'.join(paths)+'\0').encode(),capture_output=True,check=True).stdout
(O/'openwrt-integrated.cpio.gz').write_bytes(gzip.compress(cpio,mtime=0))
subprocess.run(['mkimage','-A','arm64','-O','linux','-T','ramdisk','-C','gzip','-n','HH4000 integrated RAM network','-d',str(O/'openwrt-integrated.cpio.gz'),str(O/'openwrt-integrated.uImage')],check=True)
f=O/'openwrt-integrated.uImage';size=f.stat().st_size
assert size < 0x1000000, 'Initrd exceeds its existing 16 MiB staging slot'
manifest={'lab':args.lab,'flash_written':False,'files':{f.name:{'bytes':size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()}},'netifd_helper_source':'https://github.com/openwrt/netifd/tree/7901e66c'}
(O/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
