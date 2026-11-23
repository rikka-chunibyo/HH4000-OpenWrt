# OpenWrt Ethernet and Wi-Fi: live bring-up results

Tested 2026-11-08, entirely through an external RAM initramfs with the original kernel and board DTB. **Ethernet and a secured 2.4 GHz AP passed actual client traffic and SSH tests.** No replacement OS was flashed and no boot environment was saved. This remains a stock-kernel port of OpenWrt userspace, with the rescue bootstrap as PID 1.

## Confirmed on hardware

- OpenWrt 24.10.4 AArch64/musl userspace on stock kernel 4.1.52-5.02L.07.
- `eth3` is the port currently cabled to the laptop, negotiating 1000 Mbps full duplex. Five pings passed in each direction with zero loss after setup completed. SSH command execution passed over Ethernet.
- All three BCM43684 radios loaded the stock firmware and calibration and registered as `wl0`, `wl1`, `wl2`.
- `wl0` advertised the temporary SSID `HH4000-RAM-Test` with WPA2-PSK/AES-CCMP. A Windows RTL8188EU USB adapter associated and was reported AUTHENTICATED, ASSOCIATED, AUTHORIZED and SECURED.
- OpenWrt dnsmasq assigned the client 192.168.77.18 on an isolated wireless subnet. Five client pings to 192.168.77.1 passed with zero loss.
- A 4 MiB download, explicitly bound to the USB adapter address, and a 4 MiB SSH upload both matched SHA256 `e1d9afd30349263478ae1a9563bfca9526fb94e0834996ce38b69ea4fd52e14b`.
- Key-authenticated SSH worked over Wi-Fi as well as Ethernet. The primary laptop Wi-Fi connection remained connected to its existing network throughout.
- The watchdog was still healthy at final observed uptime 1040.63 seconds.

These checks establish useful packet paths, not throughput limits or long-term stability. Only the connected Ethernet port and the 2.4 GHz AP were tested with clients. The USB adapter is 2.4 GHz only; client association on the two 5 GHz radios remains untested. PON and voice were not configured.

Machine-readable results: hardware-results.json. Raw transcript: live-session.raw. Collected runtime files: evidence directory, including driver logs, DHCP exchange and final interface/module/mount/station state.

## Driver bundle and ABI

[build_openwrt_net.py](../tools/build_openwrt_net.py) extends the already validated OpenWrt console rootfs with stock modules, firmware, calibration and a private `/vendor` compatibility area. The stock tools are ARM32/glibc. Wrappers explicitly invoke `/vendor/lib/ld-linux.so.3 --library-path /vendor/lib:/vendor/usr/lib`, while normal OpenWrt programs use ARM64/musl. The dependency closure is saved in vendor-dependencies.json.

The exact boot-tested image is openwrt-net-tested.uImage, SHA256 `f9089f0028a0f18056a99e60c723b641c88761eb762a4687cfa1b00d56b4a825`, 16,283,893 bytes. It fits the existing 16 MiB ramdisk staging slot at 0x0e000000. Preserve the low `initrd_high` setting and legacy ramdisk wrapper from the previous successful boot recipe. The live runner is openwrt_net_live.py.

The builder source has subsequently been corrected to include the hardware-proven traffic-manager command and to handle bdmf_shell's session-valued exit status. That corrected source has passed syntax checking, but a newly rebuilt image from those final source edits has not yet been boot-tested. The tested image remains available unchanged.

## Ethernet dependencies that mattered

The practical load sequence used:

```
bcmlibs wlcsm bdmf opticaldet bcm_pondrv
rdpa_gpl rdpa_gpl_ext rdpa rdpa_usr rdpa_mw
bcmbrfp bcm_license bcm_spdsvc bcm_tcpspdtest chipinfo
pktflow bcm_enet cmdlist pktrunner bcmmcast bcmvlan
bcm_ingqos rdpa_cmd
```

Write the backed-up license to `/proc/driver/license` after bcm_license loads and to `/proc/spdsvc/license` after the speed-service modules load, as the original startup script does. The script is in RAM; the original license/calibration partitions do not need to be mounted writable.

The first reduced sequence omitted the speed-service modules. `bcm_enet` asserted that `enet_spdsvc_transmit` was NULL, panicked, and the watchdog rebooted the test. Loading bcm_spdsvc and bcm_tcpspdtest before bcm_enet fixed that. The speed-service dependency exists even when no speed test is being run.

`rdpa_cmd` also needs bcm_ingqos: otherwise its bcm_iq_register_hw/unregister_hw imports are unresolved. Both dependencies are now included in the preparation source.

The vendor bdmf shell node is created by the stock mkdev script, not devtmpfs:

