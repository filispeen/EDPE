"""Fetch and build the community Direct3D 11 port of AMD's FSR 3.1 upscaler for EDPE.

    python tools/fetch_ffx_dx11.py [--dest DIR] [--commit SHA]
    python tools/fetch_ffx_dx11.py --verify DIR
    python tools/fetch_ffx_dx11.py --dry-run [--dest DIR]

A pinned, shallow, sparse clone of https://github.com/optiscaler/FidelityFX-SDK-DX11
(HEAD 9b04fa49abae5aa341c0007af13c8ba1b4ad9913; MIT; a fork of
metarutaiga/FidelityFX-SDK-DX11 hardened by the OptiScaler project; FSR version
constants 3.1.2).

Built with CMake + VS2022's "Visual Studio 17 2022" generator and the fork's own
prebuilt shader compiler (sdk/tools/binary_store/FidelityFX_SC.exe).

Staged into external/ffx-dx11:
    <dest>\\include\\FidelityFX\\host\\...   the 7-header closure
    <dest>\\lib\\*.lib                      ffx_fsr3upscaler_x64.lib, ffx_backend_dx11_x64.lib
    <dest>\\LICENSE.txt
    <dest>\\VERSION.txt
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

URL = "https://github.com/optiscaler/FidelityFX-SDK-DX11"
COMMIT = "9b04fa49abae5aa341c0007af13c8ba1b4ad9913"
FSR_VERSION = "3.1.2"

SPARSE_PATHS = ["sdk"]

HEADERS = [
    "ffx_fsr3upscaler.h",
    "ffx_interface.h",
    "ffx_types.h",
    "ffx_error.h",
    "ffx_assert.h",
    "ffx_util.h",
    os.path.join("backends", "dx11", "ffx_dx11.h"),
]
HEADER_SRC = os.path.join("sdk", "include", "FidelityFX", "host")
HEADER_DEST = os.path.join("include", "FidelityFX", "host")

LIBS = ["ffx_fsr3upscaler_x64.lib", "ffx_backend_dx11_x64.lib"]

SHADER_COMPILER_REL = os.path.join("sdk", "tools", "binary_store", "FidelityFX_SC.exe")
SHADER_COMPILER_SHA256 = "30f9df1412742b7dcd5a8655c309ebfdb2210b070f9e6c1b09290a127c7d6dd7"

LICENCE = "LICENSE.txt"
VERSION_FILE = "VERSION.txt"
VERSION_COMMIT_KEY = "commit:"
VERSION_COMPILER_MARK = "SHA-256:"

CMAKE_CONFIGURE_ARGS = [
    "-DFFX_API_CUSTOM=OFF", "-DFFX_API_VK=OFF", "-DFFX_API_DX12=OFF", "-DFFX_ALL=OFF",
    "-DFFX_API_DX11=ON", "-DFFX_FSR=ON", "-DFFX_FSR3UPSCALER=ON",
    "-DFFX_FSR1=OFF", "-DFFX_FSR2=OFF", "-DFFX_FI=OFF", "-DFFX_OF=OFF",
    "-DFFX_AUTO_COMPILE_SHADERS=1", "-DFFX_BUILD_AS_DLL=OFF",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL", "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
]

FORBIDDEN_IMPORT_PREFIXES = (
    "__imp_CreateDXGI", "__imp_DXGI",
    "__imp_D3DCompile", "__imp_D3DReflect", "__imp_D3DDisassemble",
    "__imp_D3DStripShader", "__imp_D3DGetBlobPart", "__imp_D3DCreateBlob",
    "__imp_D3DGetDebugInfo", "__imp_D3DGetInputSignatureBlob", "__imp_D3DGetOutputSignatureBlob",
    "__imp_D3DGetInputAndOutputSignatureBlob", "__imp_D3DReadFileToBlob", "__imp_D3DWriteBlobToFile",
    "__imp_D3DPreprocess", "__imp_D3DLoadModule", "__imp_D3DCreateLinker", "__imp_D3DCreateFunctionLinkingGraph",
)


def default_dest():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(repo_root, "external", "ffx-dx11")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(*args, cwd=None):
    print("+", " ".join(str(a) for a in args))
    subprocess.check_call([str(a) for a in args], cwd=cwd)


def run_capture(*args, cwd=None):
    return subprocess.check_output([str(a) for a in args], cwd=cwd,
                                    stderr=subprocess.STDOUT).decode("utf-8", "replace")


def head_of(src):
    try:
        out = subprocess.check_output(["git", "-C", src, "rev-parse", "HEAD"], stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except (subprocess.CalledProcessError, OSError):
        return ""


def fetch(src, commit):
    os.makedirs(src, exist_ok=True)
    if not os.path.isdir(os.path.join(src, ".git")):
        run("git", "init", "-q", src)
        run("git", "-C", src, "remote", "add", "origin", URL)
    if head_of(src) != commit:
        run("git", "-C", src, "fetch", "--depth", "1", "--filter=blob:none", "origin", commit)
    run("git", "-C", src, "sparse-checkout", "set", *SPARSE_PATHS)
    run("git", "-C", src, "checkout", "-q", "--detach", commit)
    got = head_of(src)
    if got != commit:
        sys.exit("fetch_ffx_dx11: checked out %s, not the pinned %s" % (got, commit))
    print("FidelityFX-SDK-DX11 %s ready at %s" % (commit[:12], src))


def find_vs_install():
    progfiles86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = os.path.join(progfiles86, "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return None
    try:
        out = subprocess.check_output([
            vswhere, "-latest", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath",
        ]).decode().strip()
        return out or None
    except (subprocess.CalledProcessError, OSError):
        return None


def cmake_candidates():
    candidates = []
    on_path = shutil.which("cmake")
    if on_path:
        candidates.append(("PATH", on_path))
    vs = find_vs_install()
    if vs:
        candidates.append(("VS2022 bundled", os.path.join(
            vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")))
    return candidates


def pick_existing(candidates):
    for label, path in candidates:
        if path and os.path.isfile(path):
            return label, path
    return None, None


def find_cmake():
    label, path = pick_existing(cmake_candidates())
    if path:
        print("fetch_ffx_dx11: cmake: %s -> %s" % (label, path))
    return path


def find_dumpbin():
    vs = find_vs_install()
    if vs:
        hits = sorted(glob.glob(os.path.join(vs, "VC", "Tools", "MSVC", "*", "bin", "Hostx64", "x64", "dumpbin.exe")))
        if hits:
            return hits[-1]
    return shutil.which("dumpbin")


def toolset_string(cmake_exe):
    parts = []
    try:
        parts.append(run_capture(cmake_exe, "--version").splitlines()[0].strip())
    except (subprocess.CalledProcessError, OSError, IndexError):
        pass
    vs = find_vs_install()
    if vs:
        msvc_dirs = sorted(glob.glob(os.path.join(vs, "VC", "Tools", "MSVC", "*")))
        if msvc_dirs:
            parts.append("MSVC " + os.path.basename(msvc_dirs[-1]))
        parts.append(os.path.basename(vs.rstrip("\\/")))
    parts.append('generator "Visual Studio 17 2022" -A x64')
    return "; ".join(parts) if parts else "unknown"


def build(src, cmake_exe, jobs=None):
    sdk = os.path.join(src, "sdk")
    build_dir = os.path.join(sdk, "build_edpe")
    run(cmake_exe, "-S", sdk, "-B", build_dir, "-G", "Visual Studio 17 2022", "-A", "x64",
        *CMAKE_CONFIGURE_ARGS)
    args = [cmake_exe, "--build", build_dir, "--config", "Release", "--parallel", str(jobs or os.cpu_count() or 4)]
    run(*args)
    return os.path.join(sdk, "bin", "ffx_sdk")


def crt_of(directives_text):
    up = directives_text.upper()
    if "MSVCRT" in up:
        return "dynamic (MSVCRT)"
    if "LIBCMT" in up:
        return "static (LIBCMT)"
    return "unknown"


def version_facts(text):
    commit = None
    compiler = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.lower().startswith(VERSION_COMMIT_KEY):
            m = re.search(r"[0-9a-f]{40}", stripped)
            commit = m.group(0) if m else None
        elif VERSION_COMPILER_MARK in stripped:
            m = re.search(r"[0-9a-f]{64}", stripped)
            compiler = m.group(0) if m else None
    return commit, compiler


def imports_in(symbols_text):
    return sorted(set(re.findall(r"__imp_[A-Za-z0-9_]+", symbols_text)))


def forbidden_imports(names):
    return [n for n in names if n.startswith(FORBIDDEN_IMPORT_PREFIXES)]


def check_library(name, lib_path, run_directives, run_symbols, problems, summary):
    if not os.path.isfile(lib_path):
        problems.append("missing: %s" % lib_path)
        summary.append("%s: MISSING" % name)
        return
    directives = run_directives(lib_path)
    crt = crt_of(directives)
    if crt != "dynamic (MSVCRT)":
        problems.append("%s: CRT is %s, expected dynamic MSVCRT for EDPE" % (name, crt))
    symbols = run_symbols(lib_path)
    imports = imports_in(symbols)
    bad = forbidden_imports(imports)
    if bad:
        problems.append("%s: by-name import of %s" % (name, ", ".join(bad)))
    summary.append("%s: CRT=%s imports=%s" % (name, crt, ",".join(imports) if imports else "none"))


def stage(src, lib_dir, dest, commit, cmake_exe):
    include_dest = os.path.join(dest, HEADER_DEST)
    os.makedirs(os.path.join(include_dest, "backends", "dx11"), exist_ok=True)
    for rel in HEADERS:
        s = os.path.join(src, HEADER_SRC, rel)
        d = os.path.join(include_dest, rel)
        if not os.path.isfile(s):
            sys.exit("fetch_ffx_dx11: expected header missing from the checkout: %s" % s)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(s, d)

    # Patch ffx_dx11.h upstream typo: definition in ffx_dx11.cpp is non-const and C++ mangled.
    # Inlining ffxGetResourceDX11_Fsr31 directly resolves both const-ness and extern "C" linkage.
    ffx_dx11_h = os.path.join(include_dest, "backends", "dx11", "ffx_dx11.h")
    if os.path.isfile(ffx_dx11_h):
        txt = Path(ffx_dx11_h).read_text(encoding="utf-8")
        old_decl = """FFX_API FfxResource ffxGetResourceDX11_Fsr31(const ID3D11Resource* dx11Resource,
    FfxResourceDescription       ffxResDescription,
    wchar_t const*               ffxResName,
    FfxResourceStates            state = FFX_RESOURCE_STATE_COMPUTE_READ);"""
        inline_def = """inline FfxResource ffxGetResourceDX11_Fsr31(ID3D11Resource* dx11Resource,
    FfxResourceDescription       ffxResDescription,
    wchar_t const*               ffxResName = nullptr,
    FfxResourceStates            state = FFX_RESOURCE_STATE_COMPUTE_READ)
{
    FfxResource resource = {};
    resource.resource    = reinterpret_cast<void*>(dx11Resource);
    resource.state       = state;
    resource.description = ffxResDescription;
#ifdef _DEBUG
    if (ffxResName) {
        wcscpy_s(resource.name, ffxResName);
    }
#endif
    return resource;
}"""
        txt = txt.replace(old_decl, inline_def)
        Path(ffx_dx11_h).write_text(txt, encoding="utf-8")

    lib_dest = os.path.join(dest, "lib")
    os.makedirs(lib_dest, exist_ok=True)
    for name in LIBS:
        s = os.path.join(lib_dir, name)
        if not os.path.isfile(s):
            sys.exit("fetch_ffx_dx11: build did not produce %s (looked in %s)" % (name, lib_dir))
        shutil.copyfile(s, os.path.join(lib_dest, name))

    licence_src = os.path.join(src, LICENCE)
    if not os.path.isfile(licence_src):
        sys.exit("fetch_ffx_dx11: %s missing from the checkout" % licence_src)
    shutil.copyfile(licence_src, os.path.join(dest, LICENCE))

    dumpbin_exe = find_dumpbin()
    crt = "unknown"
    if dumpbin_exe:
        try:
            crt = crt_of(run_capture(dumpbin_exe, "/directives", os.path.join(lib_dest, LIBS[0])))
        except (subprocess.CalledProcessError, OSError):
            pass

    sc_path = os.path.join(src, SHADER_COMPILER_REL)
    sc_hash = sha256_of(sc_path) if os.path.isfile(sc_path) else "unknown (not found in the checkout)"

    version_text = (
        "commit: %s\n"
        "FSR: %s\n"
        "CRT: %s\n"
        "toolset: %s\n"
        "shader compiler (sdk\\tools\\binary_store\\FidelityFX_SC.exe) SHA-256: %s"
        " -- build-time only, never staged or shipped\n"
        % (commit, FSR_VERSION, crt, toolset_string(cmake_exe), sc_hash)
    )
    with open(os.path.join(dest, VERSION_FILE), "w", encoding="utf-8") as f:
        f.write(version_text)
    print("Staged FidelityFX DX11 %s into %s" % (FSR_VERSION, dest))


def verify(dest, quiet=False):
    problems = []
    summary = []

    missing_headers = [rel for rel in HEADERS if not os.path.isfile(os.path.join(dest, HEADER_DEST, rel))]
    if missing_headers:
        problems += ["missing: %s" % os.path.join(dest, HEADER_DEST, rel) for rel in missing_headers]
    summary.append("headers: %s" % ("ok (%d)" % len(HEADERS) if not missing_headers else "MISSING"))

    for name in (LICENCE, VERSION_FILE):
        if not os.path.isfile(os.path.join(dest, name)):
            problems.append("missing: %s" % os.path.join(dest, name))

    version_path = os.path.join(dest, VERSION_FILE)
    if os.path.isfile(version_path):
        commit, compiler = version_facts(
            Path(version_path).read_text(encoding="utf-8", errors="replace"))
        if commit is None or commit != COMMIT:
            problems.append("commit mismatch in %s" % version_path)
        else:
            summary.append("commit: %s" % commit[:12])

    dumpbin_exe = find_dumpbin()
    run_directives = (lambda p: run_capture(dumpbin_exe, "/directives", p)) if dumpbin_exe else None
    run_symbols = (lambda p: run_capture(dumpbin_exe, "/symbols", p)) if dumpbin_exe else None

    if run_directives and run_symbols:
        for name in LIBS:
            check_library(name, os.path.join(dest, "lib", name), run_directives, run_symbols, problems, summary)
    else:
        for name in LIBS:
            lib_path = os.path.join(dest, "lib", name)
            summary.append("%s: %s" % (name, "present" if os.path.isfile(lib_path) else "MISSING"))
            if not os.path.isfile(lib_path):
                problems.append("missing: %s" % lib_path)

    if not quiet:
        for line in summary:
            print("fetch_ffx_dx11: " + line)
        if problems:
            for p in problems:
                print("fetch_ffx_dx11: PROBLEM: " + p)
        else:
            print("fetch_ffx_dx11: FidelityFX DX11 %s verified at %s" % (FSR_VERSION, dest))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=None, help="where to fetch/build/stage (default: external\\ffx-dx11)")
    ap.add_argument("--work-dir", default=None, help="work directory for checkout and build (must be pure ASCII for FidelityFX_SC)")
    ap.add_argument("--commit", default=COMMIT, help="override the pinned commit")
    ap.add_argument("--verify", default=None, metavar="DIR", help="verify an existing staged copy and exit")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    dest = args.dest or default_dest()

    if args.verify:
        problems = verify(args.verify)
        return 2 if problems else 0

    work_dir = args.work_dir
    if not work_dir:
        # FidelityFX_SC has a known issue converting non-ASCII paths (like Cyrillic).
        # If dest path is not purely ASCII, use system TEMP which is guaranteed ASCII.
        if any(ord(c) > 127 for c in os.path.abspath(dest)):
            work_dir = os.path.join(tempfile.gettempdir(), "edpe_ffx_dx11_build")
        else:
            work_dir = os.path.join(dest, "src")

    if args.dry_run:
        print("Would fetch %s at %s into %s" % (URL, args.commit[:12], work_dir))
        print("Would configure+build sdk\\ with CMake + VS2022 (Release, dynamic /MD)")
        print("Would stage include\\, lib\\, %s and %s under %s" % (LICENCE, VERSION_FILE, dest))
        return 0

    src = work_dir
    existing_dest_src = os.path.join(dest, "src")
    if os.path.isdir(existing_dest_src) and not os.path.isdir(src):
        print("fetch_ffx_dx11: copying existing checkout from %s to %s..." % (existing_dest_src, src))
        shutil.copytree(existing_dest_src, src)

    fetch(src, args.commit)
    cmake_exe = find_cmake()
    if not cmake_exe:
        sys.exit("fetch_ffx_dx11: no cmake.exe on PATH or under VS2022")
    lib_dir = build(src, cmake_exe)
    stage(src, lib_dir, dest, args.commit, cmake_exe)
    problems = verify(dest)
    return 2 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
