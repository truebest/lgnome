#!/usr/bin/env python3
"""Preserve the settings directory's sticky bit lost by ares-package."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile


def replace_ar_member(archive, name, payload):
    if not archive.startswith(b"!<arch>\n"):
        raise ValueError("invalid ar archive")
    offset = 8
    member = None
    while offset < len(archive):
        header = archive[offset:offset + 60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise ValueError("invalid ar member header")
        size = int(header[48:58])
        end = offset + 60 + size + size % 2
        if size < 0 or end > len(archive):
            raise ValueError("truncated ar member")
        if header[:16].rstrip(b" ").removesuffix(b"/") == name.encode("ascii"):
            if member is not None:
                raise ValueError("duplicate data.tar payload")
            member = offset, size, end
        offset = end
    if member is None:
        raise ValueError("missing data.tar payload")
    size_field = str(len(payload)).encode("ascii").ljust(10, b" ")
    if len(size_field) != 10:
        raise ValueError("data.tar payload is too large")
    offset, old_size, end = member
    padding = archive[end - 1:end] if old_size % 2 else b"\n"
    # webOS requires the CLI's original member names and metadata.
    return (archive[:offset + 48] + size_field + archive[offset + 58:offset + 60] +
            payload + (padding if len(payload) % 2 else b"") + archive[end:])


def fix_settings_mode(ipk):
    ipk = Path(ipk).resolve()
    with tempfile.TemporaryDirectory(prefix="lgnome-ipk-mode-") as directory:
        root = Path(directory)
        subprocess.run(["ar", "x", str(ipk)], cwd=root, check=True)
        payloads = list(root.glob("data.tar*"))
        if len(payloads) != 1:
            raise ValueError("expected one data.tar payload")
        payload = payloads[0]
        modes = {".gz": "w:gz", ".xz": "w:xz", ".bz2": "w:bz2", ".tar": "w"}
        if payload.suffix not in modes:
            raise ValueError("unsupported data.tar compression")
        replacement = root / ("fixed-" + payload.name)
        found = False
        with tarfile.open(payload, "r:*") as source, tarfile.open(replacement, modes[payload.suffix]) as dest:
            for member in source:
                name = member.name.removeprefix("./").rstrip("/")
                if name == "usr/palm/applications/com.truebest.lgnome.native/settings":
                    if not member.isdir():
                        raise ValueError("settings entry is not a directory")
                    member.mode = 0o1777
                    found = True
                dest.addfile(member, source.extractfile(member) if member.isfile() else None)
        if not found:
            raise ValueError("missing settings directory")
        replacement.replace(payload)
        output = root / "package.ipk"
        output.write_bytes(replace_ar_member(ipk.read_bytes(), payload.name, payload.read_bytes()))
        # Replace atomically in the destination filesystem.
        with tempfile.NamedTemporaryFile(dir=ipk.parent, prefix=".lgnome-ipk-", delete=False) as temp:
            temporary = Path(temp.name)
        try:
            shutil.copy2(output, temporary)
            os.replace(temporary, ipk)
        finally:
            temporary.unlink(missing_ok=True)

if __name__ == "__main__":
    try:
        fix_settings_mode(sys.argv[1])
    except (IndexError, OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"fix-ipk-settings-mode: {error}")
