#!/usr/bin/env python3
"""Build the userspace target for the kernel's Broadcom SKIPLOG module."""
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
IPTABLES = "iptables-1.8.10"
IPTABLES_SHA256 = "5cc255c189356e317d070755ce9371eb63a1b783c34498fb8c30264f3cc59c9c"
ABI = "12"
MODULE = "lib/modules/4.1.52-5.02L.07/xt_SKIPLOG.ko"


def build(rootfs: Path) -> None:
    library_dir = rootfs / "usr/lib"
    library = library_dir / f"libxtables.so.{ABI}"
    archive = PROJECT / "work/wan-firewall" / f"{IPTABLES}.tar.xz"
    sdk = PROJECT / "work/lan-fix/sdk/staging_dir"
    compilers = sorted(sdk.glob("toolchain-*/bin/aarch64-openwrt-linux-musl-gcc"))

    if not library.is_file() or not (rootfs / MODULE).is_file():
        raise SystemExit("The image needs libxtables.so.12 and the matching vendor xt_SKIPLOG.ko.")
    if len(compilers) != 1:
        raise SystemExit(f"Expected one OpenWrt 24.10.4 AArch64 SDK compiler under {sdk}.")
    if not archive.is_file():
        raise SystemExit(f"Missing upstream headers. Put the verified {IPTABLES}.tar.xz under {archive.parent}.")
    if hashlib.sha256(archive.read_bytes()).hexdigest() != IPTABLES_SHA256:
        raise SystemExit(f"Incorrect SHA-256 for {archive}.")

    extension_dir = library_dir / "iptables"
    extension_dir.mkdir(parents=True, exist_ok=True)
    source = PROJECT / "src/openwrt-hh4000/net-extras/libxt_SKIPLOG.c"
    with tempfile.TemporaryDirectory(prefix="skiplog-", dir=extension_dir) as directory:
        staging = Path(directory)
        with tarfile.open(archive, "r:xz") as package:
            for name in ("xtables.h", "xtables-version.h.in"):
                header = package.extractfile(f"{IPTABLES}/include/{name}")
                if header is None:
                    raise SystemExit(f"Missing {name} in {archive}.")
                contents = header.read().replace(b"@libxtables_vmajor@", ABI.encode())
                (staging / name.removesuffix(".in")).write_bytes(contents)

        plugin = staging / "libxt_SKIPLOG.so"
        subprocess.run(
            [str(compilers[0]), "-Os", "-Wall", "-Wextra", "-Werror",
             "-fPIC", "-shared", "-nostartfiles",
             "-I", str(staging), str(source), "-o", str(plugin)],
            # The xtables loader exports the API when it opens the extension.
            # OpenWrt's packaged libxtables has its ELF section table stripped.
            env={**os.environ, "STAGING_DIR": str(sdk / "target-aarch64_generic_musl")},
            check=True,
        )
        plugin.replace(extension_dir / plugin.name)
