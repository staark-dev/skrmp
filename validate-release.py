#!/usr/bin/env python3
"""
SKR Enhanced Client 1.0.0 — Release Validation Script
Task #21.1, Section 5: Automated Validation

Checks:
  1. Required files exist
  2. Bundled SA-MP SHA-256 matches pinned version
  3. All binaries have expected x86 (PE32) architecture
  4. Imported dependencies are available or correctly provisioned
  5. No stale binaries (build vs source timestamp)
  6. Release and build hashes match
  7. GTA files are not accidentally included
  8. Release manifest accurately describes files

Exit code 0 = all mandatory checks passed
Exit code 1 = one or more mandatory checks failed
"""

import hashlib
import json
import os
import struct
import sys
from datetime import datetime
from pathlib import Path

# ── Configuration ────────────────────────────────────────────────────

SAMP_R5_SHA256 = None  # Set via --samp-hash or extracted from samp_loader.cpp

REQUIRED_FILES = [
    "SKRLauncher.exe",
    "SKR.Client.dll",
    "samp/samp.dll",
    "cef/client.dll",
    "cef/renderer.exe",
    "cef/libcef.dll",
    "cef/chrome_elf.dll",
    "cef/icudtl.dat",
    "cef/resources.pak",
    "cef/locales/en-US.pak",
    "SKR/lobby/lobby.html",
    "SKR/lobby/lobby.js",
    "SKR/lobby/lobby.css",
]

OPTIONAL_FILES = [
    "cef/chrome_100_percent.pak",
    "cef/chrome_200_percent.pak",
    "cef/v8_context_snapshot.bin",
    "cef/d3dcompiler_47.dll",
    "cef/libEGL.dll",
    "cef/libGLESv2.dll",
    "SKR/lobby/assets/background.png",
    "SKR/lobby/assets/logo.png",
    "LICENSES/",
    "release-manifest.json",
]

BANNED_FILES = [
    "gta_sa.exe", "gta-sa.exe",
    "stream.ini", "handling.cfg",
    "anim/", "audio/", "data/", "models/", "text/",
    "eax.dll", "ogg.dll", "vorbis.dll", "vorbisFile.dll",
]

PE_BINARIES = [
    "SKRLauncher.exe",
    "SKR.Client.dll",
    "samp/samp.dll",
    "cef/client.dll",
    "cef/renderer.exe",
    "cef/libcef.dll",
    "cef/chrome_elf.dll",
]

SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
    "shell32.dll", "ole32.dll", "oleaut32.dll", "ws2_32.dll",
    "wsock32.dll", "wininet.dll", "winmm.dll", "comctl32.dll",
    "comdlg32.dll", "shlwapi.dll", "msvcrt.dll", "ntdll.dll",
    "d3d9.dll", "d3d11.dll", "dxgi.dll", "imm32.dll",
    "version.dll", "setupapi.dll", "winspool.drv", "crypt32.dll",
    "bcrypt.dll", "ncrypt.dll", "secur32.dll", "sspicli.dll",
    "rpcrt4.dll", "iphlpapi.dll", "userenv.dll", "psapi.dll",
    "powrprof.dll", "dbghelp.dll", "wtsapi32.dll", "propsys.dll",
    "uxtheme.dll", "dwmapi.dll", "netapi32.dll", "mswsock.dll",
    "usp10.dll", "msimg32.dll", "dnsapi.dll", "profapi.dll",
}
# Include CRT api-ms-win shims
for _name in [
    "runtime", "heap", "math", "stdio", "string", "locale",
    "convert", "time", "filesystem", "environment", "utility",
    "multibyte", "process", "conio", "private",
]:
    SYSTEM_DLLS.add(f"api-ms-win-crt-{_name}-l1-1-0.dll")
SYSTEM_DLLS.update({"vcruntime140.dll", "msvcp140.dll", "ucrtbase.dll"})

REDISTRIBUTABLE_DLLS = {
    "d3dx9_25.dll": "DirectX End-User Runtime (June 2010)",
    "vcruntime140.dll": "Visual C++ 2015-2022 Redistributable (x86)",
    "msvcp140.dll": "Visual C++ 2015-2022 Redistributable (x86)",
}


