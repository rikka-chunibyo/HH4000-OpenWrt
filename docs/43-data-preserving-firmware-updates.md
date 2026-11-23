# Data-preserving firmware update workflow

`tools/hh_firmware_update.py` replaces the previous installation-specific
script with a repeatable staging tool. It runs on the Linux dev host and
never flashes a router. The target script runs only in the maintenance RAM
OS, checks the retained kernel ABI and actual UBI volume mappings, backs up
the current images and managed upper-layer code, then optionally updates
`ubi0_6` (`owrt-root`) and `ubi0_8` (`owrt-init`).

The separate `ubi2:owrt-data` filesystem is preserved. Only the explicit
platform paths in `src/openwrt-hh4000/persistent/managed-platform-paths.txt`
are refreshed in its upper directory; these are extracted from the same
SquashFS being installed. Configuration, SSH keys, calibration, optional
packages, package metadata, and user panel programs are outside that list.
Edits to managed platform code are replaced, with their old upper files
saved in the backup. This does not perform a package-version migration.
Use this workflow for the current board/kernel/storage layout, rather than
treating it as a generic OpenWrt target sysupgrade image.

## Prepare on the dev host

Use a fresh build/output directory for each candidate:

```sh
cd /path/to/HH4000-OpenWrt
python3 tools/build_openwrt_persistent.py --output work/my-update
python3 tools/hh_firmware_update.py prepare \
  --build work/my-update \
  --managed-paths src/openwrt-hh4000/persistent/managed-platform-paths.txt \
  --output work/hh-update-stage
python3 tools/hh_firmware_update.py check work/hh-update-stage
tar -czf work/hh-update-stage.tgz -C work hh-update-stage
```

Preparation requires `unsquashfs`, `mksquashfs`, `mkimage`, and the existing
cross-build prerequisites. It rejects protected/traversing/duplicate paths,
checks image sizes and SHA-256, validates the bootstrap legacy-uImage
header/data CRCs and ARM64/Linux/gzip/ramdisk type, and verifies every
managed file or symlink. `check` also verifies that `update.sh` matches the
local trusted template. Hashes/CRCs detect corruption; they are not a
firmware publisher signature. Keep the staging directory private and use
trusted builds. Never execute an updater script from an unverified archive.

## Boot maintenance RAM

The tested route now uses a temporary file on the separate data filesystem.
This avoids the Windows multi-adapter TFTP problem. While the installed OS
is running, create a private `/overlay/.hh-maintenance` directory and use
a binary-safe SSH transfer to stage the build's `ramtest.uImage` there, plus
`update-stage.tgz`. Verify both SHA-256 values after transfer. These are
ordinary data-file writes, not firmware-volume updates. Sync, software
reboot, and interrupt U-Boot with paced UART commands. Then run:

```
setenv initrd_high 0x0fffffff
setenv fdt_high 0x0fffffff
ubi part ubi
run hh_load_kernel
run hh_load_dtb
ubi part storage
ubifsmount ubi0:owrt-data
ubifsload 0e000000 /.hh-maintenance/ramtest.uImage
ubifsumount
```

Check `crc32 0e000000 <image-length-in-hex>` against the host's full-image
CRC32. `iminfo` is unavailable in this U-Boot. Set the verified maintenance
boot arguments with `hh.persist=0`, then
`bootm 10000000 0e000000 0f000000`. `bootm` also verifies legacy image CRCs.
The kernel/DTB come from their retained firmware volumes; the embedded-root
maintenance image comes from the data file. UART transports commands/logs.
There is no Ethernet image transfer in this route. Do not save environment.

The optional Ethernet TFTP alternative is described next; it failed with
all four laptop adapters connected during this session. In U-Boot, ARP was
seen on the first two LAN adapters, while the laptop's static 192.168.1.10
address was on a different port. USB DHCP addresses disappeared after Linux
stopped. Treat that path as requiring a suitable directly connected adapter
with a stable address and working TFTP source-interface selection.


Keep direct power connected while the relay path is being investigated.
Stage `work/my-update/ramtest.uImage` as `hh-maintenance.uImage` and the
verified `hh-update-stage.tgz` on the laptop's existing TFTP server at
`192.168.1.10`, alongside the retained stock `kernel.uImage` and
`devicetree.dtb`.

Record the installed configuration/package fingerprints first. Sync the
router, use the known working software reboot path
`sync; /rescue/busybox reboot -f`, and interrupt U-Boot over Arduino UART.
The normal `reboot` command does not reliably reboot this custom-init image.
Do not toggle the bypassed relay. Load the three images over **Ethernet
TFTP**; UART carries commands and logs only:

