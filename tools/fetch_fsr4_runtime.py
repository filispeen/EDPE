"""Fetch the optional inputs for AMD FSR 4.1.2 into external/. Nothing fetched here is committed.

    python tools/fetch_fsr4_runtime.py                  # FidelityFX SDK and the Agility SDK D3D12Core.dll
    python tools/fetch_fsr4_runtime.py --ffx-only
    python tools/fetch_fsr4_runtime.py --agility-only
    python tools/fetch_fsr4_runtime.py --force          # fetch again even if the files are present
    python tools/fetch_fsr4_runtime.py --dry-run        # print what would be fetched

FidelityFX SDK (external/ffx-sdk): a shallow, sparse clone of the official AMD SDK at the pinned tag, limited to the
headers, the signed binaries (the FidelityFX loader and upscaler DLLs) and the license. The .github build action uses
the same pins through this script.

Agility SDK runtime (external/agility/D3D12Core.dll, optional): the x64 D3D12Core.dll from the Microsoft.Direct3D.D3D12
NuGet package at the pinned version, checked against a pinned SHA-256. The D3D11-D3D12 bridge uses it when present and
falls back to the system D3D12 runtime otherwise.

The signed AMD DLLs and D3D12Core.dll are third-party binaries. Check their license terms before redistributing them.
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

FFX_REPO = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git"
FFX_TAG = "v2.4.0"
FFX_SPARSE = [
    "Kits/FidelityFX/api",
    "Kits/FidelityFX/framegeneration",
    "Kits/FidelityFX/upscalers",
    "Kits/FidelityFX/signedbin",
    "LICENSE.txt",
]
# CMakeLists.txt enables frame generation and FSR 4 from these files.
FFX_MARKERS = [
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h",
    "Kits/FidelityFX/upscalers/include/ffx_upscale.h",
    "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll",
    "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll",
]

AGILITY_VERSION = "1.616.1"
AGILITY_URL = ("https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/%s/microsoft.direct3d.d3d12.%s.nupkg"
               % (AGILITY_VERSION, AGILITY_VERSION))
AGILITY_SHA256 = "3d8f4af714947c8b64b5656f48deb08661d6a5f743d97c63766065bd633ad52e"
AGILITY_MEMBER = "build/native/bin/x64/D3D12Core.dll"


def log(message):
    print("fetch_fsr4_runtime: " + message, flush=True)


def run(command, cwd=None):
    subprocess.run(command, cwd=cwd, check=True)


def fetch_ffx(dest, force, dry_run):
    sdk = dest / "ffx-sdk"
    if not force and all((sdk / marker).exists() for marker in FFX_MARKERS):
        log("FidelityFX SDK %s already present in %s" % (FFX_TAG, sdk))
        return
    log("fetching the FidelityFX SDK %s (sparse: %s) into %s" % (FFX_TAG, ", ".join(FFX_SPARSE), sdk))
    if dry_run:
        return
    # A partial or older copy (for example a restored CI cache without the upscaler headers) is replaced.
    if sdk.exists():
        shutil.rmtree(sdk)
    sdk.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--depth", "1", "--branch", FFX_TAG, "--filter=blob:none", "--sparse", FFX_REPO, str(sdk)])
    run(["git", "sparse-checkout", "set"] + FFX_SPARSE, cwd=sdk)
    shutil.rmtree(sdk / ".git", ignore_errors=True)
    missing = [marker for marker in FFX_MARKERS if not (sdk / marker).exists()]
    if missing:
        sys.exit("fetch_fsr4_runtime: the clone is missing: " + ", ".join(missing))
    log("FidelityFX SDK ready")


def fetch_agility(dest, force, dry_run):
    target = dest / "agility" / "D3D12Core.dll"
    if not force and target.exists():
        log("D3D12Core.dll already present at %s" % target)
        return
    log("fetching the Agility SDK %s (%s) into %s" % (AGILITY_VERSION, AGILITY_URL, target))
    if dry_run:
        return
    with urllib.request.urlopen(AGILITY_URL, timeout=120) as response:
        package = response.read()
    digest = hashlib.sha256(package).hexdigest()
    if digest != AGILITY_SHA256:
        sys.exit("fetch_fsr4_runtime: SHA-256 of the Agility package does not match the pinned value\n"
                 "  expected %s\n  got      %s" % (AGILITY_SHA256, digest))
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        data = archive.read(AGILITY_MEMBER)
    target.parent.mkdir(parents=True, exist_ok=True)
    # Write beside the target and rename, so an interrupted run never leaves a partial D3D12Core.dll.
    handle, temp_path = tempfile.mkstemp(dir=str(target.parent), suffix=".tmp")
    with os.fdopen(handle, "wb") as out:
        out.write(data)
    os.replace(temp_path, target)
    log("D3D12Core.dll ready (%d bytes)" % len(data))


def main():
    parser = argparse.ArgumentParser(description="Fetch the optional AMD FSR 4.1.2 inputs into external/.")
    parser.add_argument("--dest", type=Path, default=Path(__file__).resolve().parent.parent / "external",
                        help="directory that receives ffx-sdk/ and agility/ (default: <repo>/external)")
    parser.add_argument("--ffx-only", action="store_true", help="fetch only the FidelityFX SDK")
    parser.add_argument("--agility-only", action="store_true", help="fetch only D3D12Core.dll")
    parser.add_argument("--force", action="store_true", help="fetch again even if the files are present")
    parser.add_argument("--dry-run", action="store_true", help="print what would be fetched and change nothing")
    args = parser.parse_args()
    if args.ffx_only and args.agility_only:
        parser.error("--ffx-only and --agility-only cannot be combined")
    if not args.agility_only:
        fetch_ffx(args.dest, args.force, args.dry_run)
    if not args.ffx_only:
        fetch_agility(args.dest, args.force, args.dry_run)


if __name__ == "__main__":
    main()
