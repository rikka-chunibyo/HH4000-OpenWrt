#!/usr/bin/env python3
"""Reject corrupt updates and exercise the target script on disposable storage."""
from pathlib import Path
import importlib.util,json,os,shutil,subprocess,tempfile
P=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('update',P/'tools/hh_firmware_update.py');u=importlib.util.module_from_spec(spec);spec.loader.exec_module(u)
STAGE=P/'work/remaining-validation/update-stage';results={}
with tempfile.TemporaryDirectory(prefix='hh-update-test-') as td:
 T=Path(td);s=T/'stage';shutil.copytree(STAGE,s,symlinks=True);m=u.check(s);results['valid_stage']=True
 for bad in ['../etc/config/network','/etc/config/network','etc/config/network','etc/shadow','etc/dropbear/authorized_keys','usr/lib/opkg/status','-P','usr//bin/x','etc/ssl/private/test.key','etc/firewall.user']:
  try:u.safe(bad)
  except ValueError:results['reject_path_'+bad]=True
  else:raise AssertionError(bad)
 for name in ['root.squashfs','bootstrap.uImage','update.sh']:
  f=s/name;b=f.read_bytes();f.write_bytes(b[:-1]+bytes([b[-1]^1]))
  try:u.check(s)
  except (ValueError,UnicodeError):results['reject_tamper_'+name]=True
  else:raise AssertionError(name)
  finally:f.write_bytes(b)
 original=(s/'manifest.json').read_text();modified=json.loads(original);modified['managed'].append(modified['managed'][0]);(s/'manifest.json').write_text(json.dumps(modified))
 try:u.check(s)
 except ValueError:results['reject_duplicate']=True
 else:raise AssertionError('duplicate')
 (s/'manifest.json').write_text(original)
 # Preserve the image hash while breaking its internal header CRC.
 f=s/'bootstrap.uImage';b=f.read_bytes();bad=bytearray(b);bad[8]^=1;f.write_bytes(bad);modified=json.loads(original);modified['images']['bootstrap.uImage']['sha256']=u.sha(bad);(s/'manifest.json').write_text(json.dumps(modified))
 try:u.check(s)
 except ValueError:results['reject_bad_uimage_crc']=True
 else:raise AssertionError('CRC')
 f.write_bytes(b);(s/'manifest.json').write_text(original)
 # Translate a COPY of the exact generated target script into a disposable root.
 fs=T/'fs';fs.mkdir();stage_script=(s/'update.sh').read_text();fakebin=T/'bin';fakebin.mkdir();log=T/'operations.log'
 def put(path,data):
  f=fs/path.lstrip('/');f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(data if isinstance(data,bytes) else data.encode());return f
 put('/tmp/hh-storage-mode','RAM\n');put('/proc/device-tree/compatible',b'brcm,brcm-v8A\0')
 put('/proc/mounts',str(fs)+'/dev/loop0 /rom squashfs ro,relatime 0 0\n')
 for num,mtd in [(0,3),(1,2),(2,4)]:put('/sys/class/ubi/ubi'+str(num)+'/mtd_num',str(mtd))
 for num,name in [(6,'owrt-root'),(8,'owrt-init')]:
  put('/sys/class/ubi/ubi0_'+str(num)+'/name',name);put('/sys/class/ubi/ubi0_'+str(num)+'/reserved_ebs','256');put('/sys/class/ubi/ubi0_'+str(num)+'/data_bytes','4096')
 put('/sys/class/ubi/ubi0/eraseblock_size','126976');put('/sys/class/ubi/ubi2_0/name','owrt-data')
 put('/dev/ubi0_6',b'OLD ROOT');put('/dev/ubi0_8',b'OLD BOOTSTRAP')
 upper=fs/'mnt/hh-update-data/upper';upper.mkdir(parents=True)
 protected={'etc/config/network':b'user network','etc/shadow':b'FAKE TEST ONLY','etc/dropbear/authorized_keys':b'fake public key','usr/lib/opkg/status':b'user packages','usr/share/hh-frontpanel/programs/custom.py':b'custom code','root/data.bin':os.urandom(3000)}
 for name,data in protected.items():f=upper/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_bytes(data)
 target=upper/'sbin/firstboot';target.parent.mkdir(parents=True,exist_ok=True);sentinel=upper/'root/untouched-directory';sentinel.mkdir();(sentinel/'sentinel').write_text('unchanged');target.symlink_to(sentinel)
 shim=T/'busybox';shim.write_text('#!/bin/sh\ncmd=$1; shift\ncase "$cmd" in\n mount|umount) echo "$cmd" >>'+str(log)+'; exit 0;;\n ubiupdatevol) echo "write:$1" >>'+str(log)+'; cp "$2" "$1";;\n *) exec "$cmd" "$@";;\nesac\n');shim.chmod(0o700)
 
 for command,reply in [('id','0'),('chown',None)]:
  mock=fakebin/command;mock.write_text('#!/bin/sh\n'+('echo '+reply if reply is not None else ':')+'\n');mock.chmod(0o700)
 uname=fakebin/'uname';uname.write_text('#!/bin/sh\necho 4.1.52-5.02L.07\n');uname.chmod(0o700)
 for prefix in ['/tmp/hh-storage-mode','/tmp/hh-firmware-update.lock','/mnt/hh-update-data','/proc/device-tree/compatible','/proc/mounts','/sys/class/ubi','/dev/ubi']:
  stage_script=stage_script.replace(prefix,str(fs)+prefix)
 stage_script=stage_script.replace('/dev/loop0',str(fs)+'/dev/loop0').replace('BB=/rescue/busybox','BB='+str(shim)).replace('PATH=/usr/sbin:/usr/bin:/sbin:/bin','PATH='+str(fakebin)+':/usr/sbin:/usr/bin:/sbin:/bin')
 testscript=s/'test-update.sh';testscript.write_text(stage_script)
 subprocess.run(['sh',str(testscript),'--preflight'],check=True)
 assert 'write:' not in log.read_text();assert (fs/'dev/ubi0_6').read_bytes()==b'OLD ROOT';results['preflight_never_writes_firmware']=True
 stale=upper/(m['managed'][0]['path']+'.hh-update-new');stale.parent.mkdir(parents=True,exist_ok=True);stale.write_text('stale')
 prior=log.read_text();r=subprocess.run(['sh',str(testscript),'--install'],capture_output=True,text=True);assert r.returncode!=0 and 'stale-temp-file' in r.stderr and 'write:' not in log.read_text();results['stale_temp_refused_before_writes']=True;stale.unlink()
 subprocess.run(['sh',str(testscript),'--install'],check=True)
 assert (fs/'dev/ubi0_6').read_bytes()==(s/'root.squashfs').read_bytes();assert (fs/'dev/ubi0_8').read_bytes()==(s/'bootstrap.uImage').read_bytes();results['install_readbacks_match']=True
 for name,data in protected.items():assert (upper/name).read_bytes()==data,name
 assert (sentinel/'sentinel').read_text()=='unchanged';assert not (sentinel/'firstboot.hh-update-new').exists();results['protected_settings_keys_packages_programs_preserved']=True;results['destination_symlink_not_followed']=True
 assert not (fs/'tmp/hh-firmware-update.lock').exists();results['lock_cleanup']=True
 # The unmodified target must reject a normal installed OS before reaching mount/write.
 installed=T/'installed.sh';installed.write_text((s/'update.sh').read_text());r=subprocess.run(['sh',str(installed),'--preflight'],capture_output=True,text=True)
 assert r.returncode!=0 and ('requires-maintenance-RAM' in r.stderr or 'requires-root' in r.stderr);results['normal_OS_refused']=True
results['all_passed']=all(results.values());(P/'work/remaining-validation/update-workflow-tests.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
