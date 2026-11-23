# Everyday-use startup and network validation

## Changes

The vendor hardware/bootstrap path is retained, but it now runs enabled `/etc/rc.d/S*` services in normal order. This includes package-installed services rather than a fixed startup list. The normal network, rpcd, uhttpd, system, log, odhcpd and entropy services are used. The front panel has normal enable/disable links. `hh-services` remains a compatibility entry point that calls the standard services; it no longer contains a fixed web address.

The adapted boot/done scripts prepare the already-mounted filesystem and run `/etc/rc.local`, avoiding generic root mounting, generic 6.6 module loading and board regeneration. Each service startup result and output is recorded under `/tmp/hh-services-boot.tsv` and `/tmp/hh-boot-*.log`; failed commands are logged. The exact vendor hardware initialization and independent watchdog feeder remain in place. This is still a custom bootstrap with procd in service mode, not a complete upstream platform port.

Hostname/timezone use the normal system service and UCI triggers. Management uses normal `/etc/config/uhttpd` listeners, supporting LAN address changes and HTTP/HTTPS. WAN filtering controls access to those listeners. HTTPS uses a locally generated self-signed certificate. Logging is enabled, with runtime logs in RAM.

IPv6 defaults now include a stable ULA prefix, LAN /64 assignment, router advertisements, DHCPv6 and a DHCPv6 WAN interface tracking `@wan`. The WAN firewall zone includes both protocols. `sourcefilter=0` selects ordinary IPv6 default routes because source-specific default routes did not install on this vendor kernel. WAN addresses and routes must come from the real upstream network; there is no NAT66 or synthetic public LAN prefix.

Kernel and locally patched platform packages (`kernel`, `netifd`, `procd`, `base-files`, `busybox`, `dnsmasq`, `firewall`) are held to prevent ordinary upgrades overwriting integration/ABI fixes. Userspace installation remains available. Generic kernel modules and generic sysupgrade remain unsupported; use the documented project update/recovery procedures.

## Tests completed in RAM

* A fresh complete image boot started all enabled scripts successfully. The no-RTC sysfixtime return code was fixed.
* UCI apply/confirm changed LAN `192.168.1.1` to `192.168.1.2` and hostname to a test value, verified management at the new address and the live hostname, then restored both.
* Wired router-local transfers reached about 948 Mb/s in both directions on eth3. These are LAN endpoint measurements, not a maximum routed-NAT benchmark.
* The spare 2.4GHz USB adapter completed WPA2 association, DHCP, bidirectional transfers, routed HTTP and three disconnect/reconnect cycles. Primary Wi-Fi was preserved. This adapter cannot validate 5GHz clients; both 5GHz radio states were up but need real client tests.
* Router IPv6 internet ping passed. The laptop received a LAN ULA through RA; IPv6 DNS and LAN SSH/HTTP/HTTPS worked. WAN IPv6 SSH/HTTP/HTTPS/DNS connections were rejected. The upstream supplied IPv6 addresses/default route but no delegated prefix; public LAN IPv6 internet remains dependent on upstream PD.
* IPv4 DHCP/DNS, LAN SSH/HTTP/HTTPS and WAN management filtering passed after a clean build and boot.
* Normal signed opkg feeds updated successfully. `tcpdump-mini`, `htop`, `libpcap1`, `libncurses6` and `terminfo` installed and their binaries ran. A real DNS request/reply was captured and decoded by tcpdump. The vendor RAM filesystem reports zero free capacity, so only the RAM package test used `--force-space`; persistent UBIFS installation must use normal capacity checks.
* Ordinary Windows TCP sockets traversed a scoped temporary LAN redirect and WAN NAT for five validated TLS sessions, totaling 20MB downloaded and 5MB uploaded with hardware learning enabled. An earlier software-control run downloaded another 20MB. The redirect is needed because the laptop's wired static configuration has no gateway, and using its own primary Wi-Fi address as a normal destination would otherwise shortcut the router. These complex redirect flows are not claimed to be hardware offloaded.
* Native Windows UDP sockets traversed that same physical router path for 3,367/3,367 replies, independently of packet injection/capture.
* The corrected direct hardware-NAT harness returned 1,590/1,590 datagrams at the higher rate. During a controlled WAN outage, it received 3,366/4,085 replies, including the final sequence 4,084. The loss coincided with the deliberate outage; after DHCP/link recovery both directions again reported `hwaccel=2` and Runner hardware hits.

