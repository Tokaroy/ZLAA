"""
make_package.py - the ZLAA Builder downloads (for the release page): the game code built on a PC.

    python3 tools/pc/make_package.py <out folder> [<download cache> (default ~/laar-build/pc)]

Writes <out>/ZLAA-Builder-<version>-windows.zip and <out>/ZLAA-Builder-<version>-linux.tar.gz, each holding
  Build Game.bat / build-game.sh   what the player starts (drop the dump's folder on it, or start it and choose one)
  builder/      zlaa_build.py, the recompiler (recompiler/*.py) and the headers the game's code is compiled against
                (runtime/): the same as in this version's APK, so ZLAA takes what it builds
  zig/          zig (ziglang.org, MIT), only the parts a C compile needs: a C compiler and linker in one program
  python/       (Windows) Python's "embeddable package" (python.org, PSF license); on Linux the system's python3
  licenses/     ZLAA's, Python's, zig's and musl's (zig's libc headers)
  README.txt
Downloads are checked against pinned SHA-256 sums. Nothing of the game: it is built from the player's own files.
"""
import hashlib
import io
import os
import re
import sys
import tarfile
import urllib.request
import zipfile

PYTHON_WIN = ("https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip",
              "90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159")
ZIG_WIN = ("https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip",
           "554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c")
ZIG_LINUX = ("https://ziglang.org/download/0.14.1/zig-x86_64-linux-0.14.1.tar.xz",
             "24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c")
# the parts of zig's library a C compile for aarch64 Linux needs (as tools/android/builder/make_toolchain.py)
ZIG_LIB = ("include/", "std/", "compiler/", "libc/include/generic-musl/", "libc/include/aarch64-linux-musl/",
           "libc/include/any-linux-any/")
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DATE = (1980, 1, 1, 0, 0, 0)                         # (fixed dates: the same files make the same package)


def fetch(url, sha, folder):
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, os.path.basename(url))
    if not os.path.exists(path):
        print("downloading", url)
        urllib.request.urlretrieve(url, path + ".part")
        os.rename(path + ".part", path)
    h = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if h != sha:
        os.remove(path)
        sys.exit("%s: SHA-256 %s, expected %s (deleted it; run again)" % (path, h, sha))
    return path


def version():
    m = re.search(r"project\(\w+\s+VERSION\s+([0-9.]+)", open(os.path.join(REPO, "CMakeLists.txt")).read())
    return m.group(1)


def own_files():
    """(name in the package, bytes, executable) of ours: the builder, the recompiler, the headers, the notes."""
    files = [("builder/zlaa_build.py", open(os.path.join(REPO, "tools/pc/zlaa_build.py"), "rb").read(), False)]
    for f in sorted(os.listdir(os.path.join(REPO, "recompiler"))):
        if f.endswith(".py"):
            files.append(("builder/recompiler/" + f, open(os.path.join(REPO, "recompiler", f), "rb").read(), False))
    for f in ("recomp_runtime.h", "recomp_simd.h", "game_abi.c"):
        files.append(("builder/runtime/" + f, open(os.path.join(REPO, "runtime", f), "rb").read(), False))
    readme = open(os.path.join(REPO, "tools/pc/README.txt")).read().replace("@VERSION@", version())
    files.append(("README.txt", readme.encode(), False))
    files.append(("licenses/ZLAA.txt", open(os.path.join(REPO, "LICENSE"), "rb").read(), False))
    return files


def zig_parts(members, read):
    """(name in the package, bytes, executable) of zig's program, licenses and the library parts kept."""
    out = []
    for name in members:
        parts = name.split("/", 1)
        if len(parts) < 2 or name.endswith("/"):
            continue
        rel = parts[1]
        if rel in ("zig", "zig.exe"):
            out.append(("zig/" + rel, read(name), True))
        elif rel == "LICENSE":
            out.append(("licenses/zig.txt", read(name), False))
        elif rel == "lib/libc/musl/COPYRIGHT":
            out.append(("licenses/musl.txt", read(name), False))
        elif rel.startswith("lib/") and rel[4:].startswith(ZIG_LIB):
            out.append(("zig/" + rel, read(name), False))
    return out


def main():
    out_dir = sys.argv[1]
    cache = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/laar-build/pc")
    os.makedirs(out_dir, exist_ok=True)
    ver = version()
    top = "ZLAA-Builder-%s/" % ver
    ours = own_files()

    # Windows: a zip, with Python
    zw = zipfile.ZipFile(fetch(*ZIG_WIN, cache))
    pw = zipfile.ZipFile(fetch(*PYTHON_WIN, cache))
    files = ours + zig_parts(zw.namelist(), zw.read)
    files.append(("Build Game.bat", open(os.path.join(REPO, "tools/pc/build_game.bat"), "rb").read()
                  .replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"), False))
    for name in pw.namelist():
        files.append(("licenses/Python.txt" if name == "LICENSE.txt" else "python/" + name, pw.read(name), False))
    win = os.path.join(out_dir, "ZLAA-Builder-%s-windows.zip" % ver)
    with zipfile.ZipFile(win, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data, _ in sorted(files):
            z.writestr(zipfile.ZipInfo(top + name, date_time=DATE), data, zipfile.ZIP_DEFLATED)
    print("%s: %.1f MB" % (win, os.path.getsize(win) / 1e6))

    # Linux: a tar.gz, with the system's python3
    zl = tarfile.open(fetch(*ZIG_LINUX, cache))
    members = {m.name: m for m in zl.getmembers() if m.isfile()}
    files = ours + zig_parts(sorted(members), lambda n: zl.extractfile(members[n]).read())
    files.append(("build-game.sh", open(os.path.join(REPO, "tools/pc/build_game.sh"), "rb").read(), True))
    lin = os.path.join(out_dir, "ZLAA-Builder-%s-linux.tar.gz" % ver)
    with tarfile.open(lin, "w:gz", compresslevel=9, format=tarfile.USTAR_FORMAT) as t:
        for name, data, exe in sorted(files):
            info = tarfile.TarInfo(top + name)
            info.size, info.mode, info.mtime = len(data), 0o755 if exe else 0o644, 315532800
            t.addfile(info, io.BytesIO(data))
    print("%s: %.1f MB" % (lin, os.path.getsize(lin) / 1e6))
    for p in (win, lin):
        print(hashlib.sha256(open(p, "rb").read()).hexdigest(), os.path.basename(p))


if __name__ == "__main__":
    main()
