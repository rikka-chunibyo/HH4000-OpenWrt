# Native LuCI/UCI LAN configuration

Supersedes the fixed bridge membership in report 25. LAN device names and wired
port lists are now ordinary UCI device sections managed by native netifd. Use
LuCI Network / Interfaces / Devices / br-lan / Configure / Bridge ports, then
Save & Apply. The hardware helper only initializes queues; it never attaches a
port or brings an excluded port back. Wi-Fi uses netifd's prepared bridge name.

The installed netifd revision is 7901e66c. Its modern RTM_NEWLINK bridge creation
caused a vendor-kernel panic. The compatibility patch creates bridges with the
legacy ioctl and configures bridge properties through sysfs. The normal netifd
bridge driver still owns membership, hotplug, reload and deletion. This does not
change the kernel or flash. VLAN-aware bridge filtering remains deliberately
unsupported and returns EOPNOTSUPP before creation; VLAN/offload support must not
be inferred from ordinary bridge support.

Source patch: src/openwrt-hh4000/patches/100-netifd-hh4000-bridge-ioctl.patch.
Rebuild: tools/build_netifd_hh4000.py, then tools/build_openwrt_integrated.py --lab.
Official SDK 24.10.4 and pinned v24.10.4 base-package sources are in work/lan-fix.
Build provenance: work/lan-fix/build-manifest.json. The build checks the compiled
binary for the compatibility implementation: a first SDK attempt regenerated its
source and discarded a local edit, so patches are now installed in the package's
patch directory before clean/compile.

Tests passed on hardware: native bridge creation; removing eth4 with LuCI Save &
Apply; 5/5 wired pings afterward; full service restart keeping eth4 excluded and
recovering all three radios; creating br-lab containing eth4; renaming to br-lab2;
deleting that bridge; restoring eth4 to br-lan. The test used an uncabled port and
kept the laptop on eth3. All five wired ports and three radios were restored.
Serial evidence: work/openwrt-net/live-session.raw. Results: work/lan-fix/results.json.

Lifecycle fixes: hh-services stops wireless devices before killing netifd, so
vendor AP processes do not remain orphaned. ARM32 vendor hostapd wrappers unset
the ARM64 netifd line-buffering preload. Bootstrap initializes reload_config's
checksum baseline so the first LuCI apply emits a config-change event. Earlier
apply behavior included a reset whose exact cause was not captured; after the
baseline and native-management fixes, a continuously monitored apply and a full
service restart passed with watchdog health preserved.

The latest image identity is work/openwrt-integrate/manifest.json. It remains a
RAM image: restarting services preserves committed RAM UCI configuration, while
a device reboot loads its baked-in configuration when RAM-booted again. Default
cold boot remains stock. No rootfs or bootloader installation occurred in this
LAN task. Stock fallback boots during failed experiments may write normal logs.
Previous image and manifest are preserved under work/lan-fix/previous-*.

PON and the optical converter were not configured. WAN routing/NAT and full VLAN
support remain separate tasks. The boot greeting work follows this LAN fix.
