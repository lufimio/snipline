#!/usr/bin/env python3
"""Pin an Arch PKGBUILD to a published source/binary release or local archive."""
import argparse
import hashlib
from pathlib import Path
import re
import urllib.request


def render(template: str, repository: str, version: str, checksum: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*", repository):
        raise ValueError("repository must be owner/repository")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("version must be a stable X.Y.Z release without a v prefix")
    if not re.fullmatch(r"[0-9a-f]{64}", checksum):
        raise ValueError("invalid SHA-256 checksum")
    replacements = {
        "pkgver": f"pkgver={version}",
        "pkgrel": "pkgrel=1",
        "url": f"url='https://github.com/{repository}'",
        "sha256sums": f"sha256sums=('{checksum}')",
    }
    for key, replacement in replacements.items():
        template, count = re.subn(rf"^{key}=.*$", lambda _: replacement, template, flags=re.MULTILINE)
        if count != 1:
            raise ValueError(f"expected exactly one {key} assignment in PKGBUILD")
    return template


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True, help="GitHub owner/repository")
    parser.add_argument("--version", required=True, help="Stable version without v, such as 0.1.0")
    parser.add_argument("--archive", type=Path, help="Hash a local source archive instead of downloading")
    parser.add_argument("--binary", action="store_true", help="Use the prebuilt linux-x86_64 archive and snipline-bin template")
    parser.add_argument("--template", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    # Validate before constructing any URL or touching the output.
    relative = Path("packaging/bin/PKGBUILD") if args.binary else Path("PKGBUILD")
    template = (args.template or Path(__file__).resolve().parents[1] / relative).read_text()
    output = args.output or relative
    render(template, args.repository, args.version, "0" * 64)
    digest = hashlib.sha256()
    if args.archive:
        with args.archive.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    else:
        suffix = "-linux-x86_64" if args.binary else ""
        url = (f"https://github.com/{args.repository}/releases/download/v{args.version}/"
               f"snipline-{args.version}{suffix}.tar.gz")
        with urllib.request.urlopen(url, timeout=60) as stream:
            total = 0
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                total += len(chunk)
                if total > 128 * 1024 * 1024:
                    raise ValueError("source archive exceeds 128 MiB")
                digest.update(chunk)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(render(template, args.repository, args.version, digest.hexdigest()))


if __name__ == "__main__":
    main()
