# HH4000 installation handoff

Start with [the installation guide](INSTALL.md), the
[capability summary](README.md#current-capabilities) and the
[research index](docs/README.md). The numbered notes are records of specific
lab sessions. For current firmware updates, use the
[data-preserving procedure](docs/43-data-preserving-firmware-updates.md).

- Confirm that the user owns the target and check its model, current boot
  environment, kernel ABI and storage layout. This project targets the
  FAST5689. An untouched stock unit lacks a supported first-time install
  path here.
- Verify a working UART console, direct power and off-device backups. Use
  Ethernet/file transfer for images. Keep credentials, raw flash dumps,
  board calibration and firmware images out of Git and public logs.
- Assemble a private candidate in a new `work/` output directory. Check its
  hashes and the updater stage, then validate the actual RAM boot before
  proposing any persistent write.
- For compatible existing installations, verify updater preflight and
  obtain authorization before installation. Record the RAM and persistent
  boot outcomes separately and report whether any firmware was written.
- Preserve access from a known LAN client while testing WAN and wireless
  changes. Keep the rescue image and backups available through testing.
