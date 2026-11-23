# Persistent OpenWrt installation

Current storage layout: see [separate firmware and data partitions](32-separated-firmware-data.md). This document records the earlier layout and audit.

Install work uses the tested OpenWrt 24.10.4 userspace with the stock 4.1.52 kernel and vendor network drivers. The root is a read-only XZ SquashFS, with a writable UBIFS overlay for settings, packages and ordinary files. A small initramfs mounts these and switches to the system root. The independent watchdog feeder remains active across the handoff.

Main UBI (Linux ubi0, MTD 3) installation layout:

| ID | Name | Purpose |
| --- | --- | --- |
| 0 | filesystem1 | Retained vendor configuration used by the existing driver stack |
| 6 | owrt-root | Compressed OpenWrt system |
| 7 | owrt-kernel | Tested legacy kernel uImage |
| 8 | owrt-init | Storage bootstrap initramfs |
| 9 | owrt-dtb | Tested board DTB |
| 10 | owrt-data | Writable UBIFS overlay |

The original operational and rescue OS volumes (IDs 4 and 5) were reclaimed after a successful stored-image boot. Bootloaders, bootloader rescue, board identity and calibration remain on the separate boot UBI partition. Raw conf/log/data partitions are not reformatted. The original complete NAND archive passed every saved SHA256 check before reclamation; results are in work/openwrt-install/backup-verification.json.

Build on dev: `python3 tools/build_openwrt_persistent.py`. Source boot scripts: src/openwrt-hh4000/persistent. The builder creates root.squashfs, bootstrap.uImage and ramtest.uImage under work/openwrt-install. The latter contains the same system in RAM with a temporary overlay. Files and readback hashes are recorded in staging.json and manifest.json.

Development remains available through `tools/openwrt_install_live.py ramtest` on Windows using the existing TFTP staging. This explicitly uses hh.persist=0 and does not mount the installed writable overlay. Power cycling returns to the saved boot command. Development boot arguments omit the stock image_ubivol/rootfs mappings; those mappings otherwise keep the old OS volume busy even from an initramfs.

The installed boot script sets hh.persist=1 and reads kernel, bootstrap and DTB from their UBI volumes. It needs no laptop, TFTP server or Arduino intervention to boot. Saved environment readback and CRC are checked by the installation helper. A serial recovery prompt remains available by interrupting U-Boot and loading the RAM image.

This is a custom hybrid installation. Use the project installer for system-image changes; a generic OpenWrt sysupgrade image is not an HH4000 firmware image. All five Ethernet ports default to LAN, with the three lab APs retained. WAN routing/firewall/NAT acceleration has not been configured or validated. Loaded RDPA/Runner/flow-cache modules alone do not prove hardware NAT offload; validation requires actual NAT traffic plus hardware flow/counter evidence.

Verified unattended cold boots from NAND before and after the reboot compatibility fix. A LuCI-applied panel title, an ordinary file and the SSH host keys survived a power cycle. The default panel title was restored and the test file removed. A subsequent normal ubus/LuCI reboot also returned to the installed system. Final checks found eight LAN bridge members, all three radios up, healthy panel state, authenticated LuCI HTTP 200 and the Dropbear SSH banner. The UBIFS overlay reports 106.7 MiB total and about 102 MiB available. MTD 3 ECC failures, corrected bits and bad blocks were all zero.

The first normal reboot test exposed that procd, running below the independent watchdog/console PID 1, never loaded its inittab shutdown action. The small patch in src/openwrt-hh4000/patches/110-procd-hh4000-nonpid1-shutdown.patch loads that action in service-manager mode. It uses the normal rcS shutdown sequence and reboot path. Build with tools/build_procd_hh4000.py using the pinned OpenWrt SDK (procd revision 42d3937654508b04da64969f9d764ac2ec411904). The fix was booted and tested in RAM, initially checked as a persistent binary override, then incorporated into the read-only SquashFS base. That base volume was updated from the RAM system while the installed root was unmounted and its complete SHA1 readback verified. The temporary binary override was removed with the data overlay unmounted. The fix is also included in the source overlay for future images. The bootloaders were not rewritten for this fix. Shutdown still prints messages for unused service entries and busy virtual mounts; UBIFS journal recovery completes on the next boot.

The originally flashed system image set and manifest are preserved under work/openwrt-install/initial-image-set. The bootstrap, kernel and DTB stored in UBI still match that original installation's staging hashes. The current root matches root-update.json and the root entry in the newer manifest.json. installed-manifest.json records the complete current stored image set. The same newer RAM/development image incorporates the procd fix. Final checks compare /rom/sbin/procd to /sbin/procd and require that no writable procd override remains. Factory reset itself has not been tested.

The old hh_stock_bootcmd environment reference is not a usable stock-OS fallback after removal of operational/rescue. Recovery uses the retained U-Boot/rescue bootloader and the project RAM image, or restoring the verified backup.

Live acceleration inspection found the vendor RDPA, pktflow and pktrunner modules and /proc/fcache. FHW Flow Activation Success was zero and the hardware-flow bitmap was empty; the observed local management TCP flows are explicitly excluded from hardware acceleration. This does not test NAT support. WAN/firewall integration followed by routed traffic and before/after hardware counters is required before claiming hardware NAT acceleration. The standard OpenWrt nftables firewall is not supported by this retained 4.1 kernel, so generic modern OpenWrt feature availability should not be assumed.

Final results: work/openwrt-install/installation-results.json. Full UART evidence: live-session.raw, storage-session.raw and reboot-session.raw there, plus work/openwrt-net/live-session.raw. Dev artifacts added or changed for installation are the persistent source scripts, builders and live/test helpers under tools, source overlay SSH startup handling, procd patch/binary, generated images and manifests under work/openwrt-install and work/procd-fix, and this document. Router NAND writes include the OpenWrt UBI volumes, saved boot environment and UBIFS settings; the reboot fix is in the base SquashFS; stock OS volumes 4 and 5 were removed.

Storage follow-up: [legacy partition reclamation audit](31-storage-reclamation.md) verifies that the active scratchpad is embedded in the base image; filesystem1 was retained conservatively and is not mounted by the current system. Firmware and settings already use distinct UBI volumes.
