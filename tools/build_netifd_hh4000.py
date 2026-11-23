#!/usr/bin/env python3
"""Build the native HH4000 bridge compatibility daemon using the pinned SDK.
SDK: OpenWrt 24.10.4 armsr/armv8, SHA256
321f4d4d61e02e54795fcb0ca25243955fc425ac7a71b0d86a83c7b16c5fc4ac.
Package sources: official OpenWrt v24.10.4. netifd: 7901e66c.
"""
from pathlib import Path
import subprocess,shutil,hashlib,json
P=Path(__file__).resolve().parents[1];O=P/'work/lan-fix';S=O/'sdk'
patch=P/'src/openwrt-hh4000/patches/100-netifd-hh4000-bridge-ioctl.patch'
d=S/'base-source/package/network/config/netifd/patches';d.mkdir(exist_ok=True)
shutil.copy2(patch,d/'999-hh4000-bridge-ioctl.patch')
for target in ['clean','compile']:
 with (O/('sdk-'+target+'.log')).open('w') as log:
  subprocess.run(['make','package/netifd/'+target,'FORCE=1','V=s','-j4'],cwd=S,stdout=log,stderr=subprocess.STDOUT,check=True)
b=next((S/'build_dir/target-aarch64_generic_musl').glob('netifd-*/.pkgdir/netifd/sbin/netifd'))
assert b'/sys/class/net/%s/bridge/%s' in b.read_bytes(), 'Compatibility code missing'
f=P/'src/openwrt-hh4000/files/sbin/netifd';shutil.copy2(b,f)
subprocess.run([str(S/'staging_dir/toolchain-aarch64_generic_gcc-13.3.0_musl/bin/aarch64-openwrt-linux-strip'),'--strip-unneeded',str(f)],check=True)
m={'netifd_revision':'7901e66c','binary_sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'patch_sha256':hashlib.sha256(patch.read_bytes()).hexdigest(),'bytes':f.stat().st_size}
(O/'build-manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(json.dumps(m))
