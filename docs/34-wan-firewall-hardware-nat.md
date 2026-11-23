# Configurable Ethernet WAN, firewall3 and hardware NAT

## Default wiring and configuration

The 10G copper port, `eth4`, is the default DHCP WAN. During validation it negotiated 2.5G with the upstream router and received `192.0.2.207/24` through gateway `192.0.2.1`. The laptop is on 1G `eth3`. LAN is `192.168.1.1/24`; `br-lan` contains `eth0` through `eth3` and the three Wi-Fi interfaces. PON remains deferred.

Use LuCI **Network -> Interfaces -> WAN** to choose a different device or protocol, and **Network -> Devices -> br-lan** to remove any port assigned to WAN from the LAN bridge. Do not assign a physical port to both networks. Existing LuCI wireless and front-panel controls remain available.

The Broadcom driver selects its physical WAN role when loaded. `/usr/libexec/hh-wan-select` reads the UCI WAN device before Ethernet initialization, copies board configuration to RAM, changes only the `WanOEMac` token in that RAM copy, and bind mounts the copy. **Reboot after changing the physical WAN port** to align hardware acceleration with the new port. The original board scratchpad is preserved. Selection accepts `eth0` through `eth4`, optionally with a VLAN suffix; VLAN acceleration itself has not been tested. A reversible UCI test selected uncabled `eth0`, verified netifd's device selection, then restored `eth4`. Routed traffic was physically tested only with `eth3` LAN and `eth4` WAN.

## Firewall and services

OpenWrt 24.10.4 userspace uses the official legacy `firewall` package (firewall3) and iptables 1.8.10, with retained stock 4.1.52 kernel modules and a matching `xt_comment.ko`. There is no kernel upgrade or nftables backport here. Firewall3 manages IPv4 NAT and IPv4/IPv6 filtering from normal `/etc/config/firewall` settings, including LuCI zones, rules and redirects.

Startup loads the matching netfilter modules, sets INPUT/FORWARD to DROP, starts netifd and installs the firewall before enabling IPv4 forwarding. Only firewall3 owns the eventual allow rules; there are no unconditional extra LAN allowances ahead of LuCI's policy. Standard interface hotplug runs `/sbin/hotplug-call` so network changes refresh services.

The normal dnsmasq service reads `/etc/config/dhcp` and netifd's WAN resolver file. DHCP supplied a LAN lease, router `192.168.1.1` and DNS `192.168.1.1`; DNS forwarding resolved external names. Standard sysntpd starts from the system NTP configuration and synchronized the clock. Both init scripts avoid requesting unavailable PID/IPC/UTS namespaces on the retained vendor kernel. Firewall, dnsmasq, Dropbear and sysntpd startup honor their normal enabled-service links.

Tests accepted LAN SSH/web access and rejected WAN SSH/web/DNS access. A temporary UDP redirect added with UCI apply/confirm admitted traffic to a pinned LAN responder; deleting it blocked a new flow while the responder remained active. The temporary redirect was removed.

Optional `ipset` and Linux `FLOWOFFLOAD` facilities are unavailable in this setup; their firewall3 startup warnings do not describe Broadcom Runner acceleration, which operates separately. Kernel-backed packages need modules built for the retained vendor ABI. Package metadata now identifies the real 4.1.52 kernel and its verified netfilter providers, rather than falsely advertising the generic OpenWrt 6.6 kernel/modules. Ordinary architecture-compatible userspace packages can still be installed, subject to their dependencies. Installing arbitrary generic OpenWrt kmods remains unsupported.

## Hardware acceleration evidence and limits

`fcctl status` reports L3 mode, flow learning enabled and hardware acceleration enabled. A pinned raw Ethernet client ensures test traffic actually traverses the router instead of taking the Windows laptop's other connection.

* UDP: source translation to `192.0.2.207`, correct reverse translation and one-hop TTL decrement were observed at both interfaces. The final RAM run returned all 118 of 118 test datagrams. An earlier correctness run returned all 245 of 245. The matching conntrack record reported `swaccel=2 hwaccel=2`, and forward/reverse Runner hardware entries had incrementing hit counters.
* TCP: a complete handshake and HTTP 301 response from `1.1.1.1:80` passed through NAT, with repeated requests and no packet-harness errors. The matching established, assured conntrack entry reported `swaccel=2 hwaccel=2`; both directions had Runner hardware tuple IDs and hardware hits.
* Policy invalidation: an existing cached UDP flow initially bypassed a newly inserted scoped software DROP rule. After `fcctl flush`, the rule counted and blocked subsequent packets. The `/sbin/fw3` wrapper now flushes Broadcom's flow cache after firewall start/stop/restart/reload/flush and propagates a flush failure.

