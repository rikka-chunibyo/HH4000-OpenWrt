#!/usr/bin/env python3
"""Build the pinned OpenWrt service manager with HH4000 shutdown support."""
from pathlib import Path
import subprocess,shutil,hashlib,json
P=Path(__file__).resolve().parents[1];O=P/'work/procd-fix';O.mkdir(exist_ok=True)
S=P/'work/lan-fix/sdk'
patch=P/'src/openwrt-hh4000/patches/110-procd-hh4000-nonpid1-shutdown.patch'
d=S/'base-source/package/system/procd/patches';d.mkdir(exist_ok=True)
shutil.copy2(patch,d/'999-hh4000-nonpid1-shutdown.patch')
shutil.copy2(P/'src/openwrt-hh4000/patches/121-procd-hotplug-path-filter.patch',d/'998-hotplug-path-filter.patch')
link=S/'package/feeds/base/procd'
if not link.exists():link.symlink_to(S/'base-source/package/system/procd',target_is_directory=True)
for target in ['clean','compile']:
 with (O/('sdk-'+target+'.log')).open('w') as log:
  subprocess.run(['make','package/procd/'+target,'FORCE=1','CONFIG_PACKAGE_procd-selinux=','V=s','-j4'],cwd=S,stdout=log,stderr=subprocess.STDOUT,check=True)
b=next((S/'build_dir/target-aarch64_generic_musl').glob('procd-default/procd-*/.pkgdir/procd/sbin/procd'))
f=P/'src/openwrt-hh4000/files/sbin/procd';shutil.copy2(b,f)
subprocess.run([str(S/'staging_dir/toolchain-aarch64_generic_gcc-13.3.0_musl/bin/aarch64-openwrt-linux-strip'),'--strip-unneeded',str(f)],check=True)
m={'revision':'42d3937654508b04da64969f9d764ac2ec411904','binary_sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'binary_sha1':hashlib.sha1(f.read_bytes()).hexdigest(),'patch_sha256':hashlib.sha256(patch.read_bytes()).hexdigest(),'security_patch_sha256':hashlib.sha256((P/'src/openwrt-hh4000/patches/121-procd-hotplug-path-filter.patch').read_bytes()).hexdigest(),'bytes':f.stat().st_size}
(O/'build-manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(json.dumps(m))
