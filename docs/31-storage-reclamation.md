# Reclaiming legacy NAND storage

Current storage layout: see [separate firmware and data partitions](32-separated-firmware-data.md). This document records the earlier layout and audit.

Audit only. Router flash was not written, no legacy UBI was attached, and no boot arguments or firmware were changed. Existing console logs are in work/openwrt-net/live-session.raw; structured findings are in work/storage-reclaim/audit.json.

## Findings

The conf and log partitions reserve 62.125 and 55.5 MiB. Full live SHA256 reads match their all-0xFF backups, which were independently scanned for non-FF bytes. These 117.625 MiB are erased. The data partition reserves 173.5 MiB and contains an old UBI overlay volume. Its extracted backup has eight files totaling 90,877 bytes: factory PSI, NVRAM settings, scratchpad, key/certificate and optical calibration/adjustment files. These remain in the verified full NAND backup. The current raw data-partition hash differs from that old backup; that may reflect UBI metadata or changed files, and has not been resolved by this audit. The eight-file inventory therefore describes the original backup, not a fresh live extraction. Take a fresh snapshot and inspect it before reclaiming this partition. Do not discard those files merely because the partition is reclaimable.

None of these three partitions is mounted or attached to UBI in the installed OpenWrt. The running /opt/conf/scratchpad equals the copy in the read-only base image. The builder explicitly copies that scratchpad into OpenWrt; Wi-Fi uses calibration from the separate boot/calib volume copied into the image. Thus no dependency on the old conf/log/data partitions was found in the active startup path. This is evidence for the current working Ethernet/Wi-Fi/panel stack, not proof of every future PON feature.

A separate 31 MiB filesystem1 volume on main UBI remains unmounted as well. Its backed-up files total about 5.1 MiB, mostly historical logs. Earlier installation notes described it conservatively as retained vendor configuration; current inspection establishes that the active scratchpad comes from the base image instead. Keep this volume for now while legacy optical files and configuration are packaged deliberately; its reclamation is optional and not needed for the main capacity improvement. Keep the boot partition and its permanent_param/calib/bootenv and bootloader volumes.

## Recommended layout

Keep nvram, cfe, boot and main ubi at their existing offsets and sizes. Combine the contiguous old conf/log/data range into one secondary storage partition starting at 0x0dce0000, size 0x12320000 (291.125 MiB). Proposed Linux mtdparts:

`128k(nvram),640k(cfe),20096k(boot),205312k(ubi),-(storage)`

This preserves the main firmware UBI at MTD 3. Its firmware volume offsets and names stay intact. The kernel/bootstrap/rootfs/DTB continue to live there. A new secondary UBI contains the writable settings/packages filesystem, which the bootstrap mounts as /overlay. Normal opkg installs then use the new capacity automatically. It may also contain a separately sized monitoring-data volume, if desired. Keep the original main-UBI owrt-data as a rollback copy until migration and cold-boot tests pass; it can later become additional data storage.

The new 291 MiB partition should provide roughly 250-265 MiB free after UBI reserves and UBIFS overhead; this is an estimate pending actual formatting. Reusing the old approximately 102 MiB writable space afterward gives roughly 350-370 MiB across package/settings and data filesystems, not one merged filesystem. Keep the existing approximately 23 MiB main-UBI update reserve initially. A single expanded main UBI would also be possible, but requires erasing/merging the legacy range into the firmware container and gives less separation between firmware replacement and user storage.

The BSP requires boot identity on UBI 0 or 1. With main UBI at 0, bootstrap must attach boot MTD 2 as UBI 1 BEFORE attaching secondary storage as UBI 2. Automatically attaching storage as UBI 1 would break the existing panel/BSP identity path. Find volumes by name rather than assuming old generated MTD numbers; merging partitions changes those numbers.

## Migration and update behavior

1. Save fresh current bootenv, configuration and board-data archives alongside the verified full NAND backup. Snapshot and extract the current raw data partition as well, resolving its changed raw hash before formatting.
2. Validate the proposed Linux partition map and UBI numbering in a temporary RAM boot. Keep the saved boot path unchanged.
3. From RAM, format only the former conf/log/data range and create a new UBIFS volume using this kernel-compatible format 4. Do not write the main firmware or boot partition during formatting.
4. Copy the existing overlay while it is not in use, preserving ownership, permissions, symlinks, extended attributes and any overlay whiteouts. Verify the copied configuration and SSH keys.
5. RAM-test a bootstrap that mounts the secondary volume as /overlay. Test UCI apply/reload, packages, panel, Ethernet/Wi-Fi and normal reboot. Then install that bootstrap and save its tested Linux boot arguments. Keep the original overlay for rollback until cold-boot checks pass.

The current owrt-data superblock has leb_cnt=max_leb_cnt=970, decoded from offsets 40/44 using the Linux 4.1 UBIFS on-flash definition. Simply enlarging its UBI volume will not expand that filesystem: the max LEB count caps automatic growth. Create a correctly sized replacement and migrate files instead of modifying that geometry in place. Sources: https://raw.githubusercontent.com/torvalds/linux/v4.1/fs/ubifs/ubifs-media.h and https://raw.githubusercontent.com/torvalds/linux/v4.1/fs/ubifs/sb.c .

Currently firmware and settings already occupy different UBI volumes within main MTD 3. Updating only owrt-root, owrt-kernel, owrt-init or owrt-dtb preserves owrt-data. Erasing/reformatting the whole MTD 3 container would destroy them together. Moving the writable filesystem to secondary MTD storage adds a physical erase boundary: firmware-container replacement can leave user storage untouched. The firmware updater must still deliberately preserve that partition.

Across a substantially different firmware version, preserve UCI configuration, SSH keys and intended user data, and reinstall compatible packages rather than blindly carrying every old binary override into the new root. Boot scripts, firewall syntax and kernel modules can change between images. Generic OpenWrt sysupgrade remains unsupported by this custom installation until an HH4000-specific updater is provided.

## Monitoring/firewall tools

More storage helps userspace tools and packages. It does not make upstream OpenWrt kernel modules compatible with the retained vendor 4.1 kernel. Normal 24.10 kmods target a different kernel; the existing kernel lacks nftables, so the modern fw4 stack is not usable as-is. Compatible packet capture, userspace statistics/collectors and an iptables-based firewall are separate integration work. Verify each requested package's dependencies. Sources: https://openwrt.org/faq/cannot_satisfy_dependencies and https://openwrt.org/docs/guide-user/additional-software/extroot_configuration .

Dev-only artifacts from this audit: three storage-reclaim command lists under work/openwrt-net, work/storage-reclaim/audit.json and its backup hash, plus this document. No router NAND writes or reboots were performed.