This verifies IPv4 TCP and UDP hardware NAT on the tested Ethernet path. It is not a line-rate throughput measurement or proof for every protocol, PPPoE, VLAN arrangement, IPv6 forwarding or Wi-Fi routed acceleration. Existing conntrack state still follows normal firewall ESTABLISHED/RELATED semantics: removing an accelerator entry does not by itself delete a conntrack connection. Some policy changes intentionally preserve established sessions; explicitly clear connection state when immediate session termination is required.

## Installation and recovery

The complete candidate was built and booted in RAM before persistence. The installed update consists of changed platform files in **UBI2 `owrt-data`**, leaving the bootloaders, boot environment, stock kernel, device tree and stored read-only root image unchanged. The earlier separated 96MiB firmware/data partition layout remains in place. Source images built afterward include the same integration by default.

`tools/install_wan_overlay.py` saves and verifies the original upper-directory archive, preserves panel settings, calibration and both SSH host keys, verifies every deployed file or symlink, and remounts the data filesystem before its final readback. A whiteout hides the old `/sbin/fw4` entry in the lower image. Installation inputs and evidence are in `work/wan-firewall/`:

* `overlay-before.tar.gz`, `overlay-before-hashes.json`: recoverable pre-update settings.
* `deploy.tar.gz`, `deploy-files.json`, `deploy-manifest.json`: exact update and hashes.
* `overlay-installed.json`: installation readback and preserved-file results.
* `image/manifest.json`: final source image hashes; the only change after the final RAM boot was a package metadata hold-flag correction verified on the installed system. `ram-tested-image.json` identifies the image actually transferred for that RAM boot.
* `lan-services.json`, `uci-port-forward.json`, `uci-wan-configuration.json`, `udp-*.json`, `tcp-nat.json`: packet and configuration tests.
* `final-results.json`: completed installed-system validation results.

Recovery can use the RAM bootstrap to mount UBI2 independently, save any newly created user settings, remove the deployment's upper-directory entries and fw4 whiteout, then restore the pre-update archive. Do not replace or erase the whole data partition to roll back these files. Future read-only firmware updates should retain the data volume, as described in [32-separated-firmware-data.md](32-separated-firmware-data.md).

## Development files

Changes are under `src/openwrt-hh4000/files/` (legacy firewall packages, matching module, vendor tools, startup, DHCP/NTP guards, WAN selection and defaults), `src/openwrt-hh4000/wan/` (scratchpad helper), `src/openwrt-hh4000/patches/` (service compatibility patches), and `tools/` (builder, metadata, serial runner, installer and packet tests). `work/wan-firewall/` contains downloaded checksum-verified packages, extracted bundles, builds, backups and results. No global Windows Python installation, primary Wi-Fi configuration or Windows routing/firewall settings were changed; packet tests use a temporary local Scapy extraction and the existing Npcap installation.

## Final installed validation

The installed setup passed an unattended cold boot from NAND and a normal LuCI `system.reboot` operation, with no TFTP or relay assistance for the software reboot. LuCI and the front-panel service returned automatically. WAN DHCP, all seven LAN members, NTP synchronization, the corrected kernel package hold flag, retained setting/key hashes and zero MTD3/MTD4 ECC failures were checked afterward. The installed UDP run returned all **439/439** datagrams; TCP again completed its HTTP exchange. Four live Runner entries covered both directions of both protocols: UDP hardware hits were 437/437 at capture, TCP 24/3. LuCI WAN/firewall pages and the firewall JavaScript asset returned HTTP 200 after authentication. Data storage has approximately **339.7MiB free**. The temporary USB Wi-Fi profile and credential XML were removed; primary Wi-Fi was left connected.

Startup, IPv6, package installation and household-trial follow-up: [35-everyday-use-readiness.md](35-everyday-use-readiness.md).
