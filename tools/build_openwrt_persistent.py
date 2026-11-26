#!/usr/bin/env python3
"""Build a SquashFS OpenWrt system, flash bootstrap, and isolated RAM test initrd."""
from pathlib import Path
from local_build_inputs import require_overlay_binaries
from build_skiplog import build as build_skiplog
import shutil,subprocess,gzip,json,hashlib,argparse
P=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--output', default='work/openwrt-install')
parser.add_argument('--wireless-profile', type=Path, help='Private Wi-Fi preset to include explicitly')
args=parser.parse_args()
O=P/args.output;O.mkdir(parents=True,exist_ok=True)
R=O/'rootfs';B=O/'bootstrap';S=P/'src/openwrt-hh4000/persistent'
seed=R
if not R.exists():
 seed=P/'work/openwrt-install/rootfs'
 if seed==R or not seed.exists():seed=P/'work/openwrt-integrate/rootfs'
if not seed.is_dir():
 raise SystemExit('Prepare a local OpenWrt rootfs under work/openwrt-integrate/rootfs first.')
# Rebuild the board daemons before overlaying the image, even on an older seed.
subprocess.run(['python3', str(P/'tools/build_frontpanel.py')], check=True)
require_overlay_binaries(P/'src/openwrt-hh4000',seed)
if seed!=R:shutil.copytree(seed,R,symlinks=True)
# A native fw3 replacement must replace the old fw3->fw4 alias itself.
if (P/'src/openwrt-hh4000/files/sbin/fw3').exists():
 for name in ['fw3','fw4']:
  f=R/'sbin'/name
  if f.exists() or f.is_symlink():f.unlink()
overlay=P/'src/openwrt-hh4000/files'
for source in overlay.rglob('*'):
 destination=R/source.relative_to(overlay)
 if source.is_symlink() and (destination.exists() or destination.is_symlink()):destination.unlink()
 elif source.is_file() and destination.is_symlink():destination.unlink()
 if source.is_file() and not source.is_symlink():
  head=source.read_bytes()[:64]
  if head.startswith(b'#!/bin/sh') or head.startswith(b'#!/rescue/busybox sh'):
   subprocess.run(['sh','-n',str(source)],check=True)
shutil.copytree(overlay,R,dirs_exist_ok=True,symlinks=True)
build_skiplog(R)
if args.wireless_profile:
 shutil.copy2(args.wireless_profile,R/'etc/config/wireless')
(R/'etc/config/wireless').chmod(0o600)
wireless=(R/'etc/config/wireless').read_text();parts=wireless.split('config wifi-device')
for i in range(1,len(parts)):
 if parts[i].lstrip().startswith(("'radio1'", "'radio2'", 'radio1', 'radio2')):
  import re
  parts[i]=re.sub(r"option htmode ['\"]?HT20['\"]?", "option htmode 'VHT80'",parts[i])
  if 'option htmode' not in parts[i]:
   header,rest=parts[i].split('\n',1)
   parts[i]=header+"\n option htmode 'VHT80'\n"+rest

(R/'etc/config/wireless').write_text('config wifi-device'.join(parts))

shutil.copy2(S/'system-init',R/'init')
if (P/'src/openwrt-hh4000/files/sbin/fw3').exists():
 subprocess.run(['python3',str(P/'tools/prepare_firewall_metadata.py'),str(R)],check=True)
 subprocess.run(['python3',str(P/'tools/prepare_security_metadata.py'),str(R)],check=True)
for identity in list((R/'etc/dropbear').glob('*_host_key'))+[R/'etc/uhttpd.crt',R/'etc/uhttpd.key']:
 if identity.exists() or identity.is_symlink():identity.unlink()
(R/'etc/hh-factory-reset-capable').write_text('HH4000_FACTORY_RESET_V1\n')
for f in [S/'bootstrap-init',S/'system-init']:subprocess.run(['sh','-n',str(f)],check=True)
subprocess.run(['mksquashfs',str(R),str(O/'root.squashfs'),'-noappend','-all-root','-no-xattrs','-comp','xz','-b','131072','-processors','2','-no-progress'],check=True)
for name in ['rescue','lib','dev','proc','sys','tmp','rom','overlay','newroot']:(B/name).mkdir(parents=True,exist_ok=True)
shutil.copy2(S/'reset-data.sh',B/'rescue/reset-data.sh');shutil.copy2(S/'bootstrap-init',B/'init');shutil.copy2(R/'rescue/busybox',B/'rescue/busybox');shutil.copy2(R/'rescue/hard_watchdog.ko',B/'rescue/hard_watchdog.ko')
for name in ['ld-linux.so.3','libcrypt.so.1','libm.so.6','libresolv.so.2','libc.so.6']:shutil.copy2(R/'lib'/name,B/'lib'/name)
cfg=json.loads((P/'work/display/live/build-settings.json').read_text())
subprocess.run([cfg['compiler'],*cfg['base_arguments'],'-std=gnu11','-Wall','-Wextra','-Werror',str(S/'loop-root.c'),*cfg['tail_arguments'],'-o',str(B/'rescue/loop-root')],check=True)
def archive(name,embedded):
 root=B/'root.squashfs'
 if embedded:shutil.copy2(O/'root.squashfs',root)
 elif root.exists():root.unlink()
 paths=['.']+sorted('./'+str(x.relative_to(B)) for x in B.rglob('*'))
 data=subprocess.run(['cpio','--null','-o','--format=newc','--owner=0:0'],cwd=B,input=('\0'.join(paths)+'\0').encode(),capture_output=True,check=True).stdout
 path=O/(name+'.cpio.gz');path.write_bytes(gzip.compress(data,mtime=0))
 subprocess.run(['mkimage','-A','arm64','-O','linux','-T','ramdisk','-C','gzip','-n','HH4000 '+name,'-d',str(path),str(O/(name+'.uImage'))],check=True)
archive('bootstrap',False);archive('ramtest',True)
assert (O/'ramtest.uImage').stat().st_size<0x1000000
m={'files':{n:{'bytes':(O/n).stat().st_size,'sha256':hashlib.sha256((O/n).read_bytes()).hexdigest(),'sha1':hashlib.sha1((O/n).read_bytes()).hexdigest()} for n in ['root.squashfs','bootstrap.uImage','ramtest.uImage']},'writes_router_flash':False}
(O/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(json.dumps(m,indent=2))
