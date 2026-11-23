#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# HH4000: one existing primary BSS per calibrated stock DHD radio.
. /lib/netifd/netifd-wireless.sh
init_wireless_driver "$@"

drv_bcmdhd_init_device_config() {
 config_add_string ifname phy country bridge
}
drv_bcmdhd_init_iface_config() {
 config_add_boolean hidden
}
drv_bcmdhd_cleanup() { :; }
drv_bcmdhd_init_vlan_config() { :; }
drv_bcmdhd_init_station_config() { :; }

hh_ap() {
 local section="$1" ssid encryption key hidden mode ssid_hex pid exe n
 HH_COUNT=$((HH_COUNT + 1))
 [ "$HH_COUNT" = 1 ] || { HH_ERROR=MULTIPLE_BSS_UNSUPPORTED; return; }
 json_select config
 json_get_vars mode ssid encryption key hidden
 json_select ..
 [ "$mode" = ap ] || { HH_ERROR=AP_MODE_REQUIRED; return; }
 [ "${#ssid}" -ge 1 ] && [ "${#ssid}" -le 32 ] || { HH_ERROR=INVALID_SSID; return; }
 [ "$encryption" = psk2 ] || { HH_ERROR=WPA2_PSK_REQUIRED; return; }
 [ "${#key}" -ge 8 ] && [ "${#key}" -le 63 ] || { HH_ERROR=INVALID_PASSPHRASE; return; }
 case "$key" in *'
'*|*"$(printf '\r')"*) HH_ERROR=INVALID_PASSPHRASE; return;; esac
 ssid_hex="$(printf %s "$ssid" | hexdump -ve '1/1 "%02x"')"
 umask 077
 {
  printf 'interface=%s\ndriver=nl80211\nssid2=%s\n' "$HH_IFNAME" "$ssid_hex"
  printf 'hw_mode=%s\nchannel=%s\ncountry_code=CA\nieee80211d=1\n' "$hwmode" "$channel"
  printf 'wpa=2\nwpa_passphrase=%s\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n' "$key"
  printf 'wmm_enabled=1\nignore_broadcast_ssid=%s\nctrl_interface=/var/run/hostapd\n' "${hidden:-0}"
  [ -z "$network_bridge" ] || printf 'bridge=%s\n' "$network_bridge"
 } >"$HH_CONF"
 # netifd prepares and owns the configured bridge before AP setup.
 n=0
 until [ -z "$network_bridge" ] || [ -d "/sys/class/net/$network_bridge/bridge" ]; do
  n=$((n + 1)); [ "$n" -lt 30 ] || { HH_ERROR=BRIDGE_NOT_READY; return; }
  sleep 1
 done
 mkdir -p /var/run/hostapd
 vendor-hostapd -f "$HH_LOG" "$HH_CONF" >"$HH_LOG.stdout" 2>&1 &
 pid=$!
 sleep 1
 kill -0 "$pid" 2>/dev/null || { HH_ERROR=HOSTAPD_START_FAILED; return; }
 exe="$(readlink -f "/proc/$pid/exe")"
 wireless_add_process "$pid" "$exe" 1
 n=0
 while ! vendor-hostapd_cli -p /var/run/hostapd -i "$HH_IFNAME" status 2>/dev/null | grep -q "^state=ENABLED$"; do
  kill -0 "$pid" 2>/dev/null || { HH_ERROR=HOSTAPD_EXITED; return; }
  n=$((n + 1))
  [ "$n" -lt 45 ] || { HH_ERROR=HOSTAPD_READY_TIMEOUT; return; }
  sleep 1
 done
 wireless_add_vif "$section" "$HH_IFNAME"
}

drv_bcmdhd_setup() {
 local radio="$1" country phy ifname bridge htmode
 HH_WIDTH=20; HH_CENTER=; HH_HT=
 json_select config
 json_get_vars ifname country phy bridge htmode
 json_select ..
 case "$ifname" in wl0|wl1|wl2) ;; *) wireless_setup_failed INVALID_IFNAME; return;; esac
 [ "${country:-CA}" = CA ] || { wireless_setup_failed COUNTRY_NOT_VALIDATED; return; }
 case "${htmode:-HT20}" in HT20|VHT20|NOHT) ;; VHT80|HE80) HH_WIDTH=80;; *) wireless_setup_failed WIDTH_NOT_VALIDATED; return;; esac
 # Restrict operation to the board-tested Canadian non-DFS channel blocks.
 case "$ifname:$channel" in
  wl0:1|wl0:2|wl0:3|wl0:4|wl0:5|wl0:6|wl0:7|wl0:8|wl0:9|wl0:10|wl0:11) hwmode=g;;
  wl1:149|wl1:153|wl1:157|wl1:161|wl1:165|wl2:36|wl2:40|wl2:44|wl2:48) hwmode=a;;
  *) wireless_setup_failed CHANNEL_NOT_VALIDATED; return;;
 esac
 if [ "$HH_WIDTH" = 80 ]; then
  case "$ifname:$channel" in wl1:149|wl1:153|wl1:157|wl1:161) HH_CENTER=155;; wl2:36|wl2:40|wl2:44|wl2:48) HH_CENTER=42;; *) wireless_setup_failed WIDTH_CHANNEL_INVALID; return;; esac
  case "$channel" in 36|44|149|157) HH_HT='[HT40+]' ;; *) HH_HT='[HT40-]' ;; esac
 fi
 HH_IFNAME="$ifname"
 HH_CONF="/var/run/hh-${ifname}.conf"
 HH_LOG="/tmp/hh-${ifname}-hostapd.log"
 : >"$HH_LOG"
 {
  wlctl -i "$ifname" down &&
  wlctl -i "$ifname" country CA/0 &&
  wlctl -i "$ifname" ap 1 &&
  wlctl -i "$ifname" chanspec "$channel/$HH_WIDTH" &&
  wlctl -i "$ifname" up
 } >"/tmp/hh-${ifname}-radio.log" 2>&1 || {
  wireless_setup_failed RADIO_CONFIG_FAILED; return
 }
 wireless_set_data "phy=${phy:-phy${ifname#wl}}" "ifname=$ifname"
 HH_COUNT=0; HH_ERROR=
 for_each_interface ap hh_ap
 [ "$HH_COUNT" = 1 ] || HH_ERROR=ONE_AP_REQUIRED
 [ -z "$HH_ERROR" ] || {
  wireless_setup_failed "$HH_ERROR"
  return
 }
 wireless_set_up
}

drv_bcmdhd_teardown() {
 local ifname
 json_select config
 json_get_var ifname ifname
 json_select ..
 wireless_process_kill_all
 case "$ifname" in wl0|wl1|wl2)
  wlctl -i "$ifname" down >/dev/null 2>&1
  rm -f "/var/run/hh-${ifname}.conf"
 ;; esac
}
add_driver bcmdhd
