#!/usr/bin/env python3
"""Record verified legacy userspace packages and the actual vendor kernel ABI in a rootfs."""
from pathlib import Path
import argparse,json,tarfile,io,hashlib
P=Path(__file__).resolve().parents[1]
def apply(root):
 info=root/'usr/lib/opkg/info';info.mkdir(parents=True,exist_ok=True);status=root/'usr/lib/opkg/status'
 def fields(block):
  return dict(line.split(': ',1) for line in block.splitlines() if ': ' in line and not line.startswith(' '))
 blocks=status.read_text().split('\n\n');kept=[];removed=[]
 for b in blocks:
  f=fields(b);name=f.get('Package','')
  if name=='firewall4' or name=='kernel' or name.startswith('kmod-') or name=='hh4000-netfilter':removed.append(name)
  else:kept.append(b)
 manifest=json.loads((P/'work/wan-firewall/package-manifest.json').read_text())
 names=set(manifest);kept=[b for b in kept if fields(b).get('Package') not in names]
 for name in removed:
  for f in info.glob(name+'.*'):
   if f.is_file() or f.is_symlink():f.unlink()
 for name,m in manifest.items():
  archive=P/'work/wan-firewall/packages'/m['filename'];assert hashlib.sha256(archive.read_bytes()).hexdigest()==m['sha256']
  with tarfile.open(archive) as ar:
   ctl=ar.extractfile(next(x for x in ar if Path(x.name).name=='control.tar.gz')).read();data=ar.extractfile(next(x for x in ar if Path(x.name).name=='data.tar.gz')).read()
  control='';conffiles=[]
  with tarfile.open(fileobj=io.BytesIO(ctl),mode='r:gz') as t:
   for member in t:
    if not member.isfile():continue
    n=Path(member.name).name;b=t.extractfile(member).read()
    if n=='control':control=b.decode()
    elif n=='conffiles':conffiles=b.decode().splitlines()
    else:
     dest=info/(name+'.'+n);dest.write_bytes(b)
     dest.chmod(0o755 if n in ('postinst','preinst','prerm','postrm') else 0o644)
  assert fields(control).get('Package')==name
  kept.append(control.rstrip()+'\nStatus: install ok installed\n'+('Conffiles:\n'+''.join(' '+f+' '+hashlib.sha256((root/f.lstrip('/')).read_bytes()).hexdigest()+'\n' for f in conffiles if (root/f.lstrip('/')).is_file()) if conffiles else ''))
  (info/(name+'.control')).write_text(control)
  with tarfile.open(fileobj=io.BytesIO(data),mode='r:gz') as t:
   entries=['/'+x.name.removeprefix('./') for x in t if x.isfile() or x.issym()]
  (info/(name+'.list')).write_text('\n'.join(entries)+'\n')
 version='4.1.52~hh4000-1'
 kernel='Package: kernel\nVersion: '+version+'\nDepends: libc\nArchitecture: aarch64_generic\nDescription: Retained HH4000 vendor 4.1.52-5.02L.07 kernel\n'
 providers=['kmod-ipt-core','kmod-ipt-conntrack','kmod-nf-conntrack','kmod-nf-conntrack6','kmod-ipt-nat','kmod-nf-nat','kmod-ip6tables']
 vendor='Package: hh4000-netfilter\nVersion: 1.1\nDepends: libxtables12, kernel (= '+version+')\nProvides: '+', '.join(providers)+'\nArchitecture: aarch64_generic\nDescription: Verified stock-ABI netfilter modules and HH4000 firewall integration\n'
 for n,c in [('kernel',kernel),('hh4000-netfilter',vendor)]:
  kept.append(c+'Status: install '+('hold' if n=='kernel' else 'ok')+' installed\n');(info/(n+'.control')).write_text(c)
  files=['/lib/modules/4.1.52-5.02L.07/'+m+'.ko' for m in json.loads((P/'work/wan-firewall/stock-module-order.json').read_text())+['xt_comment','xt_SKIPLOG']] if n!='kernel' else []
  if n=='hh4000-netfilter':files+=['/usr/lib/iptables/libxt_SKIPLOG.so','/usr/libexec/hh-vpn-offload']
  (info/(n+'.list')).write_text('\n'.join(files)+'\n')
 pinned={'kernel','netifd','procd','base-files','busybox','dnsmasq','firewall'}
 for index,b in enumerate(kept):
  if fields(b).get('Package') in pinned:
   import re
   kept[index]=re.sub(r'Status: [^\n]+','Status: install hold installed',b)
 status.write_text('\n\n'.join(b.strip() for b in kept if b.strip())+'\n\n')
 return {'removed_old_kernel_package_records':removed,'userspace_packages':sorted(names),'kernel_version':version,'vendor_module_providers':providers}
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('root',type=Path);a=ap.parse_args();print(json.dumps(apply(a.root),indent=2))
