#!/usr/bin/env python3
from pathlib import Path
from elftools.elf.elffile import ELFFile
import shutil,subprocess,gzip,json,hashlib,os
P=Path(__file__).resolve().parents[1];O=P/'work/openwrt-net';R=O/'rootfs';S=P/'work/rootfs'
O.mkdir(parents=True,exist_ok=True)
if not R.exists():shutil.copytree(P/'work/openwrt-ram/rootfs',R,symlinks=True)
def virt(path):
 path=Path(path)
 for _ in range(12):
  f=S/path
  if not f.is_symlink():return f
  t=f.readlink();path=Path(str(t).lstrip('/')) if t.is_absolute() else path.parent/t
 raise RuntimeError(path)
queue=['usr/bin/nvram','usr/bin/bdmf_shell','usr/bin/ethctl','usr/bin/tmctl','usr/sbin/wl','usr/sbin/wlconf','usr/sbin/hostapd'];done={};libdirs=['lib','usr/lib']
while queue:
 n=queue.pop(0)
 if n in done:continue
 f=virt(n);assert f.is_file(),n
 dest=R/'vendor'/n;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,dest)
 with f.open('rb') as fp:
  e=ELFFile(fp);dyn=e.get_section_by_name('.dynamic');needed=[t.needed for t in dyn.iter_tags() if t.entry.d_tag=='DT_NEEDED'] if dyn else []
  for seg in e.iter_segments():
   if seg['p_type']=='PT_INTERP':queue.append(seg.get_interp_name().lstrip('/'))
 for lib in needed:
  found=next((str(Path(d)/lib) for d in libdirs if (S/d/lib).exists() or (S/d/lib).is_symlink()),None)
  if not found:raise RuntimeError('Missing '+lib)
  queue.append(found)
 done[n]={'source':str(f.relative_to(S)),'bytes':f.stat().st_size,'needed':needed}
# Keep the original tool ABI private behind explicit loader invocation.
bin=R/'usr/sbin';bin.mkdir(exist_ok=True)
for name,path in [('nvram','usr/bin/nvram'),('bdmf_shell','usr/bin/bdmf_shell'),('ethctl','usr/bin/ethctl'),('tmctl','usr/bin/tmctl'),('wlctl','usr/sbin/wl'),('wlconf','usr/sbin/wlconf'),('vendor-hostapd','usr/sbin/hostapd')]:
 f=bin/name
 if f.is_symlink():f.unlink()
 f.write_text('#!/bin/sh\nexec /vendor/lib/ld-linux.so.3 --library-path /vendor/lib:/vendor/usr/lib /vendor/'+path+' "$@"\n');f.chmod(0o755)
shutil.copytree(S/'lib/modules',R/'lib/modules',dirs_exist_ok=True,symlinks=True)
shutil.copytree(S/'etc/wlan',R/'etc/wlan',dirs_exist_ok=True,symlinks=True)
(R/'etc/licenses').mkdir(exist_ok=True);shutil.copy2(S/'etc/licenses/bcm_license.txt',R/'etc/licenses/bcm_license.txt')
(R/'etc/calib').mkdir(exist_ok=True);shutil.copy2(P/'work/ubi/calib/kernel_nvram.setting',R/'etc/calib/kernel_nvram.setting')
(R/'opt/conf').mkdir(parents=True,exist_ok=True);shutil.copy2(P/'work/ubi/fs_filesystem1/conf/scratchpad',R/'opt/conf/scratchpad')
shutil.copy2(P/'work/io-live/hh4000-io.arm',R/'usr/sbin/hh4000-io')
# Manual initialization, separate Ethernet and Wi-Fi phases. No persistent commits.
eth='''#!/bin/sh
set -e
K=/lib/modules/4.1.52-5.02L.07
for m in bcmlibs wlcsm bdmf opticaldet bcm_pondrv rdpa_gpl rdpa_gpl_ext rdpa rdpa_usr rdpa_mw bcmbrfp bcm_license bcm_spdsvc bcm_tcpspdtest chipinfo pktflow bcm_enet cmdlist pktrunner bcmmcast bcmvlan bcm_ingqos rdpa_cmd; do
 echo HH_LOAD_MODULE=$m
 insmod "$K/$m.ko"
 if [ "$m" = bcm_license ]; then cat /etc/licenses/bcm_license.txt >/proc/driver/license; fi
 if [ "$m" = bcm_tcpspdtest ]; then cat /etc/licenses/bcm_license.txt >/proc/spdsvc/license; fi
done
mkdir -p /var
mknod /dev/bdmf_shell c 321 0
bdmf_shell -c init >/tmp/hh-bdmf-init.log 2>&1 || :
grep -q "Session:" /tmp/hh-bdmf-init.log
cat /tmp/hh-bdmf-init.log
# The vendor CLI returns the session ID as its exit status.
# Egress queues are required before packets can leave the selected port.
port=${1:-eth3}
case "$port" in eth[0-4]) ;; *) exit 2 ;; esac
tmctl porttminit --devtype ETH --if "$port" --flag 1
ip link show
'''
wi='''#!/bin/sh
set -e
K=/lib/modules/4.1.52-5.02L.07
nvram kernelset /etc/calib/kernel_nvram.setting
nvram set 1:ccode=CA
nvram set 1:regrev=0
nvram set 2:ccode=CA
nvram set 2:regrev=0
nvram set 3:ccode=CA
nvram set 3:regrev=0
nvram set regrev=0
for m in wfd hnd emf igs; do insmod "$K/$m.ko"; done
# cfg80211 may be loaded before Ethernet if a dependency needs it.
if ! grep -q '^cfg80211 ' /proc/modules; then insmod "$K/cfg80211.ko"; fi
insmod "$K/dhd.ko" dhd_dpc_prio=5 iface_name=wl dhd_console_ms=0 firmware_path=/etc/wlan/dhd/
ip link show
'''
for n,s in [('hh-ethernet-init',eth),('hh-wifi-init',wi)]:f=bin/n;f.write_text(s);f.chmod(0o755)
# GPIO bootstrap dependencies are not auto-loaded; retain separate explicit control.
(R/'init').write_text((P/'work/openwrt-ram/rootfs/init').read_text().replace('/bin/ubus -h 2>&1',': # ubus is tested through a real daemon after startup'))
paths=['.']+sorted('./'+str(f.relative_to(R)) for f in R.rglob('*'))
a=subprocess.run(['cpio','--null','-o','--format=newc','--owner=0:0'],cwd=R,input=('\0'.join(paths)+'\0').encode(),capture_output=True,check=True).stdout
(O/'openwrt-net.cpio.gz').write_bytes(gzip.compress(a,mtime=0))
subprocess.run(['mkimage','-A','arm64','-O','linux','-T','ramdisk','-C','gzip','-n','HH4000 OpenWrt network test','-d',str(O/'openwrt-net.cpio.gz'),str(O/'openwrt-net.uImage')],check=True)
(O/'vendor-dependencies.json').write_text(json.dumps(done,indent=2)+'\n')
f=O/'openwrt-net.uImage';print('IMAGE',f.stat().st_size,hashlib.sha256(f.read_bytes()).hexdigest(),'LIBRARIES',len(done))
