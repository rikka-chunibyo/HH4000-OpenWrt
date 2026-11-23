#!/usr/bin/env python3
# Build on dev; only writes work/openwrt-ram, never contacts router.
from pathlib import Path
import hashlib,json,shutil,subprocess,gzip,os,sys
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'work/openwrt-ram';FS=OUT/'rootfs'
ARCHIVE=OUT/'downloads/openwrt-24.10.4-armsr-armv8-rootfs.tar.gz'
EXPECTED='b9e784570872f0c3875d75c54eba842fa6f8a9fb2503897080db240fff6431a3'
assert hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()==EXPECTED
assert (FS/'sbin/procd').is_file()
rescue=FS/'rescue';rescue.mkdir(exist_ok=True)
shutil.copy2(ROOT/'work/boot/ramboot/initramfs/bin/busybox',rescue/'busybox')
(rescue/'busybox').chmod(0o755)
for f in (ROOT/'work/boot/ramboot/initramfs/lib').iterdir():
 target=FS/'lib'/f.name
 if target.is_symlink():target.unlink()
 if f.is_symlink():target.symlink_to(os.readlink(f))
 elif f.is_file():shutil.copy2(f,target)
mod=ROOT/'work/rootfs/lib/modules/4.1.52-5.02L.07/hard_watchdog.ko'
shutil.copy2(mod,rescue/'hard_watchdog.ko')
init=r'''#!/rescue/busybox sh
# Skip preinit, rcS, mount_root, network configuration and overlay.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
BB=/rescue/busybox
$BB mount -t proc proc /proc 2>/dev/null
$BB mount -t sysfs sysfs /sys 2>/dev/null
$BB mount -t devtmpfs devtmpfs /dev 2>/dev/null
$BB mkdir -p /tmp /run /dev/pts
$BB mount -t tmpfs -o mode=1777 tmpfs /tmp 2>/dev/null
$BB mount -t devpts devpts /dev/pts 2>/dev/null
echo HH_OPENWRT_RAM_STAGE1
$BB uname -a
if $BB insmod /rescue/hard_watchdog.ko && [ -w /proc/watchdog/info ]; then
    (
        while :; do
            printf 'U\n' >/proc/watchdog/info || exit 1
            $BB sleep 5
        done
    ) &
    echo HH_WATCHDOG_FEEDER_PID=$!
else
    echo 'HH_WATCHDOG_FAILED: board may reset; return to stock boot.'
fi
/bin/busybox echo HH_OPENWRT_BUSYBOX_OK
/bin/busybox uname -a
/bin/ubus -h 2>&1
while :; do
    echo 'OpenWrt RAM shell; networking and writable NAND mounts not initialized.'
    /bin/sh -i
    echo 'OpenWrt shell exited; rescue shell available. Exit rescue to retry.'
    $BB sh -i
done
'''
(FS/'init').write_text(init);(FS/'init').chmod(0o755)
for n in ['proc','sys','dev','tmp','run']:(FS/n).mkdir(exist_ok=True)
paths=['.']+sorted('./'+str(x.relative_to(FS)) for x in FS.rglob('*'))
cpio=subprocess.run(['cpio','--null','-o','--format=newc','--owner=0:0'],cwd=FS,input=('\0'.join(paths)+'\0').encode(),capture_output=True,check=True)
(OUT/'openwrt-stage1.cpio.gz').write_bytes(gzip.compress(cpio.stdout,mtime=0))
subprocess.run(['mkimage','-A','arm64','-O','linux','-T','ramdisk','-C','gzip','-n','HH4000 OpenWrt console stage1','-d',str(OUT/'openwrt-stage1.cpio.gz'),str(OUT/'openwrt-stage1.uImage')],check=True)
assert (OUT/'openwrt-stage1.uImage').stat().st_size<0x1000000
sys.path.insert(0,str(ROOT/'tools'))
from uboot_ramboot import STOCK_ARGS
(OUT/'bootargs.txt').write_text(STOCK_ARGS+'\n')
manifest={'rootfs_url':'https://downloads.openwrt.org/releases/24.10.4/targets/armsr/armv8/'+ARCHIVE.name,'rootfs_sha256':EXPECTED,'stage':'console-only userspace; procd PID1 not yet tested','hardware_tested':False,'automatic_vendor_modules':['hard_watchdog.ko'],'watchdog_control':'U every 5 seconds to /proc/watchdog/info; statically verified, not yet RAM-boot tested','kernel':'existing stock kernel.uImage','dtb':'existing stock devicetree.dtb','files':{}}
for name in ['openwrt-stage1.cpio.gz','openwrt-stage1.uImage','bootargs.txt']:
 data=(OUT/name).read_bytes();manifest['files'][name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
(OUT/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
