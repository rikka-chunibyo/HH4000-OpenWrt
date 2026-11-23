#!/usr/bin/env python3
"""Stage an HH4000 data-preserving update on the dev host; never flash here."""
from pathlib import Path, PurePosixPath
import argparse,hashlib,json,os,re,shlex,shutil,stat,struct,subprocess,zlib
P=Path(__file__).resolve().parents[1]
PROTECTED=('etc/config','etc/dropbear','etc/calib','etc/shadow','etc/passwd','etc/group','etc/ssh','root','overlay','usr/lib/opkg','usr/share/hh-frontpanel/programs')
def sha(data):return hashlib.sha256(data).hexdigest()
def safe(name):
 if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_./+-]+',name) or name.startswith('/') or any(x in ('','.','..') for x in name.split('/')):raise ValueError('Unsafe managed path: '+name)
 if any(name==x or name.startswith(x+'/') for x in PROTECTED):raise ValueError('Protected user data: '+name)
 if name.startswith('etc/') and not name.startswith(('etc/init.d/','etc/rc.d/','etc/rc.button/','etc/hotplug.d/')):raise ValueError('Protected configuration: '+name)
 return name
def uimage(b):
 if len(b)<64:raise ValueError('Truncated bootstrap')
 magic,hcrc,_,size,_,_,dcrc=struct.unpack('>7I',b[:28])
 if magic!=0x27051956 or zlib.crc32(b[:4]+bytes(4)+b[8:64])!=hcrc or size!=len(b)-64 or zlib.crc32(b[64:])!=dcrc or b[28:32]!=bytes((5,22,3,1)):raise ValueError('Invalid arm64 Linux gzip ramdisk uImage')
def render(m):
 t=(P/'src/openwrt-hh4000/persistent/update-ram.sh.in').read_text();checks=[];refresh=[]
 for item in m['managed']:
  n=item['path'];q=shlex.quote(n)
  if item['kind']=='file':
   checks.append('check_hash "$D/platform/'+n+'" '+item['sha256']);refresh.append('refresh_file '+q+' '+format(item['mode'],'o'))
  else:
   checks.append('[ "$(readlink "$D/platform/'+n+'")" = '+shlex.quote(item['target'])+' ] || fail platform-link');refresh.append('refresh_link '+q+' '+shlex.quote(item['target']))
 values={'ROOT_HASH':m['images']['root.squashfs']['sha256'],'INIT_HASH':m['images']['bootstrap.uImage']['sha256'],'ROOT_BYTES':str(m['images']['root.squashfs']['bytes']),'INIT_BYTES':str(m['images']['bootstrap.uImage']['bytes']),'PLATFORM_CHECKS':'\n'.join(checks),'UPPER_CHECKS':'\n'.join(checks).replace('$D/platform/','$U/'),'PLATFORM_REFRESH':'\n'.join(refresh),'MANAGED_PATHS':'\n'.join(x['path'] for x in m['managed'])}
 return re.sub(r'@([A-Z_]+)@',lambda match: values[match.group(1)],t)
def check(directory):
 d=Path(directory);m=json.loads((d/'manifest.json').read_text())
 if m.get('schema')!=1 or m.get('board')!='FAST5689' or m.get('abi')!='4.1.52-5.02L.07':raise ValueError('Unsupported target')
 for n,limit in [('root.squashfs',24*1024*1024),('bootstrap.uImage',4*1024*1024)]:
  f=d/n
  if f.is_symlink() or not f.is_file():raise ValueError('Missing regular image')
  b=f.read_bytes()
  if not 0<len(b)<=limit or m['images'][n]!={'bytes':len(b),'sha256':sha(b)}:raise ValueError('Image size/hash mismatch: '+n)
  if n.endswith('uImage'):uimage(b)
  elif b[:4]!=b'hsqs':raise ValueError('Bad SquashFS magic')
 seen=set()
 if not m['managed']:raise ValueError('No managed paths')
 for item in m['managed']:
  n=safe(item['path']);f=d/'platform'/n
  if n in seen:raise ValueError('Duplicate managed path')
  seen.add(n)
  if item['kind']=='file':
   if f.is_symlink() or not f.is_file() or sha(f.read_bytes())!=item['sha256'] or stat.S_IMODE(f.stat().st_mode)!=item['mode']:raise ValueError('Managed file mismatch: '+n)
  elif item['kind']=='symlink':
   if not f.is_symlink() or os.readlink(f)!=item['target'] or any(x in item['target'] for x in ['\n','\r','\0']):raise ValueError('Managed link mismatch: '+n)
  else:raise ValueError('Unsupported entry')
  for parent in f.parents:
   if parent==d:break
   if parent.is_symlink():raise ValueError('Symlink parent')
 if (d/'update.sh').is_symlink() or (d/'update.sh').read_text()!=render(m):raise ValueError('Unexpected updater script')
 return m
def prepare(build,paths,output):
 build=Path(build).resolve();d=Path(output).absolute()
 if d.exists():raise ValueError('Output exists; choose a new directory')
 names=[safe(x.strip()) for x in Path(paths).read_text().splitlines() if x.strip() and not x.startswith('#')]
 if not names or len(set(names))!=len(names):raise ValueError('Empty or duplicate path list')
 d.mkdir(mode=0o700);m={'schema':1,'board':'FAST5689','abi':'4.1.52-5.02L.07','images':{},'managed':[]}
 for n in ['root.squashfs','bootstrap.uImage']:
  b=(build/n).read_bytes();(d/n).write_bytes(b);m['images'][n]={'bytes':len(b),'sha256':sha(b)}
 extracted=d/'.extracted';subprocess.run(['unsquashfs','-no-progress','-d',str(extracted),str(d/'root.squashfs'),*names],check=True,stdout=subprocess.DEVNULL)
 for n in names:
  source=extracted/n;dest=d/'platform'/n
  for parent in source.parents:
   if parent==extracted:break
   if parent.is_symlink():raise ValueError('Symlink parent in root image')
  dest.parent.mkdir(parents=True,exist_ok=True)
  if source.is_symlink():
   target=os.readlink(source)
   if any(x in target for x in ['\n','\r','\0']):raise ValueError('Invalid link')
   dest.symlink_to(target);item={'path':n,'kind':'symlink','target':target}
  elif source.is_file():
   shutil.copy2(source,dest);item={'path':n,'kind':'file','mode':stat.S_IMODE(source.stat().st_mode),'sha256':sha(source.read_bytes())}
  else:raise ValueError('Managed path missing: '+n)
  m['managed'].append(item)
 shutil.rmtree(extracted);(d/'manifest.json').write_text(json.dumps(m,indent=2)+'\n');(d/'update.sh').write_text(render(m));(d/'update.sh').chmod(0o700);check(d);subprocess.run(['sh','-n',str(d/'update.sh')],check=True);return m
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);s=p.add_subparsers(dest='action',required=True);a=s.add_parser('prepare');a.add_argument('--build',required=True);a.add_argument('--managed-paths',required=True);a.add_argument('--output',required=True);a=s.add_parser('check');a.add_argument('directory');a=p.parse_args();m=prepare(a.build,a.managed_paths,a.output) if a.action=='prepare' else check(a.directory);print(json.dumps({'valid':True,'images':m['images'],'managed_files':len(m['managed']),'writes_router_flash':False},indent=2))