### Packet-harness correction

Earlier high-rate tests using raw Wi-Fi injection for the echo response stalled even without an outage and with acceleration disabled. They do not establish a router recovery bug. Replacing the responder with a connected native UDP socket removed the apparent stall. Windows capture still missed some WAN observations while the native server and LAN reply capture counted every packet. The replacement measures native-server receipt independently and requires replies at the end of recovery tests; the old `mixed` assertion alone could pass an early burst followed by permanent loss. Historical failed reports are retained as diagnostics, and are superseded by `udp-native-echo-fixed.json` and `udp-readiness-recovered.json` under `work/wan-firewall/`.

## Persistence and artifacts

The new changes are a targeted data-overlay update, validated in RAM first. The installer archives the complete original upper directory to `work/readiness/overlay-before.tar.gz`, records hashes, changes only the deployment manifest entries, verifies untouched settings and keys, and remounts the data filesystem for readback. It writes UBI2 `owrt-data`; bootloaders, environment, kernel, DTB and the read-only root image are not rewritten.

Source changes: `src/openwrt-hh4000/files/{etc,usr}/`, enabled front-panel links and `tools/prepare_firewall_metadata.py`. Build and install tools: `tools/build_openwrt_persistent.py`, `tools/install_readiness_overlay.py`. New transfer, configuration, native UDP/TCP and IPv6 tests are under `tools/`. Build, deployment manifest, backups, logs and final results are under `work/readiness/`; packet-harness reports retain the `work/wan-firewall/` location.

The prior tested setup can be restored from RAM using the archive and deployment manifest, preserving any newer user settings first. Replacing the firmware image must retain the separate data volume. Generic sysupgrade/factory-reset behavior is not validated for this custom layout. See [32-separated-firmware-data.md](32-separated-firmware-data.md) and [34-wan-firewall-hardware-nat.md](34-wan-firewall-hardware-nat.md).

## Before the household trial

Keep eth4 as Ethernet WAN and eth0-eth3/Wi-Fi as LAN unless deliberately reconfigured. Set a root password and personal Wi-Fi names/keys in LuCI; the lab credentials were retained for testing. The runtime panel is configurable at System -> Front Panel. Watch System Log/Kernel Log and use `htop`, `tcpdump` and `fcctl status` when diagnosing behavior; packet capture and software counters do not necessarily include hardware-offloaded packets.

Remaining limits: real 5GHz client/throughput tests, full 10G routed throughput, public LAN IPv6 without upstream PD, VLAN/PPPoE routed validation, guest isolation, VPN/SQM and their interaction with acceleration, and longer household use. PON remains deferred. A sustained lab session does not replace testing across normal clients, devices and upstream outages.

## Persistent validation

The saved update passed cold boot without TFTP and a normal LuCI reboot. A temporary enabled procd service started automatically on reboot, and a temporary saved hostname was loaded by the standard system service; the probe was removed and the default hostname restored afterward. Normal persistent `opkg install tcpdump-mini htop` succeeded without forcing capacity checks. Both binaries survived reboot, along with the preserved panel settings, calibration and SSH keys. MTD3/MTD4 ECC failure counters remained zero. The installed direct hardware-NAT run returned all **1,569/1,569** packets; both Runner directions had 1,567 hardware hits at capture. The installed USB Wi-Fi test again passed routed access, transfers and three reconnects. HTTP/HTTPS/SSH/DNS filtering passed for IPv4 and IPv6. Data had about **338.8MiB free** after reboot. Temporary forwarding rules, test service, transfer server and USB Wi-Fi profile were removed; private backup staging copies were removed from the laptop TFTP directory after matching them to retained dev archives. A final clean cold boot verified the restored hostname, correct DHCP hostname, IPv4/IPv6 WAN up, every enabled service returning zero, absence of the probe/transfer server, and zero ECC failures. Final machine-readable results are in `work/readiness/final-results.json`.
