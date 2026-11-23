#!/rescue/busybox sh
# SPDX-License-Identifier: GPL-2.0-only
# Called only after mounting the verified writable data volume, before overlayfs.
hh_reset_data_root() {
 local root="$1" entry mountpoint
 case "$root" in /overlay|/tmp/hh-reset-test.*) ;; *) echo 'Invalid reset root' >&2; return 1;; esac
 [ -d "$root" ] && [ ! -L "$root" ] || return 1
 [ "$(cd "$root" && pwd -P)" = "$root" ] || return 1
 [ -f "$root/.hh-factory-reset-v1" ] && [ ! -L "$root/.hh-factory-reset-v1" ] || return 1
 [ "$("$BB" cat "$root/.hh-factory-reset-v1")" = HH4000_FACTORY_RESET_V1 ] || return 1
 # Refuse a nested mount: rm must never traverse another volume.
 while read -r _ mountpoint _; do case "$mountpoint" in "$root"/*) echo 'Nested mount blocks reset' >&2; return 1;; esac; done </proc/mounts
 # Keep the request until deletion and sync complete, so interrupted resets retry.
 for entry in "$root"/* "$root"/.[!.]* "$root"/..?*; do
  [ -e "$entry" ] || [ -L "$entry" ] || continue
  [ "$entry" != "$root/.hh-factory-reset-v1" ] || continue
  "$BB" rm -rf -- "$entry" || return 1
 done
 "$BB" sync || return 1
 "$BB" rm -- "$root/.hh-factory-reset-v1" || return 1
 "$BB" sync
}
