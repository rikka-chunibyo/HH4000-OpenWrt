#!/usr/bin/env python3
"""Build the optional HH4000 FQ-CoDel service with verified vendor-ABI modules."""
from pathlib import Path
import tarfile,io,gzip,json,hashlib,subprocess
P=Path(__file__).resolve().parents[1];O=P/'work/remaining-validation';S=P/'src/openwrt-hh4000/sqm'
def archive(entries):
 b=io.BytesIO()
 with tarfile.open(fileobj=b,mode='w') as a:
  for name,data,mode,link in entries:
   t=tarfile.TarInfo('./'+name);t.mode=mode;t.uid=t.gid=0;t.mtime=0
   if link:t.type=tarfile.SYMTYPE;t.linkname=link;a.addfile(t)
   else:t.size=len(data);a.addfile(t,io.BytesIO(data))
 return gzip.compress(b.getvalue(),mtime=0)
entries=[]
def add(path,source,mode=0o644):entries.append((path,source.read_bytes(),mode,None))
for m in ['ifb','sch_fq_codel','act_mirred','cls_u32']:add('lib/modules/4.1.52-5.02L.07/'+m+'.ko',O/'modules'/(m+'.ko'))
add('usr/libexec/tc-tiny',O/'tc-tiny',0o755);add('usr/libexec/hh-sqm',S/'hh-sqm',0o755);add('etc/init.d/hh-sqm',S/'hh-sqm.init',0o755);add('etc/config/hh_sqm',S/'hh_sqm.config',0o600);add('www/luci-static/resources/view/network/hh_sqm.js',S/'hh_sqm.js');add('usr/share/luci/menu.d/luci-hh-sqm.json',S/'menu.json');add('usr/share/rpcd/acl.d/luci-hh-sqm.json',S/'acl.json')
entries.append(('usr/sbin/hh-sqm',b'',0o777,'../libexec/hh-sqm'))
control=b'Package: hh4000-sqm\nVersion: 1.0-r6\nArchitecture: aarch64_generic\nDepends: kernel (= 4.1.52~hh4000-1), python3-light, libmnl0, luci-base\nSection: net\nMaintainer: HH4000 Project\nDescription: Optional FQ-CoDel/HTB and IFB queue management for HH4000\n'
post=b'#!/bin/sh\nif [ -z "${IPKG_INSTROOT:-}" ]; then\n /sbin/reload_config || exit 1\n /etc/init.d/hh-sqm enable || exit 1\n /etc/init.d/hh-sqm restart || exit 1\n /etc/init.d/rpcd reload || exit 1\nfi\nexit 0\n'
prerm=b'#!/bin/sh\nif [ -z "${IPKG_INSTROOT:-}" ]; then\n /etc/init.d/hh-sqm stop || exit 1\n /usr/libexec/hh-sqm quiesce || exit 1\n /etc/init.d/hh-sqm disable || exit 1\nfi\nexit 0\n'
postrm=b'#!/bin/sh\n[ -n "${IPKG_INSTROOT:-}" ] || /etc/init.d/rpcd reload\nexit 0\n'
ctl=archive([('prerm',prerm,0o755,None),('postrm',postrm,0o755,None),('control',control,0o644,None),('conffiles',b'/etc/config/hh_sqm\n',0o644,None),('postinst',post,0o755,None)])
f=O/'hh4000-sqm_1.0-r6_aarch64_generic.ipk';f.write_bytes(archive([('debian-binary',b'2.0\n',0o644,None),('control.tar.gz',ctl,0o644,None),('data.tar.gz',archive(entries),0o644,None)]))
m={'package':f.name,'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size,'files':[x[0] for x in entries],'default_enabled':False,'does_not_claim_cake_or_generic_sched_core':True};(O/'sqm-package-manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(json.dumps(m))
