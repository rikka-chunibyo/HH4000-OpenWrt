# OpenWrt RAM stage 1: preparation and resume checkpoint

Prepared 2026-11-08. **Live RAM boot passed:** OpenWrt ARM64 shell and ubus ran on the stock kernel for over eight minutes. The original preparation was offline; the later authorized live experiment is recorded below.

## Ready artifacts

- `work/openwrt-ram/openwrt-stage1.uImage`: 5,825,364-byte legacy ramdisk uImage.
- `work/openwrt-ram/openwrt-stage1.cpio.gz`: corresponding gzip/newc initramfs.
- manifest.json: source URL, pinned checksum and output hashes.
- validation.json: offline validation results.
- [build_openwrt_stage1.py](../tools/build_openwrt_stage1.py): build script, run with Python on dev.
- stock-module-inventory.json: exact stock modules and dependencies for later Ethernet/Wi-Fi work. Only hard_watchdog is included in this first image.

The upstream ARM64 rootfs is OpenWrt **24.10.4 armsr/armv8**, selected as a pinned compatibility baseline, not a claim about the latest release. Its archive matches the SHA256 in the [official download directory](https://downloads.openwrt.org/releases/24.10.4/targets/armsr/armv8/). We use its userspace with the original HH4000 kernel and DTB; the generic armsr kernel is not being used.

The new image is also staged on the Windows laptop at:
`/path/to/local-user/AppData/Local/Temp/opencode/tftp/openwrt-stage1.uImage`.
The existing `kernel.uImage`, `devicetree.dtb` and prior `initramfs.uImage` were not overwritten. Verify the TFTP server and Ethernet 192.168.1.10 again before a live run.

## What this image starts

`/init` uses the ARM32 stock BusyBox/glibc combination from our previously successful RAM image. It mounts proc, sysfs, devtmpfs, devpts and a RAM /tmp, loads the stock watchdog module, starts its keepalive, then runs OpenWrt BusyBox and opens an OpenWrt musl shell. PID 1 remains the rescue bootstrap. Exiting the OpenWrt shell opens the rescue shell; exiting rescue retries OpenWrt.

It deliberately does not invoke `/etc/preinit`, rcS, mount_root, procd, network scripts or any Bell hardware initialization script. Thus this image tests the new userspace before starting any automatic networking or writable NAND mounts. The full OpenWrt startup scripts remain in the archive for later inspection; their presence does not mean they were validated. OpenWrt's normal fw4 setup is unsuitable for the stock kernel without adaptation.

## Watchdog correction

The old notes abbreviated this as `/proc/watchdog`. That is a **directory**. `hard_watchdog.ko` creates **`/proc/watchdog/info`**. Static analysis of the original module establishes:

- Module initialization starts 85,000 ms timeout / 5,000 ms kernel pacify timer.
- The timer increments a retry counter; after 60 missed userspace updates it stops pacifying.
- The write-handler jump table maps **`U`** to the branch at ELF text offset **0x6e0**, which resets the retry counter at `wdg_smu+0x18`.
- The bootstrap writes `U` every five seconds to `/proc/watchdog/info` using the known-working rescue shell.

The U keepalive path was subsequently verified during the live RAM boot, which remained running at 488.24 seconds. Loading the module is the only automatic vendor-driver action. If it fails, the bootstrap prints `HH_WATCHDOG_FAILED`; do not mistake a later watchdog reset for an OpenWrt crash.

## Next live experiment

Use Arduino UART COM3, 115200 baud, DTR/RTS false. Reboot and interrupt the installed U-Boot normally. Do not run old helpers that assume the pre-install U-Boot instruction bytes. The saved environment already loads with the installed U-Boot.

At the U-Boot prompt, transfer the existing stock kernel and DTB plus this new ramdisk. Temporary environment commands only; no saveenv:

```
setenv autostart no
setenv tftpblocksize 1468
setenv initrd_high 0x0fffffff
setenv fdt_high 0x0fffffff
tftpboot 10000000 kernel.uImage
tftpboot 0e000000 openwrt-stage1.uImage
tftpboot 0f000000 devicetree.dtb
setenv bootargs 'mtdparts=brcmnand.0:128k(nvram),640k(cfe),20096k(boot),205312k(ubi),63616k(conf),56832k(log),-(data) ubi.mtd=ubi,0 part_main=ubi part_boot=boot image_ubivol=operational board_type=00030103 root=mtd:rootfs init=/etc/preinit ro rootfstype=squashfs console=ttyS0,115200 rootfs_offset=0x4f7000 rootfs_size=0x252d000 coherent_pool=4M cpuidle_sysfs_switch pci=pcie_bus_safe rootwait cma=0M rdinit=/init'
bootm 10000000 0e000000 0f000000
```

Stop if a transfer fails or its size/hash is wrong. The ramdisk must be a legacy uImage even when experimenting with booti, and the low initrd_high value is required by the previously tested stock-kernel path. This image fits below the next staging region.

Record the UART transcript. Look for `HH_OPENWRT_RAM_STAGE1`, `HH_WATCHDOG_FEEDER_PID` and `HH_OPENWRT_BUSYBOX_OK`. At the shell:

```
cat /etc/openwrt_release
uname -a
cat /proc/1/comm
cat /proc/watchdog/info
cat /proc/mounts
cat /proc/modules
```

Confirm the stock kernel version, OpenWrt release and the absence of a writable NAND overlay. Wait at least seven minutes with UART logging, then check the watchdog state and process again; the missed-update limit is approximately five minutes, so a short uptime test is insufficient. Check basic file operations under /tmp, ubus/ubusd availability and library errors. This is not yet a procd-PID1 or working-network milestone.

Return to the normal saved boot with a relay power cycle and no UART autoboot interrupt. No environment restoration is needed because no saveenv was used. The normal stock boot restores its original hardware initialization.

## Subsequent milestones

1. Demonstrate this shell and watchdog remain stable on the actual router.
2. Prepare a custom procd startup/inittab that keeps automatic storage probing and generic network configuration out of the experiment; verify procd, ubusd and netifd individually.
3. Reproduce Ethernet initialization incrementally from the stock script, using the stock kernel modules and required ARM32/glibc tools. Check one LAN port and SSH first.
4. Bring up one Wi-Fi AP using stock DHD, firmware, calibration and compatible control tools; then expand to three radios.
5. Routing/firewall/LuCI, then optional PON.

**Do not run the unmodified stock `bcm-base-drivers.sh` as a RAM-only shortcut:** it contains `nvram commit` and `nvram kcommit`. A later hardware-init script must deliberately omit those persistent operations. The stock license and calibration data already exist in the backup and should be used from a prepared RAM compatibility area; no need to mount their NAND partitions writable.


## Live result

The actual router booted this exact ramdisk with the stock kernel and DTB. No OS was flashed and no U-Boot environment settings were saved. OpenWrt reported **24.10.4 r28959-29397011cc**, BusyBox 1.36.1, on kernel **4.1.52-5.02L.07**.

Verified on hardware:

- OpenWrt's AArch64 musl shell executed successfully.
- Stock `hard_watchdog.ko` loaded; the U feeder remained alive as PID 452.
- Final observed uptime was **488.24 seconds**, exceeding the missed-update window and hardware timeout without a reboot.
- `ubusd` started as PID 476 on a temporary Unix socket. A listener received `{ "hh.stage1": {} }` from an actual `ubus send`.
- /tmp file writes and reads passed.
- Final mounts were rootfs, proc, sysfs, devtmpfs, tmpfs and devpts; no NAND-backed filesystem was mounted writable. The kernel did attach its normal UBI device; this is not a claim that UBI never performs internal metadata writes.

Scope remains **OpenWrt userspace in RAM**, with the rescue bootstrap as PID 1. procd as PID 1, netifd, Ethernet and Wi-Fi have not yet been tested. Successful CLI/IPC execution does not establish that the entire release supports this old kernel.

The first UART command was truncated; pacing U-Boot commands at six milliseconds per byte resolved it. The live runner openwrt_stage1_live.py uses the installed U-Boot directly, without the old environment-loader workaround.

The bootstrap's `ubus -h` prints usage with an unrecognized-option message; this is a harmless probe mistake, and subsequent real ubus messaging passed. The boot also reported the PRAM FDT reservation warning, omission of `/sys/firmware/fdt` after a CRC check, and lack of shell job control. These warnings are retained in boot-warnings.json; this test does not establish their irrelevance for future driver work.

Evidence: hardware-results.json, full UART transcript, smoke-test extract. The router was returned to its normal stock boot after collecting the final state. The restore runner observed the saved bootcmd marker, stock squashfs mount and stock init, then allowed stock services to initialize; see stock-restoration.raw.

The next milestone is a controlled procd/ubus startup followed by one Ethernet port. Stock runtime inspection remains available for module ordering, firmware requests, ioctls and daemon interactions that the static backup does not fully explain.

For the next one-port Ethernet/SSH test, the existing direct laptop cable is sufficient. A second cable to the main router is useful for later WAN/routing tests, after the port is configured as WAN.
