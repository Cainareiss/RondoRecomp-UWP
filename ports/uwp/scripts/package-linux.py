#!/usr/bin/env python3
"""Package the interpreter bootstrap with the locally provisioned Linux tools.

Run from the repository with:
    dev python3 ports/uwp/scripts/package-linux.py

This does not download tools, publish packages, install certificates or deploy.
The native executable must already have been cross-compiled for UWP x64.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "build/cross-tools"
FOUNDATION = "http://schemas.microsoft.com/appx/manifest/foundation/windows10"
UAP = "http://schemas.microsoft.com/appx/manifest/uap/windows10"
SUBJECT = "CN=RondoRecomp.UWP.Dev"
ET.register_namespace("", FOUNDATION)
ET.register_namespace("uap", UAP)


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def wine_path(path):
    return "Z:" + str(path.resolve()).replace("/", "\\")


def run(command, log):
    with Path(log).open("wb") as stream:
        subprocess.run([str(part) for part in command], stdout=stream,
                       stderr=subprocess.STDOUT, check=True, cwd=ROOT)


def check_uwp_executable(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Executable is not a Windows PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 + 72 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Executable has a malformed PE header")
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    subsystem, flags = struct.unpack_from("<HH", data, optional + 68)
    if machine != 0x8664 or magic != 0x20B or subsystem != 2 or not flags & 0x1000:
        raise ValueError("Executable must be x64 PE32+, Windows GUI and APPCONTAINER")


def certificate(signing):
    signing.mkdir(parents=True, exist_ok=True)
    signing.chmod(0o700)
    key = signing / "RondoRecomp-development.key.pem"
    cert = signing / "RondoRecomp-development.cert.pem"
    if key.exists() != cert.exists():
        raise ValueError("Incomplete signing identity; preserve/recover the existing key and certificate")
    if not key.exists():
        previous_umask = os.umask(0o077)
        try:
            run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                 "-nodes", "-keyout", key, "-out", cert, "-days", "365",
                 "-subj", "/" + SUBJECT,
                 "-addext", "basicConstraints=critical,CA:FALSE",
                 "-addext", "keyUsage=critical,digitalSignature",
                 "-addext", "extendedKeyUsage=codeSigning"],
                signing / "certificate-generation.log")
        finally:
            os.umask(previous_umask)
    key.chmod(0o600)
    subject = subprocess.check_output(
        ["openssl", "x509", "-in", str(cert), "-subject", "-nameopt", "RFC2253", "-noout"],
        text=True).strip()
    if subject != "subject=" + SUBJECT:
        raise ValueError("Development certificate subject differs from the package Publisher")
    return key, cert


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path,
                        default=ROOT / "build/uwp-linux/RondoRecomp.exe")
    parser.add_argument("--output", type=Path, default=ROOT / "build/uwp/MSIX")
    args = parser.parse_args()
    executable, output = args.executable.resolve(), args.output.resolve()
    makemsix = TOOLS / "msix-build/bin/makemsix"
    fxc = TOOLS / "windows-sdk-cpp-22621/c/bin/10.0.22621.0/x64/fxc.exe"
    vclibs = ROOT / "build/cross-toolchain/vclibs/Microsoft.VCLibs.x64.14.00.appx"
    wine = Path("/usr/lib/wine/wine64")
    signer = shutil.which("osslsigncode")
    for path in (executable, makemsix, fxc, vclibs, wine):
        if not path.is_file():
            parser.error(f"Missing local build input: {path}. Run inside dev-ports with the provisioned tools.")
    if not signer:
        parser.error("osslsigncode is missing; run this script with dev python3")
    cache = (TOOLS / "msix-build/CMakeCache.txt").read_text()
    if "USE_VALIDATION_PARSER:BOOL=ON" not in cache or "MSIX_PACK:BOOL=ON" not in cache:
        parser.error("makemsix must be built with MSIX_PACK=ON and USE_VALIDATION_PARSER=ON")
    check_uwp_executable(executable)
    output.mkdir(parents=True, exist_ok=True)
    logs = ROOT / "build/uwp-linux/package-logs"
    logs.mkdir(parents=True, exist_ok=True)
    key, cert = certificate(TOOLS / "signing")
    with zipfile.ZipFile(vclibs) as package:
        dependency = ET.fromstring(package.read("AppxManifest.xml"))
    identity = dependency.find(f"{{{FOUNDATION}}}Identity")
    if identity is None or identity.get("Name") != "Microsoft.VCLibs.140.00" or identity.get("ProcessorArchitecture") != "x64":
        raise ValueError("Expected the Microsoft VCLibs 140 UWP x64 framework, not desktop DLLs")
    manifest = ET.parse(ROOT / "ports/uwp/Package.appxmanifest")
    package_identity = manifest.getroot().find(f"{{{FOUNDATION}}}Identity")
    if package_identity.get("Publisher") != SUBJECT or package_identity.get("ProcessorArchitecture") != "x64":
        raise ValueError("Unexpected package Publisher or architecture")
    application = manifest.getroot().find(f"{{{FOUNDATION}}}Applications/{{{FOUNDATION}}}Application")
    application.set("Executable", "RondoRecomp.exe")
    dependencies = manifest.getroot().find(f"{{{FOUNDATION}}}Dependencies")
    for item in list(dependencies):
        if item.tag == f"{{{FOUNDATION}}}PackageDependency" and item.get("Name") == identity.get("Name"):
            dependencies.remove(item)
    ET.SubElement(dependencies, f"{{{FOUNDATION}}}PackageDependency", {
        "Name": identity.get("Name"), "Publisher": identity.get("Publisher"),
        "MinVersion": identity.get("Version")})
    name = f"RondoRecomp-UWP_{package_identity.get('Version')}_x64_interpreter-bootstrap.msix"
    environment = os.environ.copy()
    environment.update(WINEPREFIX=str(TOOLS / "wine"), WINEDEBUG="-all")
    with tempfile.TemporaryDirectory(prefix="package-", dir=ROOT / "build/uwp-linux") as temporary:
        work = Path(temporary)
        layout = work / "layout"
        shutil.copytree(ROOT / "ports/uwp/Assets", layout / "Assets")
        (layout / "Shaders").mkdir()
        shutil.copy2(executable, layout / "RondoRecomp.exe")
        manifest.write(layout / "AppxManifest.xml", encoding="utf-8", xml_declaration=True)
        for stage, entry, profile in (("VS", "VSMain", "vs_4_0"), ("PS", "PSMain", "ps_4_0")):
            with (logs / f"fxc-{stage}.log").open("wb") as stream:
                subprocess.run([str(wine), str(fxc), "/nologo", "/O3", "/T", profile,
                                "/E", entry, "/Fo", wine_path(layout / f"Shaders/Frame{stage}.cso"),
                                wine_path(ROOT / "ports/uwp/Frame.hlsl")], env=environment,
                               stdout=stream, stderr=subprocess.STDOUT, check=True, cwd=ROOT)
        run([makemsix, "unpack", "-p", vclibs, "-d", work / "verified-vclibs"], logs / "vclibs-validation.log")
        unsigned, signed = work / "unsigned.msix", work / name
        run([makemsix, "pack", "-d", layout, "-p", unsigned], logs / "pack.log")
        run([signer, "sign", "-certs", cert, "-key", key, "-h", "sha256",
             "-n", "RondoRecomp UWP local development", "-in", unsigned, "-out", signed], logs / "sign.log")
        run([signer, "verify", "-CAfile", cert, "-in", signed], logs / "signature-validation.log")
        # Only the self-signed origin is accepted here; signature, hashes and
        # manifest validation remain enabled. VCLibs above uses normal trust.
        run([makemsix, "unpack", "-p", signed, "-d", work / "verified-app", "-ac"], logs / "package-validation.log")
        os.replace(signed, output / name)
    dependency_output = output / "Dependencies/x64/Microsoft.VCLibs.x64.14.00.appx"
    dependency_output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(vclibs, dependency_output)
    public_certificate = output / "RondoRecomp-development.cer"
    run(["openssl", "x509", "-in", cert, "-outform", "DER", "-out", public_certificate], logs / "export-public-cert.log")
    report = {
        "package": name, "sha256": sha256(output / name),
        "executable_sha256": sha256(executable),
        "vclibs_version": identity.get("Version"), "vclibs_sha256": sha256(vclibs),
        "schema_validation": True, "signature_verified": True,
        "resources_pri_generated": False, "windows_or_xbox_executed": False,
        "mode": "interpreter-bootstrap", "logs": str(logs),
    }
    (output / "package-report.json").write_text(json.dumps(report, indent=2) + "\n")
    payload = [output / name, public_certificate, dependency_output]
    (output / "SHA256SUMS.txt").write_text("".join(
        f"{sha256(path)}  {path.relative_to(output).as_posix()}\n" for path in payload))
    print(f"Signed MSIX generated and verified: {output / name}")
    print("Windows/Xbox installation and execution still require device testing.")


if __name__ == "__main__":
    main()
