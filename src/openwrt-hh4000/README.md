# HH4000 stock-kernel OpenWrt integration

See docs/26-lan-configuration.md for current LAN behavior. Native netifd owns
bridge creation, deletion, names and memberships from ordinary UCI device
sections. LuCI Network / Interfaces / Devices is the intended editor. The
board helper initializes vendor hardware queues only and never adds ports.

The daemon matches installed revision 7901e66c with one bridge compatibility
patch under patches/. It uses bridge ioctls and sysfs because the stock 4.1
kernel panics on modern nested bridge creation attributes. VLAN-aware filtering
is deliberately rejected with EOPNOTSUPP until separately validated. Plain
bridges and independent 802.1q devices are different features; neither implies
that all advanced VLAN/offload configurations have been tested.

Build daemon: python3 tools/build_netifd_hh4000.py (requires the prepared official
OpenWrt 24.10.4 SDK under work/lan-fix/sdk; toolchain and patch hashes are recorded).
Build RAM image: python3 tools/build_openwrt_integrated.py --lab.
Boot: python -X utf8 tools/openwrt_net_live.py boot-integrated.
The historical RAM tests ran without installing a root filesystem to flash.

The pinned GPL-2.0 netifd shell library comes from
https://github.com/openwrt/netifd/blob/7901e66c/scripts/netifd-wireless.sh .
## Local build inputs

The build scripts use the existing HH4000 staging layout. Prepare an
OpenWrt root filesystem and supply the compatible vendor runtime and
calibration from a backup of your own device. The network rootfs builder
reads the extracted vendor filesystem from work/rootfs, device calibration
from work/ubi/calib, and the other inputs named in the script. These are
local files and can contain sensitive information.

The [binary input list](local-binaries.txt) describes files required by a
full integrated or persistent image. Paths are relative to the platform
overlay directory. Use matching outputs from the package and component
builders where available. Supply stock runtime files from your own
firmware backup. You can put these inputs at the listed paths below the
overlay's files directory, or use a prepared root filesystem that already
contains the same components at the matching runtime paths. The input
check verifies presence and ELF format. Use the ABI and versions described
in the platform research when selecting the files.

Run the build commands from the checkout root:

- Stage the OpenWrt root filesystem with tools/build_openwrt_stage1.py
  and tools/build_openwrt_net.py.
- Build the integrated image with tools/build_openwrt_integrated.py.
  Use the prepared root filesystem for tools/build_openwrt_persistent.py.
- The default wireless configuration keeps the radios disabled. If you
  need a local preset, put it in work/openwrt-integrate/wireless.lab.

See the [maintenance documentation](../../docs/43-data-preserving-firmware-updates.md)
for image preparation and verification. Full image assembly requires local
inputs, including the ARM32 toolchain settings and vendor runtime. The
PON host tests can run independently with make in src/hh-pon.
