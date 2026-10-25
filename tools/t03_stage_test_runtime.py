"""T03-M1: stage the SKR-X VehicleReady build into the branded TEST runtime.

- Gate: build-gate/Release/SKRGate.dll -> server-runtime-branded-test/components/Gate.dll
  (the original is backed up first; SKRGate.dll is never copied beside Gate.dll).
- Client: verifies the canonical build output in place
  (build-test-certification-win32/Release/SKR.Client.dll and samp/SAMP/custom.img);
  nothing is copied, since that folder is the client test runtime.

Refuses while skr-server.exe or gta_sa.exe is running. Never touches server-runtime/
or any production installation. Nothing is deleted.

usage: python -I tools/t03_stage_test_runtime.py
"""
import datetime
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE_SRC = os.path.join(ROOT, "build-gate", "Release", "SKRGate.dll")
GATE_DST = os.path.join(ROOT, "server-runtime-branded-test", "components", "Gate.dll")
CLIENT_DLL = os.path.join(ROOT, "build-test-certification-win32", "Release", "SKR.Client.dll")
CUSTOM_IMG = os.path.join(ROOT, "build-test-certification-win32", "Release", "samp", "SAMP", "custom.img")
CUSTOM_IMG_SHA = "C9E4FCE661F3F96F56C6FB1658D150C41F302257A70EDBDC8554697FFF3B1FDB"


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def running(image):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"],
                         capture_output=True, text=True).stdout
    return image.lower() in out.lower()


def fail(message):
    print("FAIL:", message)
    sys.exit(1)


def main():
    for image in ("skr-server.exe", "gta_sa.exe"):
        if running(image):
            fail(f"{image} is running; stop it before staging")
    for path in (GATE_SRC, GATE_DST, CLIENT_DLL, CUSTOM_IMG):
        if not os.path.isfile(path):
            fail(f"missing {path}")

    img = sha(CUSTOM_IMG)
    if img != CUSTOM_IMG_SHA:
        fail(f"custom.img {img} is not the certified Operation 215 archive")
    print(f"client  SKR.Client.dll {sha(CLIENT_DLL)}")
    print(f"client  custom.img     {img} (certified)")

    src, dst = sha(GATE_SRC), sha(GATE_DST)
    if src == dst:
        print(f"gate    Gate.dll already {src}")
        return
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = f"{GATE_DST}.backup-t03-{dst[:8]}-{stamp}"
    shutil.copy2(GATE_DST, backup)
    if sha(backup) != dst:
        fail("backup readback mismatch")
    staging = GATE_DST + ".staging"
    if os.path.exists(staging):
        fail(f"{staging} exists; inspect it first")
    shutil.copy2(GATE_SRC, staging)
    if sha(staging) != src:
        fail("staging readback mismatch")
    os.replace(staging, GATE_DST)
    if sha(GATE_DST) != src:
        fail("final readback mismatch")
    print(f"gate    backup {backup} ({dst})")
    print(f"gate    Gate.dll {src}")


if __name__ == "__main__":
    main()