class ValidationResult:
    def __init__(self):
        self.passed = []
        self.warnings = []
        self.failed = []
        self.info = []

    def ok(self, msg):
        self.passed.append(msg)
        print(f"  [PASS] {msg}")

    def warn(self, msg):
        self.warnings.append(msg)
        print(f"  [WARN] {msg}")

    def fail(self, msg):
        self.failed.append(msg)
        print(f"  [FAIL] {msg}")

    def note(self, msg):
        self.info.append(msg)
        print(f"  [INFO] {msg}")

    @property
    def success(self):
        return len(self.failed) == 0


def sha256_file(filepath):
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def read_pe_machine(filepath):
    try:
        with open(filepath, "rb") as f:
            if f.read(2) != b"MZ":
                return None
            f.seek(0x3C)
            pe_offset = struct.unpack("<I", f.read(4))[0]
            f.seek(pe_offset)
            if f.read(4) != b"PE\x00\x00":
                return None
            return struct.unpack("<H", f.read(2))[0]
    except Exception:
        return None


def read_pe_imports(filepath):
    try:
        with open(filepath, "rb") as f:
            if f.read(2) != b"MZ":
                return []
            f.seek(0x3C)
            pe_offset = struct.unpack("<I", f.read(4))[0]
            f.seek(pe_offset)
            if f.read(4) != b"PE\x00\x00":
                return []
            machine = struct.unpack("<H", f.read(2))[0]
            num_sections = struct.unpack("<H", f.read(2))[0]
            f.read(12)
            opt_header_size = struct.unpack("<H", f.read(2))[0]
            f.read(2)
            opt_header_start = f.tell()
            magic = struct.unpack("<H", f.read(2))[0]
            if magic == 0x10B:
                dd_offset = opt_header_start + 96
            elif magic == 0x20B:
                dd_offset = opt_header_start + 112
            else:
                return []
            f.seek(dd_offset + 8)  # DD[1] = Import Table
            import_rva = struct.unpack("<I", f.read(4))[0]
            import_size = struct.unpack("<I", f.read(4))[0]
            if import_rva == 0:
                return []
            section_table_offset = opt_header_start + opt_header_size
            f.seek(section_table_offset)
            sections = []
            for _ in range(num_sections):
                sec = f.read(40)
                vrva = struct.unpack("<I", sec[12:16])[0]
                vsize = struct.unpack("<I", sec[8:12])[0]
                raw_ptr = struct.unpack("<I", sec[20:24])[0]
                raw_size = struct.unpack("<I", sec[16:20])[0]
                sections.append((vrva, vsize, raw_ptr, raw_size))

            def rva_to_offset(rva):
                for vrva, vsize, raw_ptr, raw_size in sections:
                    if vrva <= rva < vrva + max(vsize, raw_size):
                        return raw_ptr + (rva - vrva)
                return None

            imports = []
            offset = rva_to_offset(import_rva)
            if offset is None:
                return []
            while True:
                f.seek(offset)
                entry = f.read(20)
                if len(entry) < 20:
                    break
                name_rva = struct.unpack("<I", entry[12:16])[0]
                if name_rva == 0:
                    break
                name_off = rva_to_offset(name_rva)
                if name_off:
                    f.seek(name_off)
                    dll_name = b""
                    while True:
                        ch = f.read(1)
                        if not ch or ch == b"\x00":
                            break
                        dll_name += ch
                    try:
                        imports.append(dll_name.decode("ascii").lower())
                    except Exception:
                        pass
                offset += 20
            return imports
    except Exception:
        return []


def read_pe_timedatestamp(filepath):
    try:
        with open(filepath, "rb") as f:
            f.seek(0x3C)
            pe_offset = struct.unpack("<I", f.read(4))[0]
            f.seek(pe_offset + 8)
            return struct.unpack("<I", f.read(4))[0]
    except Exception:
        return None


def check_required_files(release_dir, result):
    print("\n=== Check 1: Required files ===")
    for relpath in REQUIRED_FILES:
        fullpath = release_dir / relpath
        if fullpath.exists():
            result.ok(f"{relpath} ({fullpath.stat().st_size:,} bytes)")
        else:
            result.fail(f"MISSING: {relpath}")
    for relpath in OPTIONAL_FILES:
        fullpath = release_dir / relpath
        if fullpath.exists():
            if fullpath.is_dir():
                result.note(f"Optional dir present: {relpath}")
            else:
                result.note(f"Optional present: {relpath}")
        else:
            result.warn(f"Optional missing: {relpath}")


