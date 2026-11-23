#!/usr/bin/env python3
"""Prepare UART commands for LuCI on a fresh tested HH4000 RAM boot.

Run this on dev, then run openwrt_net_live.py script luci-start.json on Windows.
Does not rebuild an image or contact the router. The generated commands only
configure the RAM filesystem; normal OpenWrt startup and firewall are not run.
"""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]
commands = [
    "hh-ethernet-init >/tmp/hh-ethernet.log 2>&1; grep -q 'Session:' /tmp/hh-bdmf-init.log",
    "tmctl porttminit --devtype ETH --if eth3 --flag 1; ip link set eth3 up",
    "mkdir -p /var/run/ubus /var/state /var/lock /tmp/luci-sessions /tmp/sysinfo",
    "printf 'config interface loopback\\n option device lo\\n option proto static\\n option ipaddr 127.0.0.1\\n option netmask 255.0.0.0\\n' >/etc/config/network",
    "printf '\\nconfig interface lan\\n option device eth3\\n option proto static\\n option ipaddr 192.168.1.1\\n option netmask 255.255.255.0\\n' >>/etc/config/network",
    "printf 'config system\\n option hostname HH4000-OpenWrt\\n option timezone UTC\\n' >/etc/config/system",
    "echo HH4000-OpenWrt >/proc/sys/kernel/hostname; printf '%s\\n' 'Sagemcom FAST5689 (HH4000)' >/tmp/sysinfo/model",
    "ubusd -s /var/run/ubus/ubus.sock >/tmp/hh-ubusd.log 2>&1 & echo UBUS_STARTED",
    "sleep 1; procd -S -s /var/run/ubus/ubus.sock >/tmp/hh-procd.log 2>&1 & echo PROCD_RPC_STARTED",
    "rpcd -s /var/run/ubus/ubus.sock >/tmp/hh-rpcd.log 2>&1 & echo RPCD_STARTED",
    "netifd -S -h /bin/true -s /var/run/ubus/ubus.sock >/tmp/hh-netifd.log 2>&1 & echo NETIFD_STARTED",
    "uhttpd -p 192.168.1.1:80 -h /www -x /cgi-bin -u /ubus -U /var/run/ubus/ubus.sock",
    "sleep 2; ubus call system board; ubus call network.interface.lan status",
]
# Leave space for the UART helper's exit-status marker. Long console input is
# silently truncated on this image, even with paced transmission.
assert all(len(c.encode()) < 350 and '\n' not in c for c in commands)
out = ROOT / 'work/openwrt-net/luci-start.json'
out.write_text(json.dumps(commands, indent=2) + '\n')
print(out)
