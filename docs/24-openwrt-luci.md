# OpenWrt LuCI Web UI on the HH4000 RAM image

Current state: OpenWrt is now installed persistently. See [the installation and verification record](30-installed-openwrt.md). The RAM tests below remain historical evidence.

## Result

Test date: 2026-11-08 local / 2026-11-09 UTC. LuCI from the existing official
OpenWrt 24.10.4 ARM64 rootfs runs on the HH4000 with the stock 4.1.52-5.02L.07
kernel and recovered vendor Ethernet modules. No additional packages were needed.

Access from the existing wired laptop connection:

- URL: http://192.168.1.1/cgi-bin/luci/
- Username: root
- Password: use the credentials set during local setup.
- Laptop address: 192.168.1.10; router LAN: eth3, 192.168.1.1/24.

Confirmed in the shared browser: successful login; live system overview with
kernel, OpenWrt version, uptime and memory; Network / Interfaces showing LAN,
link connectivity, address and traffic counters; the LAN configuration editor
opens with the current settings. The editor was dismissed without changes.
Configuration Save & Apply, firewall, Wi-Fi configuration, firmware installation,
package installation and service restart through LuCI have not been validated.

This is still a hybrid RAM boot, not a completed HH4000 OpenWrt target. The rootfs
reports the generic armsr/armv8 target. The model and hostname are set explicitly
in RAM. Zero-sized storage bars reflect the initramfs setup and do not describe
available persistent storage. Clock initialization/NTP is not configured.

## Services required

The rescue BusyBox /init remains PID 1 and continues feeding the watchdog.
Start ubusd, procd (as a non-PID-1 RPC service), rpcd, netifd and uhttpd manually.

Without procd, the bootstrap theme fails rendering its login page because its
system.board call returns null. The ucode exception points at runtime.uc's
rethrow, rather than the missing service. Starting procd fixes this by registering
system and service objects. Source inspection of work/os/owrt/procd/procd.c
confirms non-PID-1 mode connects to ubus without entering the PID-1 boot state
machine or executing the normal rcS sequence.

netifd provides network, network.interface and LAN status objects. Minimal UCI
network/system files are needed because the usual first-boot configuration has
not run. Configure only loopback and the known-working eth3 port, and initialize
its vendor traffic queues as documented in report 23. For this isolated test,
netifd uses -h /bin/true so generic hotplug scripts do not attempt to start the
unvalidated firewall. There is no WAN/NAT service or firewall in this test.

uhttpd binds only 192.168.1.1:80, serves /www and /cgi-bin, and exposes /ubus using
the existing uhttpd ubus module. LuCI runs through its ucode CGI executable.
The old kernel and vendor DHD Wi-Fi stack still need OpenWrt service integration;
the standard Wireless and LED pages are not hardware adapters for these devices.

## Reproduce from a fresh RAM boot

On dev, from the project root:

```sh
python3 tools/prepare_openwrt_luci.py
```

On Windows, using the existing TFTP staging and Arduino COM3 connection:

```powershell
python -X utf8 .\tools\openwrt_net_live.py boot
python -X utf8 .\tools\openwrt_net_live.py script luci-start.json
```

Run the recipe once after a fresh RAM boot. It is not a daemon restart script.
The service sequence was repeated after a fresh RAM boot and LuCI was reachable
again. During this check the Ethernet success-marker path was corrected to
/tmp/hh-bdmf-init.log. It targets the tested openwrt-net image from report 23, whose older Ethernet
helper returns the BDMF session ID as a nonzero exit status. The recipe checks
for the session marker then explicitly initializes eth3 queues. The builder's
newer Ethernet helper source already handles these details, but its rebuilt
image has not yet been tested.

Keep each UART shell command short: a previous long command was silently
truncated before its status marker. The prepared recipe enforces a conservative
350-byte command limit. It also creates /var/lock before service activity.

Artifacts: tools/prepare_openwrt_luci.py; work/openwrt-net/luci-start.json;
serial evidence
appended to work/openwrt-net/live-session.raw. No kernel, initrd, DTB, U-Boot,
secondaryboot, stock filesystem or calibration flash was written for this task.

## Next integration work

1. Put the proven vendor driver setup into a controlled OpenWrt startup sequence,
   preserving watchdog ownership, and validate configuration reloads.
2. Add a netifd/UCI adapter for the vendor Wi-Fi tools and hostapd, so LuCI can
   control the working radio stack. Test 5 GHz with a capable client.
3. Validate LAN bridging, DHCP and the firewall/NAT approach supported by the
   stock kernel, then choose a persistent OpenWrt rootfs layout.
4. Connect the recovered display/buttons/LED toolkit to a status/menu service.
