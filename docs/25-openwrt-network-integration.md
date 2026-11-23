# OpenWrt network integration: five Ethernet ports and three radios

Current state: OpenWrt is now installed persistently. See [the installation and verification record](30-installed-openwrt.md). The RAM tests below remain historical evidence.

## Hardware inventory

Live ethctl phy-map confirms eth0, eth1, eth2 and eth3 are the four on-chip 1G
EGPHY ports. eth4 is the external 10G PHY (84891 B0 firmware loaded with good CRC).
Its advertised modes include 10G, 5G, 2.5G, 1G and 100M. The cabled eth3 negotiates
1000 Mbps full duplex and passes traffic. The remaining four ports have no cable;
registering and configuring their PHYs/queues is not an end-to-end traffic test.

All three calibrated BCM43684 radios register. The integration starts one secured
AP on each: wl0 / phy0 at channel 6 (2437 MHz), wl1 / phy1 at channel 149 (5745 MHz),
and wl2 / phy2 at channel 36 (5180 MHz). Vendor hostapd control status reports all
three ENABLED concurrently, and wlctl confirms the actual channels. OpenWrt iwinfo
sees all three interfaces; its old-kernel capability presentation remains limited.
Only wl0 has been tested with an actual client: the USB RTL8188EU is 2.4 GHz only.
No 5 GHz client association, Wi-Fi 6 performance, or 10G throughput is claimed.

PON's kernel dependency is present as part of RDPA initialization, but there is no
PON netdev/service, OMCI or subscriber registration configured. PON remains deferred.

## Added platform integration

Source: src/openwrt-hh4000/files. Builder: tools/build_openwrt_integrated.py.

- The RAM init starts the existing watchdog feeder, then automatic networking.
- The recovered driver setup loads the Ethernet and three-radio Wi-Fi bundle.
- hh-bridge-init creates br-lan and initializes queues for all five Ethernet ports.
- A bcmdhd netifd driver consumes ordinary wifi-device/wifi-iface UCI sections and
  supervises the recovered vendor hostapd instances. It publishes their state and
  interfaces through network.wireless. `/sbin/wifi` supplies up/down/status/reload.
- procd supervises hh-netifd, rpcd, uhttpd and dnsmasq as hh-services instances.
- LAN address is 192.168.1.1/24 on br-lan. DHCP provides 192.168.1.100-150 without
  gateway or DNS options; the laptop's primary Wi-Fi stays responsible for internet.
- uhttpd exposes the existing LuCI interface on the wired/wireless LAN address.
  LuCI's feature RPC now reports wifi=true and its wireless RPC sees the radios.

The current backend intentionally accepts WPA2-PSK AP mode, one BSS per radio,
stock CA settings, and fixed non-DFS 20 MHz channels. Wider modes, WPA3, guest BSSs,
station mode, arbitrary network assignment and power control still need validation.
It does not silently accept an unvalidated width or country. Stock calibration
and private ARM32 tools are reused without persistent nvram commits.

## Compatibility findings

A direct netifd bridge configuration caused a captured kernel panic in
validate_nla / rtnl_newlink, at lib/nlattr.c:44. The same failure happened with
Wi-Fi disabled, so it was isolated from the radios. Evidence is saved separately
as work/openwrt-integrate/bridge-netlink-panic.txt. The stock fallback boot remained
usable after these RAM test failures.

The working path uses brctl's older ioctl API, after netifd registers the external
LAN device. AP startup waits for the platform-owned bridge. This order matters:
if hostapd creates the bridge itself, its teardown deletes that bridge and all
wired memberships. The platform marker/barrier prevents AP ownership.

The driver's UCI defaults set reconf=0: the vendor process cannot perform netifd's
default in-place reconfiguration. Proper stop/start fixed reload failures with
stale AP processes. serialize=1 prevents concurrent setup handlers. AP readiness
uses vendor-hostapd_cli STATUS, not buffered log output. The original requested
channel-6/actual-channel-1 mismatch is fixed by configuring the radio through
wlctl before hostapd starts.

One immediate automatic startup reset showed a Dying Gasp message, without a
captured kernel panic. Its cause is not established. Loading drivers after a
15-second settling delay matches the previously proven manual sequence. Two
subsequent automatic boots reached the network services; this is not a long-term
stability test or proof of the reset's underlying cause.

## Client validation

The integrated image provided the USB WPA2 client 192.168.1.125 via DHCP. Hostapd
reported AUTH, ASSOC and AUTHORIZED with WPA2/CCMP. With eth3 administratively down,
five source-bound pings passed with zero loss (2-4 ms). A 4 MiB HTTP download and
4 MiB HTTP POST upload both matched SHA256:

f70713f1371de1fe0aac29348362307400a63fb2f12dd9f15a82a5947647b46b

The temporary CGI receiver was removed afterward, then eth3 restored. This proves
wireless traffic reached the shared bridge's LAN address without the wired path.
It does not prove forwarding between two independent external bridge clients.
The laptop primary Wi-Fi remained associated to its existing network.

## Reproduction and scope

Build on dev, stage the resulting image in the Windows TFTP root, and use:

```sh
python3 tools/build_openwrt_integrated.py --lab
```

```powershell
python -X utf8 .\tools\openwrt_net_live.py boot-integrated
```

The --lab build uses work/openwrt-integrate/wireless.lab. Source defaults disable
all radios until a suitable key is configured. The image fits the existing 16 MiB
staging slot; its exact size and SHA256 are in work/openwrt-integrate/manifest.json.
The earlier known-good network image and its manifest are preserved.

Changes are platform overlay source, the new image builder, the UART helper's
additional boot-integrated mode, generated RAM images/recipes, evidence and docs.
No bootloader, firmware volume or calibration partition was flashed during this
work. Test resets booted stock normally, which can perform its ordinary log and
configuration writes. The final custom OS remains in RAM; normal reboot uses stock.

There is no WAN/NAT/firewall integration yet. The installed firewall4 package and
its LuCI feature flag do not establish that nftables works on this 4.1 kernel.
Do not create a generic LuCI/netifd bridge until its netlink compatibility is fixed.
Next useful external tests are a cable on each unused port (including a 10G peer)
and a 5 GHz client, followed by routing/firewall work. PON remains out of scope.

## Final recorded result

Final image: 16,318,891 bytes, SHA256
`d9b238d91204010025c8213b1602e767603b67b3274684dccfabf187af09ab32`.
A fresh automatic boot and subsequent hh-services configuration reload retained
all eight bridge members (five Ethernet plus three Wi-Fi), with all three APs UP
and wired ping passing 3/3. Bridge MAC is the board Ethernet MAC, confirming the
platform creates it before hostapd. Four procd instances are running, and LuCI's
getWirelessDevices RPC returns radio0, radio1 and radio2. The watchdog remained
healthy. Sanitized JSON/state files are in work/openwrt-integrate/evidence;
machine-readable results are in work/openwrt-integrate/hardware-results.json.
The temporary USB profile and private profile XML were removed. The primary
laptop adapter remained connected. No browser rendering of the new Wireless page
was checked in this task; its RPC data and feature detection were verified.

Current WAN/firewall follow-up: [34-wan-firewall-hardware-nat.md](34-wan-firewall-hardware-nat.md) records the installed configurable eth4 WAN, firewall3 and verified TCP/UDP hardware NAT.
