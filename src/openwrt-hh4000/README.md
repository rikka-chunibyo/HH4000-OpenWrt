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
For the current RAM and persistent images, use the
[installation guide](../../INSTALL.md). It links the compatible boot and
data-preserving maintenance procedures.

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

The stage1, network and integrated scripts are intermediate images. If
starting from an existing prepared rootfs, assemble the supported candidate
with tools/build_openwrt_persistent.py as described in the installation
guide. Both the integrated and persistent image builders compile the panel
and reset daemons and the SKIPLOG userspace extension. For a private build
with a Wi-Fi preset, pass --wireless-profile with the file path to the
persistent builder. The integrated builder uses --lab with the local
work/openwrt-integrate/wireless.lab file.

The ARM32 JSON file at work/display/live/build-settings.json supplies the
compiler and two argument arrays. Use the ARM GNU EABI compiler with
ARMv7-A soft-float flags and the matching vendor glibc and startup files.
Put architecture, include and compile flags in base_arguments. Put linker
flags, startup files and libraries appropriate to your toolchain in
tail_arguments. The daemons run through the vendor glibc loader. The
separate SKIPLOG extension needs the ARM64 musl SDK and the netfilter
header archive listed in the installation guide.

The PON host tests can run independently with make in src/hh-pon.