```
setenv autostart no
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.10
setenv tftpblocksize 1468
setenv initrd_high 0x0fffffff
setenv fdt_high 0x0fffffff
tftpboot 10000000 kernel.uImage
tftpboot 0e000000 hh-maintenance.uImage
tftpboot 0f000000 devicetree.dtb
```

Set the verified boot arguments from `work/openwrt-install/bootargs.txt`,
changing the maintenance selection to `hh.persist=0` (and removing any
`hh.repartition=1`). Then run `bootm 10000000 0e000000 0f000000`.
These are temporary U-Boot environment changes: **do not `saveenv`**.
The generated maintenance image must remain below 16 MiB for this address
layout; the builder enforces that limit. Confirm `HH_SWITCH_ROOT=RAM`,
`/tmp/hh-storage-mode` equals `RAM`, and the root comes from embedded
`/dev/loop0` before proceeding.

## Preflight and optional installation

For the tested data-file route, attach UBI1 to mtd2 and UBI2 to mtd4 if
they are not already attached. Verify those mappings, mount
`ubi2:owrt-data` temporarily at `/mnt/hh-stage`, copy
`/mnt/hh-stage/.hh-maintenance/update-stage.tgz` to `/tmp`, then unmount it.
Extract the archive in `/tmp` and run preflight. The target updater requires
data to be unmounted so it can own that mount itself.

For the Ethernet TFTP alternative, in the maintenance shell:

```sh
/rescue/busybox tftp -g -r hh-update-stage.tgz \
  -l /tmp/hh-update-stage.tgz 192.168.1.10
tar -xzf /tmp/hh-update-stage.tgz -C /tmp
sh /tmp/hh-update-stage/update.sh --preflight
```

Preflight does **not** call `ubiupdatevol`. It mounts the separate data
filesystem temporarily and writes verified backups to
`/mnt/hh-update-data/.hh-updates/<UTC-time>-<pid>`. The same directory is
visible under `/overlay/.hh-updates` after a normal installed boot.
Backups include the old root/bootstrap, hashes, update manifest, path list,
and a tar of old managed upper files. It unmounts data on exit. A mounted
data filesystem, pending factory reset, wrong volumes/ABI/device tree,
insufficient space, custom directory/symlink parents, or bad payload hashes
cause failure before firmware writes.

Only after validating the candidate's real RAM boot and preflight, the
actual installation command is:

```sh
sh /tmp/hh-update-stage/update.sh --install
```

It repeats preflight/backup, skips unchanged images, verifies each written
volume, refreshes only listed platform files with atomic replacement,
checks the resulting file hashes/links, syncs and unmounts data. It does
not change the kernel volume, DTB, boot partition/bootloaders, saved U-Boot
environment, identity, or calibration. There is no A/B rollback: interruption
during image/code refresh requires the retained U-Boot + maintenance RAM
recovery path and the saved images/code archive. Retain a host copy of the
backups before future updates. The tool refuses to write the mounted
installed root and does not format the data filesystem.

After `HH_UPDATE_READBACK_OK_DATA_PRESERVED`, or after a preflight-only
test, return to the installed OS with:

```sh
sync
/rescue/busybox reboot -f
```

Do not interrupt autoboot on the return trip. Compare the saved config,
public authorized-key, panel-program and package fingerprints, and check
LAN/WAN, Wi-Fi, SSH, panel, reset service, optional TUN, and default-disabled
SQM. The recorded host harnesses under `tools/remaining_validation` document
the RAM-only test and deliberately never call `--install`. Failed transfer
attempts are retained as evidence, not presented as successful tests.

## Validation

`python3 tools/test_hh_firmware_update.py` runs meaningful negative checks
and executes a translated copy of the generated shell script against a
disposable filesystem with mount/UBI-write shims. It verifies corrupt
payload/CRC/script rejection, protected/duplicate paths, no firmware writes
in preflight, install readback, preservation of sample settings/keys/package
metadata/programs, symlink destinations, and lock cleanup. This is not a
claim that the shim simulates NAND power loss. Actual hardware preflight
and return-boot evidence is stored separately in
`work/remaining-validation/evidence`.

The actual data-file RAM boot and hardware preflight passed. The preflight
backed up both installed firmware images, left their SHA-256 values
unchanged, and cleanly unmounted data. The router returned to its installed
OS. No hardware `--install` was performed in this session.

Additional stale-temporary-file and post-backup refresh-space guards were
added after hardware preflight and exercised by the disposable-storage
tests. All non-script `/etc` paths are rejected by preparation. The actual
firmware installation path was exercised only against disposable storage,
not the router's NAND volumes.
