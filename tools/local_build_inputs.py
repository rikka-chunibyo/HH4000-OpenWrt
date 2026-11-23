"""Validate the local overlay executables before assembling a firmware image."""

from pathlib import Path


def require_overlay_binaries(platform: Path, rootfs: Path) -> None:
    """Accept an input in the ignored overlay or an existing prepared rootfs."""
    overlay = platform / "files"
    entries = (platform / "local-binaries.txt").read_text().splitlines()
    missing = []
    for path in entries:
        if not path or path.startswith("#"):
            continue
        source = overlay / path
        if not source.exists() and not source.is_symlink():
            source = rootfs / path
        if source.is_file():
            with source.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    continue
        missing.append(path)
    if missing:
        raise SystemExit(
            "The firmware build requires local ELF inputs. See the platform README.\n"
            + "\n".join("  " + path for path in missing)
        )
