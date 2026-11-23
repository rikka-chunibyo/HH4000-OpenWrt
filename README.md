# HH4000-OpenWRT

OpenWrt integration and firmware research for the Bell Home Hub 4000
(Sagemcom FAST5689, BCM6858). The tested configuration pairs OpenWrt
userspace with the device's vendor kernel and hardware drivers.

- [Platform support](src/openwrt-hh4000/README.md) covers Ethernet, Wi-Fi,
  the front panel, the reset button, VPN support, and SQM.
- [PON experiments](src/hh-pon/README.md) include the protocol core and
  host tests.
- The Arduino directory contains the UART bridge and relay controller.
- The [research index](docs/README.md) links hardware documentation and
  reports from the lab.

To build a complete image, prepare the local inputs described in the
[platform build notes](src/openwrt-hh4000/README.md#local-build-inputs).
Use a RAM test image to validate the resulting firmware on your board
before an installation. For an existing installation, see the
[data-preserving maintenance procedure](docs/43-data-preserving-firmware-updates.md).

Source files and patches retain their existing license and attribution
notices. The text for GPL-2.0-only files is in
[LICENSES](LICENSES/GPL-2.0-only.txt).
