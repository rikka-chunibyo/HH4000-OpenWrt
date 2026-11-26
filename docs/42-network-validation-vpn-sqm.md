# Direct-power network validation and optional VPN/SQM

Tests performed on 2026-11-10 local time. Detailed results live under
`work/remaining-validation/evidence`; module builds, package manifests, and
updater test results are in `work/remaining-validation`.

## Power and unexpected resets

The captured `D%G` sequence comes from the stock kernel's
`kerSysDyingGaspIsr`. Disassembly confirms that it prints those three bytes,
notifies power-loss callbacks, arms the watchdog, and loops. A subsequent
software-reset report is compatible with this power-loss path; it does not
by itself establish an ordinary software crash.

After the user bypassed the Arduino-controlled relay and used the router's
normal supply directly, the router stayed on the same boot through more than 95 minutes of testing. UART stayed
connected. No relay commands were sent. This implicates the relay power path
but does not distinguish a wiring/connector/contact issue from a false
power-loss signal or establish the precise failed component. Keep direct
power while evaluating the router. Later planned software reboots for RAM
maintenance are recorded separately and are not spontaneous resets.

## LAN mapping and VLAN isolation


All four negotiated 1G. Default WAN remains eth4. The full 10G performance
test is deferred until suitable adapters are available.

The vendor kernel has 802.1Q support but lacks bridge VLAN filtering. Use
explicit UCI `config device` entries of type `8021q` for each port/VLAN,
then add those devices to ordinary bridges. For example:

```
config device
 option type '8021q'
 option name 'eth0.100'
 option ifname 'eth0'
 option vid '100'
```

Implicit `eth0.100` interface shorthand did not work reliably with this
kernel/netifd combination. Avoid a VLAN-aware bridge/DSA recipe.

The temporary test used VLANs 100 and 200 across eth0/eth1/eth2, alongside
the native LAN. Tests passed tagged gateway access on all six paths,
same-VLAN bridging, DHCP OFFER/ACK and DNS on both VLANs, blocked guest
management on TCP 22/80/443, blocked cross-VLAN forwarding, no guest
broadcast leakage onto the native management adapter, VLAN 100 WAN NAT
(30/30 replies), and VLAN 200 WAN denial. These are IPv4 wired VLAN tests;
they do not establish guest Wi-Fi or IPv6 behavior. The temporary UCI tree
was mounted in RAM and all original configuration hashes were verified
when it was removed. No test VLAN is left configured.

Four concurrent bidirectional TCP echo streams ran for 120 seconds, one
per LAN adapter, with byte-for-byte payload checks and no errors. These
are router endpoint/USB/CPU-constrained rates, not routing benchmarks.
A separate 120-second routed UDP test returned 5,569/5,569 packets with
valid payloads; conntrack showed hardware acceleration in both directions.

## Optional OpenVPN

`hh4000-tun` supplies a real module for the retained kernel ABI and loads
it through `/etc/init.d/hh-tun`. OpenVPN 2.6.23 and `luci-app-openvpn` are
installed in the separate data overlay. No production profile is enabled.

The data-path test used two temporary OpenVPN loopback endpoints, distinct
TUN routes, scoped NAT/firewall rules, and an external LAN packet generator.
It returned 30/30 encrypted-path replies with flow acceleration disabled
and another 30/30 after restoring acceleration. The outer capture contained
no test plaintext. Temporary endpoints, keys, routes, rules, and listeners
were removed. Static-key mode was used only to test this path; production
TLS/client configuration and remote-provider interoperability were not tested.
No DCO or VPN throughput claim is made. WireGuard has not been validated.

### SKIPLOG in new images

The image builder compiles `libxt_SKIPLOG.so` against the pinned iptables
1.8.10 headers and includes the matching vendor kernel module from the
local rootfs. After firewall3 starts or reloads, it installs IPv4 and IPv6
mangle rules for ingress from `tun*` and forwarded or locally generated
egress to `tun*`. Use a TUN device whose name starts with `tun` for this
policy. These matches cover routed TUN payloads. Outer VPN transport can
still use the normal WAN path.

SKIPLOG tells Broadcom's datapath to skip flow learning for matched packets.
Packets continue through normal firewall processing. The helper leaves the
global flow-learning settings alone and firmware still flushes stale flow
decisions after firewall policy changes. Verify the rule counters and the
hardware acceleration state of an ordinary routed flow in a live test.
[Broadcom SKIPLOG kernel target source](https://raw.githubusercontent.com/RMerl/asuswrt-merlin.ng/main/release/src-rt-5.02L.07p2axhnd/kernel/linux-4.1/net/netfilter/xt_SKIPLOG.c)

## Optional SQM

`hh4000-sqm` adds **Network -> SQM**. It uses HTB, FQ-CoDel, IFB ingress
redirection, and u32/mirred modules built for `4.1.52-5.02L.07`. It does not
claim CAKE support or install generic 6.6-kernel modules. The user can choose
a logical network (default `wan`), override its device, and set upload and
download limits in kbit/s; zero leaves that direction unshaped. At least
one rate must be nonzero when enabled.

SQM pauses vendor flow learning and hardware acceleration globally because
those paths would bypass the shaper. Disabling/stopping the service restores
the previously recorded flags and removes owned queues/IFB. The service
follows network changes and uses procd supervision. Tests passed enable,
disable, explicit restart, SIGKILL recovery, invalid-device rejection, and
restoration of the previous acceleration state. Actual package removal while enabled also restored queues/acceleration; the package was reinstalled with its original disabled configuration. A real bidirectional TCP
test observed the configured 20 Mbit/s ceiling with valid payloads and
both ingress/egress FQ-CoDel counters. This is functional validation, not
a maximum shaping performance measurement. SQM is left **disabled** so
normal hardware acceleration remains on.

The initial package failed to respond to its first configuration change
because the newly installed UCI file was not yet tracked by `reload_config`.
The package now primes that tracking at installation. Explicit reload also
restarts the instance so changing WAN selection cannot leave shaping on an
old device.

## Changed artifacts and storage scope

Sources: `src/openwrt-hh4000/net-extras`, `src/openwrt-hh4000/sqm`, and the
package builders in `tools`. Validation helpers and results are retained
under `tools/remaining_validation` and the private evidence directory.
Optional TUN/OpenVPN/SQM packages write the UBI2 data overlay. Firmware,
bootloaders, saved U-Boot environment, kernel and DTB were not updated by
these package installations. The old vendor kernel remains a limitation;
passing these tests is not a claim that it meets current upstream security
support standards.

The return boot verified automatic TUN loading, all five Ethernet links and
all three Wi-Fi interfaces, panel/reset daemons, enabled hardware
acceleration, and unchanged configuration/key/program fingerprints.
LuCI SQM and OpenVPN pages returned HTTP 200 in an authenticated read-only
verification session; anonymous status access was denied. The session was
removed. No full interactive browser test was performed. Temporary
maintenance files were deleted; one verified preflight backup remains in
`/overlay/.hh-updates`, and its host copy is in the private evidence set.
The final optional SQM package is 1.0-r6; package-removal hooks stop it
and restore queues/offload before its files are removed.

Removal testing exposed an asynchronous stop race: procd could return
before the daemon released its lock, allowing opkg to delete tc before
queue cleanup finished. The final package uses a synchronous `quiesce`
action and refuses removal if cleanup cannot complete. Actual enabled
package removal and reinstallation passed after this fix.