```
mknod /dev/bdmf_shell c 321 0
bdmf_shell -c init
```

`bdmf_shell -c init` reports a session ID and can return that value as its exit status. The observed session 621 produced exit status 49, so a plain `set -e` must not interpret that as failed initialization. Check the `Session:` output instead.

Merely getting eth3 carrier and an IP address was insufficient. TX failed with `can't map DS port/queue 6/7 P0 to RDD`. The missing operation was:

```
tmctl porttminit --devtype ETH --if eth3 --flag 1
ip link set eth3 up
ip addr add 192.168.1.1/24 dev eth3
```

The tool created eight queues. Once link negotiation finished, ping and SSH worked in both directions. A software bridge was tried during diagnosis; the final successful direct Ethernet test did not require one. For a normal LAN with multiple wired/Wi-Fi members, bridge policy and hardware offload still need deliberate integration.

## Wi-Fi initialization and AP

`nvram kernelset /etc/calib/kernel_nvram.setting` loads the backed-up per-device calibration into the in-memory Broadcom store. The country settings used CA/0, matching stock. No `nvram commit` or `nvram kcommit` was run.

After the Ethernet/Runner base, the additional modules were:

```
wfd hnd emf igs cfg80211
insmod dhd.ko dhd_dpc_prio=5 iface_name=wl dhd_console_ms=0 firmware_path=/etc/wlan/dhd/
```

The stock firmware download and NVRAM upload/compare succeeded for all three radios. PCIe is available through this original kernel; a separate bcm_pcie_hcd.ko was neither available nor required. Runner DHD offload initialized. This does not establish support on a replacement modern kernel.

A single minimal vendor-hostapd configuration was sufficient for WPA2 on wl0:

```
interface=wl0
driver=nl80211
ssid=HH4000-RAM-Test
hw_mode=g
channel=6
country_code=CA
ieee80211d=1
wpa=2
wpa_passphrase=<fresh temporary test key>
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
ctrl_interface=/tmp/hostapd
```

**Channel caveat:** the requested channel was 6, but both wlctl and the client observed channel 1. AP operation worked, but channel selection needs investigation in the Broadcom/hostapd configuration path before treating this as a complete radio-control integration.

The vendor hostapd printed rfkill/ovs-vsctl warnings and initially remained in COUNTRY_UPDATE. It subsequently advertised the AP, created its control socket, completed client authentication and passed traffic. Its process command line starts with the explicit ARM32 loader, so a narrow `ps | grep hostapd` can miss it; `ps w` showed the full command. Keep these warnings for further investigation rather than assuming every stock integration feature is present.

The isolated test subnet used wl0=192.168.77.1/24. A Windows static address attempt was denied by the OS, so a temporary OpenWrt dnsmasq provided the address instead:

```
dnsmasq --conf-file=/dev/null --interface=wl0 --bind-interfaces --port=0 \
  --dhcp-range=192.168.77.10,192.168.77.20,255.255.255.0,10m \
  --dhcp-option=3 --dhcp-option=6 \
  --dhcp-leasefile=/tmp/hh-dhcp.leases --pid-file=/tmp/hh-dhcp.pid
```

Empty router and DNS options intentionally supplied neither a default route nor DNS. The USB interface could reach the test subnet while the primary laptop adapter continued serving internet/dev access. The test profile was manual and removed afterward; the USB adapter was returned disconnected with DHCP still enabled.

## SSH and transfer validation

OpenWrt's native dropbear and uhttpd worked on this stock kernel. Dropbear used a fresh temporary client public key, disabled password authentication, and listened only on the test addresses. A 4 MiB random file was served over uhttpd to the wireless client and uploaded back over SSH. Matching hashes confirmed the payloads, rather than relying solely on association or ping.

This OpenWrt BusyBox omits `base64` and `tftp`. The rescue BusyBox includes the working TFTP applet; use `/rescue/busybox tftp` for transfers until the package set is expanded. The temporary private SSH key and WLAN profile are not part of a persistent installation.

## Status UI and next integration work

[22-buttons-and-leds.md](22-buttons-and-leds.md) records the user-assisted physical button mapping, MCU events and camera-verified LED colours, including the red main ring and all-off capability. Alongside the previously tested display renderer, those interfaces are sufficient to design the status/menu backend.

Next: controlled procd startup, reproducible netifd integration around the working vendor setup, verification of 5 GHz APs with a capable client, explicit channel/power control, bridge forwarding and firewall/routing tests. The current experiment did not enable a WAN, test NAT, or replace Bell's permanent firmware. The router was returned to its normal saved stock boot after evidence capture. The saved bootcmd, stock squashfs root and stock init were verified; stock services were given time to initialize. See stock-restoration.raw.
