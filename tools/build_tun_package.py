#!/usr/bin/env python3
"""Package the tested stock-ABI TUN module; never installs generic OpenWrt kmods."""
from pathlib import Path
import tarfile,io,gzip,json,hashlib
P=Path(__file__).resolve().parents[1];O=P/'work/remaining-validation';SRC=P/'src/openwrt-hh4000/net-extras'
def tar(entries):
 b=io.BytesIO()
 with tarfile.open(fileobj=b,mode='w') as a:
  for name,data,mode in entries:
   t=tarfile.TarInfo('./'+name);t.size=len(data);t.mode=mode;t.uid=t.gid=0;t.mtime=0;a.addfile(t,io.BytesIO(data))
 return gzip.compress(b.getvalue(),mtime=0)
control=b'Package: hh4000-tun\nVersion: 1.0-r1\nArchitecture: aarch64_generic\nDepends: kernel (= 4.1.52~hh4000-1)\nProvides: kmod-tun\nSection: kernel\nMaintainer: HH4000 Project\nDescription: Verified TUN module for the retained HH4000 vendor kernel\n'
post=b'#!/bin/sh\nif [ -z "${IPKG_INSTROOT:-}" ]; then\n /etc/init.d/hh-tun enable || exit 1\n /etc/init.d/hh-tun start || exit 1\nfi\nexit 0\n'
ctl=tar([('control',control,0o644),('postinst',post,0o755)])
data=tar([('lib/modules/4.1.52-5.02L.07/tun.ko',(O/'modules/tun.ko').read_bytes(),0o644),('etc/init.d/hh-tun',(SRC/'hh-tun.init').read_bytes(),0o755)])
f=O/'hh4000-tun_1.0-r1_aarch64_generic.ipk';f.write_bytes(tar([('debian-binary',b'2.0\n',0o644),('control.tar.gz',ctl,0o644),('data.tar.gz',data,0o644)]))
m={'package':f.name,'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size,'provides':['kmod-tun'],'abi':'4.1.52-5.02L.07'};(O/'tun-package-manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(json.dumps(m))