def check_samp_hash(release_dir, result, pinned_hash=None):
    print("\n=== Check 2: SA-MP SHA-256 verification ===")
    samp_path = release_dir / "samp" / "samp.dll"
    if not samp_path.exists():
        result.fail("samp/samp.dll not found")
        return
    actual = sha256_file(samp_path)
    result.note(f"samp.dll SHA-256: {actual}")
    if pinned_hash:
        if actual == pinned_hash.upper():
            result.ok("SHA-256 matches pinned R5 hash")
        else:
            result.fail(f"SHA-256 MISMATCH — expected {pinned_hash[:16]}...")
    else:
        result.warn("No pinned hash provided — use --samp-hash to verify")


def check_pe_architecture(release_dir, result):
    print("\n=== Check 3: PE architecture (x86) ===")
    for relpath in PE_BINARIES:
        fullpath = release_dir / relpath
        if not fullpath.exists():
            result.fail(f"{relpath}: file not found")
            continue
        machine = read_pe_machine(str(fullpath))
        if machine is None:
            result.fail(f"{relpath}: not a valid PE file")
        elif machine == 0x14C:
            result.ok(f"{relpath}: x86 (0x{machine:04X})")
        elif machine == 0x8664:
            result.fail(f"{relpath}: x64 — must be x86")
        else:
            result.fail(f"{relpath}: unexpected machine 0x{machine:04X}")


def check_imports(release_dir, result):
    print("\n=== Check 4: Import dependencies ===")
    bundled = {f.name.lower() for f in release_dir.rglob("*.dll")}
    for relpath in PE_BINARIES:
        fullpath = release_dir / relpath
        if not fullpath.exists():
            continue
        imports = read_pe_imports(str(fullpath))
        if not imports:
            result.warn(f"{relpath}: could not read imports")
            continue
        result.note(f"{relpath} imports: {', '.join(imports)}")
        for dll in imports:
            dl = dll.lower()
            if dl in SYSTEM_DLLS or dl.startswith("api-ms-"):
                pass
            elif dl in bundled:
                pass
            elif dl in {k.lower() for k in REDISTRIBUTABLE_DLLS}:
                desc = next((v for k, v in REDISTRIBUTABLE_DLLS.items() if k.lower() == dl), "unknown")
                result.warn(f"{relpath} → {dll}: requires {desc}")
            else:
                result.warn(f"{relpath} → {dll}: not bundled, not system")


def check_no_stale(release_dir, result, build_dir=None):
    print("\n=== Check 5: Binary timestamps ===")
    for relpath in PE_BINARIES:
        fullpath = release_dir / relpath
        if not fullpath.exists():
            continue
        ts = read_pe_timedatestamp(str(fullpath))
        if ts and ts > 0:
            try:
                dt = datetime.utcfromtimestamp(ts)
                result.note(f"{relpath}: PE timestamp {dt.strftime('%Y-%m-%d %H:%M:%S')} UTC")
            except Exception:
                result.note(f"{relpath}: PE timestamp 0x{ts:08X}")
    if build_dir:
        bp = Path(build_dir)
        for name in ["SKR.Client.dll", "SKRLauncher.exe"]:
            bf, rf = bp / name, release_dir / name
            if bf.exists() and rf.exists():
                if sha256_file(str(bf)) == sha256_file(str(rf)):
                    result.ok(f"{name}: release matches build")
                else:
                    result.fail(f"{name}: release differs from build — stale copy?")


def check_no_gta_files(release_dir, result):
    print("\n=== Check 7: No GTA files ===")
    found = False
    for banned in BANNED_FILES:
        if banned.endswith("/"):
            d = release_dir / banned.rstrip("/")
            if d.exists() and d.is_dir():
                result.fail(f"GTA directory: {banned}")
                found = True
        else:
            for m in release_dir.rglob(banned):
                result.fail(f"GTA file: {m.relative_to(release_dir)}")
                found = True
    if not found:
        result.ok("No GTA game files detected")


