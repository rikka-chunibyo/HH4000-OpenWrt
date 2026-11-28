#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# HH4000: one hostapd instance per calibrated stock DHD radio.
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
 local ssid encryption key hidden mode ssid_hex bssid vif network_bridge n
 [ -z "$HH_ERROR" ] || return
 json_get_var network_bridge bridge
 json_select config
 json_get_vars mode ssid encryption key hidden
 json_select ..
 case "$mode" in
  ap) ;;
  mesh) HH_ERROR=MESH_DRIVER_UNSUPPORTED; return;;
  *) HH_ERROR=AP_MODE_REQUIRED; return;;
 esac
 [ "$HH_COUNT" -lt 8 ] || { HH_ERROR=TOO_MANY_APS; return; }
 [ "${#ssid}" -ge 1 ] && [ "${#ssid}" -le 32 ] || { HH_ERROR=INVALID_SSID; return; }
 [ "$encryption" = psk2 ] || { HH_ERROR=WPA2_PSK_REQUIRED; return; }
 [ "${#key}" -ge 8 ] && [ "${#key}" -le 63 ] || { HH_ERROR=INVALID_PASSPHRASE; return; }
 case "$key" in *'
'*|*"$(printf '\r')"*) HH_ERROR=INVALID_PASSPHRASE; return;; esac
 case "${hidden:-0}" in 0|1) ;; *) HH_ERROR=INVALID_HIDDEN; return;; esac
 case "$network_bridge" in *[!A-Za-z0-9_.-]*) HH_ERROR=INVALID_BRIDGE; return;; esac

 # Wait for netifd to own the bridge before hostapd can attach this BSS.
 n=0
 until [ -z "$network_bridge" ] || [ -d "/sys/class/net/$network_bridge/bridge" ]; do
  n=$((n + 1)); [ "$n" -lt 30 ] || { HH_ERROR=BRIDGE_NOT_READY; return; }
  sleep 1
 done

 ssid_hex="$(printf %s "$ssid" | hexdump -ve '1/1 "%02x"')"
 vif="$HH_IFNAME"
 if [ "$HH_COUNT" -gt 0 ]; then
  vif="$HH_IFNAME.$HH_COUNT"
  # The factory MAC need not align with hostapd's automatic BSSID mask.
  # Use a stable, locally administered address per physical radio and slot.
  bssid="$(printf '%s:%s\n' "$HH_MAC" "$HH_COUNT" |
   sha256sum | cut -c1-10 | sed 's/../:&/g; s/^/02/')"
  printf '\nbss=%s\nbssid=%s\n' "$vif" "$bssid" || {
   HH_ERROR=HOSTAPD_CONFIG_WRITE_FAILED
   return
  }
 fi
 if ! {
  printf 'ssid2=%s\n' "$ssid_hex" &&
  printf 'wpa=2\nwpa_passphrase=%s\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n' "$key" &&
  printf 'wmm_enabled=1\nignore_broadcast_ssid=%s\nctrl_interface=/var/run/hostapd\n' "${hidden:-0}" &&
  { [ -z "$network_bridge" ] || printf 'bridge=%s\n' "$network_bridge"; }
 }; then
  HH_ERROR=HOSTAPD_CONFIG_WRITE_FAILED
  return
 fi
 # Section names come from netifd. Keep their mapping for interface publication.
 HH_VIFS="$HH_VIFS $1:$vif"
 HH_COUNT=$((HH_COUNT + 1))
}

hh_start_ap() {
 local pid exe n entry vif
 mkdir -p /var/run/hostapd
 vendor-hostapd -f "$HH_LOG" "$HH_CONF" >"$HH_LOG.stdout" 2>&1 &
 pid=$!
 sleep 1
 exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null)"
 if [ -z "$exe" ] || ! wireless_add_process "$pid" "$exe" 1; then
  kill "$pid" 2>/dev/null
  wireless_setup_failed HOSTAPD_START_FAILED
  return 1
 fi
 for entry in $HH_VIFS; do
  vif="${entry#*:}"
  n=0
  until vendor-hostapd_cli -p /var/run/hostapd -i "$vif" status 2>/dev/null | grep -qx 'state=ENABLED'; do
   kill -0 "$pid" 2>/dev/null || { wireless_setup_failed HOSTAPD_EXITED; return 1; }
   n=$((n + 1))
   [ "$n" -lt 45 ] || { wireless_setup_failed HOSTAPD_READY_TIMEOUT; return 1; }
   sleep 1
  done
 done
 for entry in $HH_VIFS; do
  wireless_add_vif "${entry%%:*}" "${entry#*:}" || {
   wireless_setup_failed VIF_REGISTRATION_FAILED
   return 1
  }
 done
}

drv_bcmdhd_setup() {
 local country phy ifname htmode config_tmp LC_ALL=C
 HH_WIDTH=20; HH_CENTER=; HH_HT=
 json_select config
 json_get_vars ifname country phy htmode
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
 HH_MAC="$(cat "/sys/class/net/$ifname/address")"
 [ -n "$HH_MAC" ] || { wireless_setup_failed RADIO_NOT_FOUND; return; }
 HH_COUNT=0; HH_ERROR=; HH_VIFS=
 umask 077
 config_tmp="$(mktemp "$HH_CONF.XXXXXX")" || {
  wireless_setup_failed HOSTAPD_CONFIG_WRITE_FAILED
  return
 }
 if ! {
  printf 'interface=%s\ndriver=nl80211\n' "$HH_IFNAME" &&
  printf 'hw_mode=%s\nchannel=%s\ncountry_code=CA\nieee80211d=1\n' "$hwmode" "$channel"
 } >"$config_tmp"; then
  HH_ERROR=HOSTAPD_CONFIG_WRITE_FAILED
 else
  for_each_interface "" hh_ap >>"$config_tmp"
  [ "$HH_COUNT" -gt 0 ] || HH_ERROR="${HH_ERROR:-ONE_AP_REQUIRED}"
 fi
 if [ -n "$HH_ERROR" ] || ! mv -f "$config_tmp" "$HH_CONF"; then
  rm -f "$config_tmp"
  wireless_setup_failed "${HH_ERROR:-HOSTAPD_CONFIG_WRITE_FAILED}"
  return
 fi
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
 hh_start_ap || return
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
