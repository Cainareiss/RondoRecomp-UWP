#!/usr/bin/env python3
"""Prepare official Windows SDK 22621 and MSVC 14.38 Store files for Linux.

Requires acceptance of Microsoft's SDK/build tools terms. Everything is kept
under build/, including downloaded license/provenance manifests and libraries.
No administrator access, account, compiler installation, or game data is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

XWIN_VERSION = "0.10.0"
XWIN_ARCHIVE_SHA256 = "d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88"
SDK_VERSION = "10.0.22621"
CRT_VERSION = "14.38.17.8"
LICENSE_URL = "https://go.microsoft.com/fwlink/?LinkId=2086102"


def download(url, destination, checksum):
    if destination.is_file() and hashlib.sha256(destination.read_bytes()).hexdigest() == checksum.lower():
        return
    temporary = destination.with_name(destination.name + ".download")
    urllib.request.urlretrieve(url, temporary)
    actual = hashlib.sha256(temporary.read_bytes()).hexdigest()
    if actual != checksum.lower():
        temporary.unlink()
        raise RuntimeError(f"Checksum mismatch for {url}: {actual}")
    temporary.replace(destination)


def main():
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--accept-license", action="store_true",
                        help=f"Accept Microsoft SDK/build tools license: {LICENSE_URL}")
    parser.add_argument("--directory", type=Path, default=root / "build/cross-toolchain")
    args = parser.parse_args()
    if not args.accept_license:
        parser.error(f"Read {LICENSE_URL} and pass --accept-license to download SDK/CRT files")
    base = args.directory.resolve()
    base.mkdir(parents=True, exist_ok=True)
    platform = f"xwin-{XWIN_VERSION}-x86_64-unknown-linux-musl"
    archive = base / "xwin.tar.gz"
    url = f"https://github.com/Jake-Shadle/xwin/releases/download/{XWIN_VERSION}/{platform}.tar.gz"
    download(url, archive, XWIN_ARCHIVE_SHA256)
    executable = base / platform / "xwin"
    if not executable.is_file():
        with tarfile.open(archive) as tar:
            # Only extract a release whose digest is pinned above, and still
            # disallow traversal outside the dedicated toolchain directory.
            tar.extractall(base, filter="data")
    cache = base / "cache22621"
    sysroot = base / "sysroot22621"
    command = [str(executable), "--accept-license", "--cache-dir", str(cache),
               "--sdk-version", SDK_VERSION, "--crt-version", CRT_VERSION]
    # list downloads only Microsoft's channel/package manifests. Preserve the
    # untouched upstream manifest before selecting its matching SDK UCRT.
    subprocess.run(command + ["list"], check=True, stdout=subprocess.DEVNULL)
    package_path = next((cache / "dl").glob("pkg_manifest_*.vsman"))
    provenance = base / "microsoft-package-manifest-original.vsman"
    if not provenance.exists():
        shutil.copyfile(package_path, provenance)
    manifest = json.loads(provenance.read_text())
    sdk = next(p for p in manifest["packages"] if p["id"] == "Win11SDK_" + SDK_VERSION)
    ucrt_msi = next(p for p in sdk["payloads"]
                    if "Universal CRT Headers Libraries and Sources" in p["fileName"])
    ucrt = next(p for p in manifest["packages"]
                if p["id"] == "Microsoft.Windows.UniversalCRT.HeadersLibsSources.Msi")
    ucrt_payload = dict(ucrt_msi)
    ucrt_payload["fileName"] = "Universal CRT Headers Libraries and Sources-x86_en-us.msi"
    # xwin 0.10 prefers a global, older UCRT package. Select the SDK's own
    # official payload and CAB checksums instead. No SDK header is patched and
    # WINAPI_FAMILY_APP remains enforced during compilation.
    ucrt["payloads"] = [ucrt_payload] + [p for p in sdk["payloads"] if p["fileName"].endswith(".cab")]
    package_path.write_text(json.dumps(manifest))
    selection = {"sdk": SDK_VERSION, "crt": CRT_VERSION, "xwin": XWIN_VERSION,
                 "license": LICENSE_URL, "sdkUcrt": ucrt_msi,
                 "originalManifestSha256": hashlib.sha256(provenance.read_bytes()).hexdigest()}
    (base / "toolchain-provenance.json").write_text(json.dumps(selection, indent=2) + "\n")
    subprocess.run(command + ["splat", "--output", str(sysroot)], check=True)

    # xwin merges CRT variants for desktop cross builds. Extract Store's own
    # libraries into a separate path used exclusively by build-linux.py.
    store_archive = cache / "dl" / f"Microsoft.VC.{CRT_VERSION}.CRT.x64.Store.base.vsix"
    store_dir = sysroot / "crt/lib/store-x64"
    store_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(store_archive) as package:
        for name in package.namelist():
            if "/lib/x64/store/" in name and name.endswith(".lib"):
                (store_dir / Path(name).name).write_bytes(package.read(name))

    # NTFS ignores case; Linux does not. Supply canonical aliases from the
    # projection header guards without editing any Microsoft header contents.
    winrt = sysroot / "sdk/include/cppwinrt/winrt"
    for header in list(winrt.rglob("*.h")):
        if header.is_symlink() or not header.name.startswith("windows."):
            continue
        match = re.search(r"WINRT_(Windows_\w+?)_[0-9]*_?H\b", header.read_text(errors="replace"))
        if not match:
            continue
        part = "." + header.stem.rsplit(".", 1)[1] if re.search(r"\.[0-9]\.h$", header.name) else ""
        alias = header.with_name(match.group(1).replace("_", ".") + part + ".h")
        if not alias.exists():
            alias.symlink_to(header.name)
    onecore = sysroot / "sdk/lib/um/x86_64/OneCore.lib"
    if not onecore.exists():
        onecore.symlink_to("onecore.lib")
    if not (sysroot / "sdk/include/ucrt/stdlib.h").is_file():
        raise RuntimeError("SDK UCRT extraction did not produce its required headers")
    print(f"Prepared official SDK/Store sysroot: {sysroot}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError, StopIteration) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