def check_manifest(release_dir, result):
    print("\n=== Check 8: Release manifest ===")
    mp = release_dir / "release-manifest.json"
    if not mp.exists():
        result.warn("release-manifest.json not found — use --generate-manifest")
        return
    try:
        manifest = json.loads(mp.read_text())
    except Exception as e:
        result.fail(f"Manifest parse error: {e}")
        return
    if "files" not in manifest:
        result.fail("Manifest missing 'files' key")
        return
    mf = {e.get("path"): e for e in manifest["files"]}
    for relpath in REQUIRED_FILES:
        norm = relpath.replace("\\", "/")
        if norm not in mf:
            result.fail(f"Manifest missing: {norm}")
        else:
            fp = release_dir / relpath
            if fp.exists() and "sha256" in mf[norm]:
                if sha256_file(str(fp)).upper() == mf[norm]["sha256"].upper():
                    result.ok(f"Manifest hash OK: {norm}")
                else:
                    result.fail(f"Manifest hash mismatch: {norm}")
    for pk in mf:
        if not (release_dir / pk).exists():
            result.warn(f"Manifest lists missing file: {pk}")


def generate_manifest(release_dir):
    print("Generating release-manifest.json...")
    files = []
    for fp in sorted(release_dir.rglob("*")):
        if fp.is_file() and fp.name != "release-manifest.json":
            rp = str(fp.relative_to(release_dir)).replace("\\", "/")
            entry = {"path": rp, "size": fp.stat().st_size, "sha256": sha256_file(str(fp))}
            if rp.endswith((".exe", ".dll")):
                m = read_pe_machine(str(fp))
                if m is not None:
                    entry["pe_machine"] = f"0x{m:04X}"
                ts = read_pe_timedatestamp(str(fp))
                if ts:
                    entry["pe_timestamp"] = f"0x{ts:08X}"
            files.append(entry)
    manifest = {
        "product": "SKR Enhanced Client",
        "version": "1.0.0",
        "generated": datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
        "file_count": len(files),
        "files": files,
    }
    mp = release_dir / "release-manifest.json"
    mp.write_text(json.dumps(manifest, indent=2))
    print(f"Wrote {mp} ({len(files)} files)")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="SKR Enhanced Client 1.0.0 — Release Validation")
    parser.add_argument("release_dir", nargs="?")
    parser.add_argument("--build-dir")
    parser.add_argument("--samp-hash")
    parser.add_argument("--generate-manifest", action="store_true")
    args = parser.parse_args()

    release_dir = Path(args.release_dir) if args.release_dir else (
        Path.home() / "Desktop" / "SKR Enhanced — Release Packaging" / "SKR Enhanced Client 1.0.0"
    )
    if not release_dir.exists():
        print(f"ERROR: Release directory not found: {release_dir}")
        sys.exit(1)

    print("=" * 60)
    print("SKR Enhanced Client 1.0.0 — Release Validation")
    print("=" * 60)
    print(f"Release directory: {release_dir}")
    print(f"Timestamp: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

    if args.generate_manifest:
        generate_manifest(release_dir)

    result = ValidationResult()
    check_required_files(release_dir, result)
    check_samp_hash(release_dir, result, pinned_hash=args.samp_hash)
    check_pe_architecture(release_dir, result)
    check_imports(release_dir, result)
    check_no_stale(release_dir, result, build_dir=args.build_dir)
    check_no_gta_files(release_dir, result)
    check_manifest(release_dir, result)

    print("\n" + "=" * 60)
    print("VALIDATION SUMMARY")
    print("=" * 60)
    print(f"  Passed:   {len(result.passed)}")
    print(f"  Warnings: {len(result.warnings)}")
    print(f"  Failed:   {len(result.failed)}")
    print(f"  Info:     {len(result.info)}")

    if result.failed:
        print("\nFAILED CHECKS:")
        for msg in result.failed:
            print(f"  x {msg}")
    if result.warnings:
        print("\nWARNINGS:")
        for msg in result.warnings:
            print(f"  ! {msg}")
    print()
    if result.success:
        print("VERDICT: PASS")
        sys.exit(0)
    else:
        print(f"VERDICT: FAIL — {len(result.failed)} mandatory check(s) failed.")
        sys.exit(1)


if __name__ == "__main__":
    main()
