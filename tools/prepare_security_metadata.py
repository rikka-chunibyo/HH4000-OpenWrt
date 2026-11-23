#!/usr/bin/env python3
"""Pin and identify locally built HH4000 security components after verifying hashes."""
from pathlib import Path
import argparse,json,hashlib,re
P=Path(__file__).resolve().parents[1]
PACKAGES={
 'uhttpd':('2026.11.05~93b05e0b-hh1',['usr/sbin/uhttpd']),
 'uhttpd-mod-ubus':('2026.11.05~93b05e0b-hh1',['usr/lib/uhttpd_ubus.so']),
 'odhcpd-ipv6only':('2025.10.02~b14cf98c-hh-security1',['usr/sbin/odhcpd']),
 'cgi-io':('2026.07.21~31cb3c89-hh1',['usr/libexec/cgi-io']),
 'procd':('2024.12.22~42d39376-hh-security1',['sbin/procd'])}
def apply(root,manifest=None):
 m=json.loads(Path(manifest or P/'work/security-audit/source-manifest.json').read_text())['files']
 for version,names in PACKAGES.values():
  for n in names:assert hashlib.sha256((root/n).read_bytes()).hexdigest()==m[n],n
 def update(block,name):
  block=re.sub(r'^Version:.*$', 'Version: '+PACKAGES[name][0],block,flags=re.M)
  if re.search(r'^Status:',block,re.M):block=re.sub(r'^Status:.*$','Status: install hold installed',block,flags=re.M)
  return block
 status=root/'usr/lib/opkg/status';blocks=status.read_text().split('\n\n');seen=set()
 for i,b in enumerate(blocks):
  match=re.search(r'^Package: ([^\n]+)',b,re.M);name=match[1] if match else ''
  if name in PACKAGES:blocks[i]=update(b,name);seen.add(name)
 assert seen==set(PACKAGES),(seen,set(PACKAGES))
 for name in PACKAGES:
  control=root/'usr/lib/opkg/info'/ (name+'.control')
  if control.exists():control.write_text(update(control.read_text(),name))
 status.write_text('\n\n'.join(b.strip() for b in blocks if b.strip())+'\n\n')
 return {name:version for name,(version,files) in PACKAGES.items()}
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('root',type=Path);a.add_argument('--manifest',type=Path);v=a.parse_args();print(json.dumps(apply(v.root,v.manifest)))
