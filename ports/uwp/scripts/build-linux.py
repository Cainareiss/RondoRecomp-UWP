#!/usr/bin/env python3
"""Cross-compile the UWP executable with clang-cl and official MSVC/SDK files.

Run in the Linux development container. This script compiles and links only;
it does not download a game, accept SDK licenses, sign, or deploy a package.
The sysroot must contain UWP/Store CRT libraries, not desktop runtime imports.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


def main():
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysroot", type=Path,
                        default=root / "build/cross-toolchain/sysroot22621")
    parser.add_argument("--output", type=Path, default=root / "build/uwp-linux")
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--compiler", default="clang-cl-18")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 8:
        parser.error("--jobs must be between 1 and 8; this machine defaults to 2")
    for tool in (args.compiler, args.linker):
        if not shutil.which(tool):
            parser.error(f"Required tool is unavailable: {tool}")
    sysroot = args.sysroot.resolve()
    output = args.output.resolve()
    include_dirs = [sysroot / p for p in (
        "crt/include", "sdk/include/ucrt", "sdk/include/shared",
        "sdk/include/um", "sdk/include/winrt", "sdk/include/cppwinrt")]
    # Store libraries import MSVCP140_APP/VCRUNTIME140_APP. Never fall back to
    # the ordinary desktop CRT directory when a UWP library is missing.
    library_dirs = [sysroot / p for p in (
        "crt/lib/store-x64", "sdk/lib/ucrt/x86_64", "sdk/lib/um/x86_64")]
    for path in include_dirs + library_dirs:
        if not path.is_dir():
            parser.error(f"Incomplete official SDK/Store CRT sysroot: {path}")
    if not (library_dirs[0] / "msvcrt.lib").is_file():
        parser.error("Store CRT msvcrt.lib is missing")

    generated = args.generated_dir.resolve() if args.generated_dir else None
    if generated and not (generated / "table.c").is_file():
        parser.error("--generated-dir must contain rondo_gen output including table.c")
    objdir = output / "obj"
    logdir = output / "logs"
    # The official SDK is case-insensitive on Windows, while this cross-build
    # runs on Linux. Provide aliases for two legacy SDK includes whose spelling
    # differs from their on-disk names.
    include_compat = output / "include-case-compat"
    include_compat.mkdir(parents=True, exist_ok=True)
    um_compat = include_compat / "um"
    um_compat.mkdir(parents=True, exist_ok=True)
    for alias, target in (("OAIdl.h", "oaidl.h"), ("OCIdl.h", "ocidl.h"),
                          ("D2DBaseTypes.h", "d2dbasetypes.h"),
                          ("D2D1EffectAuthor.h", "d2d1effectauthor.h")):
        link = um_compat / alias
        source = include_dirs[3] / target
        if not source.is_file():
            parser.error(f"Windows SDK header is missing: {source}")
        if not link.exists():
            link.symlink_to(source)
    objdir.mkdir(parents=True, exist_ok=True)
    logdir.mkdir(parents=True, exist_ok=True)
    ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    project = ET.parse(root / "ports/uwp/RondoRecomp.vcxproj")
    sources = []
    for item in project.findall(".//m:ClCompile", ns):
        value = item.get("Include", "").replace("\\", "/")
        if not value.endswith((".c", ".cpp")) or "$" in value or "*" in value:
            continue
        if generated and value.endswith("recomp_none.c"):
            continue
        sources.append((root / "ports/uwp" / value).resolve())
    if generated:
        sources.extend(sorted(generated.glob("*.c")))
    common = [args.compiler, "--target=x86_64-pc-windows-msvc", "/nologo",
              "/c", "/MD", "/O2", "/Z7", "/bigobj", "/utf-8",
              "/DWINAPI_FAMILY=WINAPI_FAMILY_APP", "/DUNICODE", "/D_UNICODE",
              "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS", "/D_WIN32_WINNT=0x0A00",
              "/DWINVER=0x0A00"]
    common += ["/I" + str(um_compat)]
    common += ["-imsvc" + str(p) for p in include_dirs]
    common += ["/I" + str(root / "runtime"), "/I" + str(root / "third_party/lzma")]
    if generated:
        common += ["/I" + str(generated)]
    commands, objects = [], []
    for i, source in enumerate(sources):
        obj = objdir / f"{i:03d}_{source.stem}.obj"
        objects.append(obj)
        language = (["/TP", "/std:c++20", "/EHsc", "/Zc:__cplusplus"]
                    if source.suffix == ".cpp" else ["/TC", "/std:c11"])
        optimize = ["/O1"] if generated and source.parent == generated else []
        commands.append(common + language + optimize + ["/Fo" + str(obj), str(source)])
    (output / "compile_commands.json").write_text(json.dumps([
        {"directory": str(root), "file": str(src), "arguments": cmd}
        for src, cmd in zip(sources, commands)], indent=2) + "\n")

    def compile_one(job):
        i, command = job
        source = sources[i]
        print(f"[{i + 1}/{len(sources)}] {source.name}", flush=True)
        result = subprocess.run(command, cwd=root, capture_output=True, text=True)
        log = logdir / f"{i:03d}_{source.stem}.log"
        log.write_text(result.stdout + result.stderr)
        if result.returncode:
            print(log.read_text(), file=sys.stderr)
            raise RuntimeError(f"Compilation failed: {source}; log: {log}")

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(compile_one, enumerate(commands)))
    executable = output / "RondoRecomp.exe"
    link = [args.linker, "/nologo", "/out:" + str(executable),
            "/pdb:" + str(output / "RondoRecomp.pdb"), "/debug:full",
            "/pdbaltpath:RondoRecomp.pdb", "/subsystem:windows,10.00",
            "/machine:x64", "/APPCONTAINER", "/DYNAMICBASE", "/NXCOMPAT",
            "/HIGHENTROPYVA", "/OPT:REF", "/OPT:ICF", "/INCREMENTAL:NO"]
    link += ["/libpath:" + str(p) for p in library_dirs]
    link += [str(p) for p in objects]
    link += ["d3d11.lib", "dxgi.lib", "d2d1.lib", "dwrite.lib", "xaudio2.lib",
             "windowsapp.lib", "runtimeobject.lib"]
    (output / "link-command.json").write_text(json.dumps(link, indent=2) + "\n")
    result = subprocess.run(link, cwd=root, capture_output=True, text=True)
    (logdir / "link.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    # Check the resulting PE, instead of treating a successful linker as proof
    # that the architecture and AppContainer option were actually applied.
    data = executable.read_bytes()
    pe = int.from_bytes(data[0x3C:0x40], "little")
    assert data[pe:pe + 4] == b"PE\0\0", "Missing PE header"
    assert int.from_bytes(data[pe + 4:pe + 6], "little") == 0x8664, "Not x64"
    optional = pe + 24
    assert int.from_bytes(data[optional:optional + 2], "little") == 0x20B, "Not PE32+"
    assert int.from_bytes(data[optional + 70:optional + 72], "little") & 0x1000, "Not AppContainer"
    report = {"executable": str(executable), "sha256": hashlib.sha256(data).hexdigest(),
              "architecture": "x64", "appContainer": True,
              "core": "generated" if generated else "interpreter",
              "sources": len(sources), "consoleTested": False,
              "compiler": subprocess.check_output([args.compiler, "--version"], text=True).splitlines()[0]}
    (output / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Built x64 AppContainer: {executable}\nSHA256: {report['sha256']}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
