"""
zlaa_build.py - build ZLAA's game code on a PC, from the player's own game files.

    python zlaa_build.py [game folder] [--out folder] [--jobs n] [--keep] [--push]

What Build Game does on the phone (host/builder.c), on a PC instead, where it takes minutes rather than an hour:
  1. translate: recompiler/recomp.py turns the dump's ARM64 code (exefs) into ~290 C files (~700 MB)
  2. compile them with zig cc for the phone (aarch64), as many at a time as the PC has cores (and memory for)
  3. link them with zig's ld.lld into libgame-<main build id>.so
The result is copied to the phone (the Download folder, the folder with the game files, or ZLAA's own folder
Android/data/io.github.zlaa/files/game); ZLAA takes it from there at its next start. --push does it by adb.

It runs from the ZLAA Builder package (python/, zig/ and builder/ beside each other: tools/pc/make_package.py) or
from the repository (python 3.10+ and zig 0.14 on PATH, or `pip install ziglang`). Nothing of the game is in
either: everything it makes comes from the files you point it at, and stays on your PC until you copy it.
"""
import argparse
import hashlib
import os
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WINDOWS = os.name == "nt"
VERSIONS = {"AE16F71E002AF8CB": "1.0.0", "909E904AF78AC1B8": "1.0.1 / 1.1.0"}   # (main's build id: ZLAA's list)
APP_DIR = "/sdcard/Android/data/io.github.zlaa/files"

# the compiler's options for the game's code: as host/builder.c and CMakeLists.txt (wrapping integers, no fused
# multiply-adds: as the Switch computes; no vectorizing or instruction scheduling: ~20% faster to build, as fast)
CFLAGS = ["-target", "aarch64-linux-musl", "-mcpu=generic+v8_2a", "-O2", "-g0", "-fPIC", "-fwrapv",
          "-ffp-contract=off", "-fno-math-errno", "-w", "-fvisibility=hidden", "-fno-vectorize", "-fno-slp-vectorize",
          "-mllvm", "-enable-misched=false", "-mllvm", "-enable-post-misched=false", "-fdata-sections",
          "-ffunction-sections", "-funwind-tables"]
JOB_MEMORY = 0.75e9            # a compile needs up to ~0.5 GB (the biggest files): room for that and the rest


def find(*names):
    """The first of these (relative to this script) that exists."""
    for n in names:
        p = os.path.normpath(os.path.join(HERE, n))
        if os.path.exists(p):
            return p
    return None


SOURCES = {"recompiler": find("recompiler", "../../recompiler"), "runtime": find("runtime", "../../runtime")}


def say(*a):
    print(*a, flush=True)


def fail(msg):
    say("\nError: " + msg)
    sys.exit(1)


def zig_command():
    z = find("zig/zig.exe" if WINDOWS else "zig/zig", "../zig/zig.exe" if WINDOWS else "../zig/zig")
    if z:
        return [z]
    z = shutil.which("zig")
    if z:
        return [z]
    try:                                             # (pip install ziglang)
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang"]
    except ImportError:
        fail("zig wasn't found: use the ZLAA Builder package, or install zig 0.14 (ziglang.org, or pip install ziglang)")


def abi_id():
    """The id of the headers the game's code is compiled against, as CMakeLists.txt makes it: ZLAA refuses a library
    built against other ones (another version of the app)."""
    hexes = ""
    for h in ("recomp_runtime.h", "recomp_simd.h"):
        hexes += hashlib.sha256(open(os.path.join(SOURCES["runtime"], h), "rb").read()).hexdigest()
    return hashlib.sha256(hexes.encode()).hexdigest()


def tools_id():
    """What the translated C and the objects depend on besides the game: when it changes, they are made again."""
    h = hashlib.sha256()
    for d in ("recompiler", "runtime"):
        for f in sorted(os.listdir(SOURCES[d])):
            if f.endswith((".py", ".h", ".c")):
                h.update(f.encode() + open(os.path.join(SOURCES[d], f), "rb").read())
    h.update(" ".join(CFLAGS).encode())
    return h.hexdigest()[:16]


def find_exefs(folder):
    """The exefs folder: this one, or one inside it (exefs, or ExeFS: the dumping tools differ)."""
    folder = folder.strip().strip('"')
    for c in (folder, os.path.join(folder, "exefs"), os.path.join(folder, "ExeFS")):
        if os.path.isfile(os.path.join(c, "main")):
            return c
    fail("there is no exefs folder with a file named 'main' in %s.\nChoose the folder of your dump that holds "
         "exefs (and romfs), or exefs itself." % folder)


def build_id(exefs):
    with open(os.path.join(exefs, "main"), "rb") as f:
        h = f.read(0x60)
    if len(h) != 0x60 or h[:4] != b"NSO0":
        fail(os.path.join(exefs, "main") + " is not a Switch program (NSO) file: is this an extracted exefs?")
    return h[0x40:0x48].hex().upper()


def pick_folder():
    """No folder given (the .bat double-clicked): ask for one."""
    if WINDOWS:                                      # (Windows' own folder dialog, through PowerShell)
        ps = ("Add-Type -AssemblyName System.Windows.Forms;"
              "$d = New-Object System.Windows.Forms.FolderBrowserDialog;"
              "$d.Description = 'Choose the folder of your Link''s Awakening dump (the one with exefs and romfs)';"
              "$d.ShowNewFolderButton = $false;"
              "if ($d.ShowDialog() -eq 'OK') { $d.SelectedPath }")
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-STA", "-Command", ps], capture_output=True, text=True)
            if r.stdout.strip():
                return r.stdout.strip()
        except OSError:
            pass
    say("Drag the folder of your Link's Awakening dump (the one with exefs and romfs) into this window, then press "
        "Enter:")
    try:
        return input("> ")
    except EOFError:
        fail("no folder chosen")


def memory_bytes():
    try:
        if WINDOWS:
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong), ("pf_total", ctypes.c_ulonglong),
                            ("pf_avail", ctypes.c_ulonglong), ("v_total", ctypes.c_ulonglong),
                            ("v_avail", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]
            s = Status()
            s.length = ctypes.sizeof(s)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s))
            return s.total
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (OSError, ValueError, AttributeError):
        return 8e9


def low_priority():
    """Children at a lower priority, so the PC stays usable while it works."""
    if WINDOWS:
        return {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS}
    return {"start_new_session": True}               # (and os.nice in main; its own group, for Ctrl+C)


def fmt_time(s):
    s = int(s + 0.5)
    return "%d:%02d" % (s // 60, s % 60) if s >= 60 else "%d s" % s


def bar(x, width=30):
    n = int(x * width + 0.5)
    return "[" + "#" * n + "-" * (width - n) + "] %3d%%" % int(x * 100)


class Line:
    """One status line, rewritten in place."""
    def __init__(self):
        self.last = ""

    def show(self, text):
        sys.stdout.write("\r" + text + " " * max(0, len(self.last) - len(text)))
        sys.stdout.flush()
        self.last = text

    def done(self, text=None):
        if text:
            self.show(text)
        sys.stdout.write("\n")
        self.last = ""


running = set()                                      # child processes, stopped on Ctrl+C
running_lock = threading.Lock()


def spawn(cmd, log, env, **kw):
    p = subprocess.Popen(cmd, stdout=kw.pop("stdout", log), stderr=subprocess.STDOUT, env=env, **low_priority(), **kw)
    with running_lock:
        running.add(p)
    return p


def reap(p):
    with running_lock:
        running.discard(p)


def stop_all():
    with running_lock:
        for p in running:
            try:
                p.kill() if WINDOWS else os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass


def translate(exefs, gen, stamp, logs):
    marker = os.path.join(gen, ".complete")
    if os.path.isfile(marker) and open(marker).read() == stamp:
        say("1/3 Translating: done before")
        return
    if os.path.isdir(gen):
        shutil.rmtree(gen)
    os.makedirs(gen)
    line, t0 = Line(), time.time()
    cmd = [sys.executable, os.path.join(SOURCES["recompiler"], "recomp.py"), exefs, "-o", gen, "--no-comments",
           "--progress"]
    with open(os.path.join(logs, "translate.log"), "w") as log:
        p = spawn(cmd, log, None, stdout=subprocess.PIPE, text=True)
        for text in p.stdout:
            if text.startswith("progress "):
                x = float(text.split()[1])
                t = time.time() - t0
                left = " (about %s left)" % fmt_time(t * (1 - x) / x) if x > 0.1 and t > 20 else ""
                line.show("1/3 Translating the game's code " + bar(x) + left)
            else:
                log.write(text)
        st = p.wait()
        reap(p)
    # (1: some words couldn't be translated, as always: data among the code; the C is still complete)
    if st > 1 or st < 0 or not os.path.isfile(os.path.join(gen, "functable.c")):
        line.done()
        fail("translating failed (status %d): see %s" % (st, os.path.join(logs, "translate.log")))
    line.done("1/3 Translating the game's code: done in " + fmt_time(time.time() - t0))
    open(marker, "w").write(stamp)


def compile_all(zig, gen, obj, stamp, jobs, env, logs):
    sources = sorted(f for f in os.listdir(gen) if f.endswith(".c"))
    paths = {f: os.path.join(gen, f) for f in sources}
    paths["game_abi.c"] = os.path.join(SOURCES["runtime"], "game_abi.c")   # (the library's ABI id)
    obj_stamp = os.path.join(obj, ".stamp")
    if not (os.path.isfile(obj_stamp) and open(obj_stamp).read() == stamp):
        if os.path.isdir(obj):
            shutil.rmtree(obj)
    os.makedirs(obj, exist_ok=True)
    open(obj_stamp, "w").write(stamp)

    def out(f):
        return os.path.join(obj, f[:-2] + ".o")

    todo = [f for f in paths if not (os.path.isfile(out(f)) and os.path.getmtime(out(f)) >= os.path.getmtime(paths[f]))]
    todo.sort(key=lambda f: -os.path.getsize(paths[f]))   # (the biggest first: none left alone at the end)
    total = sum(os.path.getsize(paths[f]) for f in todo) or 1
    done_bytes, t0, line = 0, time.time(), Line()
    flags = CFLAGS + ['-DRECOMP_ABI="%s"' % abi_id(), "-I" + gen, "-I" + SOURCES["runtime"]]
    log_path = os.path.join(logs, "compile.log")
    say("2/3 Compiling %d files, %d at a time" % (len(todo), jobs) if todo else "2/3 Compiling: done before")
    active = []                                      # (process, file, its log)
    failed = None
    with open(log_path, "w") as log:
        while (todo or active) and not failed:
            while todo and len(active) < jobs:
                f = todo.pop(0)
                flog = open(os.path.join(logs, "cc-" + f + ".log"), "w+")
                active.append((spawn(zig + ["cc"] + flags + ["-c", paths[f], "-o", out(f) + ".tmp"], flog, env), f, flog))
            time.sleep(0.2)
            for a in list(active):
                p, f, flog = a
                if p.poll() is None:
                    continue
                reap(p)
                active.remove(a)
                flog.seek(0)
                text = flog.read()
                flog.close()
                os.remove(flog.name)
                log.write(text)
                if p.returncode != 0:
                    failed = (f, text)
                    break
                os.replace(out(f) + ".tmp", out(f))
                done_bytes += os.path.getsize(paths[f])
            x, t = done_bytes / total, time.time() - t0
            left = " (about %s left)" % fmt_time(t * (1 - x) / x) if x > 0.05 and t > 20 else ""
            line.show("2/3 Compiling " + bar(x) + left)
    if failed:
        stop_all()
        line.done()
        fail("compiling %s failed:\n%s\n(the whole log: %s)" % (failed[0], failed[1][-3000:], log_path))
    if done_bytes:
        line.done("2/3 Compiling: done in " + fmt_time(time.time() - t0))
    return [out(f) for f in paths]


def link(zig, objects, work, lib, game_id, env, logs):
    say("3/3 Linking")
    # the library needs ZLAA's libmain.so (the runtime its functions call), found by that name on the phone; a stand-
    # in with that name is all the linker needs here (the symbols are looked up when ZLAA opens it)
    stub_c, stub_o, stub = (os.path.join(work, n) for n in ("libmain_stub.c", "libmain_stub.o", "libmain.so"))
    open(stub_c, "w").write("/* a stand-in for ZLAA's libmain.so, to link against */\nint zlaa_libmain_stub;\n")
    rsp = os.path.join(work, "objects.txt")
    with open(rsp, "w") as f:
        f.write("\n".join(o.replace("\\", "/") for o in objects) + "\n")
    with open(os.path.join(logs, "link.log"), "w") as log:
        for cmd in (zig + ["cc", "-target", "aarch64-linux-musl", "-fPIC", "-c", stub_c, "-o", stub_o],
                    zig + ["ld.lld", "-shared", "-o", stub, stub_o, "-soname", "libmain.so"],
                    zig + ["ld.lld", "-shared", "-o", lib + ".tmp", "@" + rsp, stub, "-soname",
                           "libgame-%s.so" % game_id, "--gc-sections", "-z", "noexecstack"]):
            p = spawn(cmd, log, env)
            st = p.wait()
            reap(p)
            if st != 0:
                fail("linking failed: see " + os.path.join(logs, "link.log"))
    if abi_id().encode() not in open(lib + ".tmp", "rb").read():
        fail("the library came out without its ABI id: something is wrong with this builder")
    os.replace(lib + ".tmp", lib)


def push(lib):
    adb = shutil.which("adb")
    if not adb:
        fail("adb wasn't found (Android's platform tools): copy %s to the phone yourself" % lib)
    dst = APP_DIR + "/game"
    say("Copying it to the phone by adb (%s)" % dst)
    subprocess.run([adb, "shell", "mkdir", "-p", dst], check=False)
    if subprocess.run([adb, "push", lib, dst + "/"]).returncode != 0:
        fail("adb push failed: is the phone connected, with USB debugging allowed?")


def main():
    ap = argparse.ArgumentParser(description="Build ZLAA's game code from your own Link's Awakening dump.")
    ap.add_argument("game", nargs="?", help="your dump's folder (the one with exefs and romfs), or its exefs")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "output") if find("../python", "../zig") else
                    os.path.join(os.getcwd(), "zlaa-output"), help="where libgame-<id>.so goes")
    ap.add_argument("--work", help="where the translated C (~700 MB) and the compiled files go while it works")
    ap.add_argument("--jobs", "-j", type=int, help="compiles at a time (default: 3 of every 4 cores, as memory allows)")
    ap.add_argument("--keep", action="store_true", help="keep the work folder afterwards")
    ap.add_argument("--push", action="store_true", help="copy the result to the phone by adb")
    args = ap.parse_args()
    if not SOURCES["recompiler"] or not SOURCES["runtime"]:
        fail("the recompiler or the runtime headers are missing beside this script: unpack the whole package")

    say("ZLAA Builder\n")
    exefs = find_exefs(args.game or pick_folder())
    game_id = build_id(exefs)
    version = VERSIONS.get(game_id)
    if not version:
        fail("this isn't a version of Link's Awakening that ZLAA knows (main build id %s...; ZLAA knows 1.0.0, 1.0.1 "
             "and 1.1.0)" % game_id)
    say("Game files: %s\nVersion:    Link's Awakening %s (build id %s)" % (exefs, version, game_id))

    out = os.path.abspath(args.out)
    work = os.path.abspath(args.work or os.path.join(out, "work"))
    gen, obj, logs = (os.path.join(work, n) for n in ("gen", "obj", "logs"))
    for d in (out, work, logs):
        os.makedirs(d, exist_ok=True)
    free = shutil.disk_usage(work).free
    if free < 2e9:
        fail("%s has %.1f GB free; the build needs about 2 GB while it works" % (work, free / 1e9))
    cpus = os.cpu_count() or 4
    # (by default three of every four cores, with 4 GB of memory left: the PC stays usable while it works)
    jobs = args.jobs or max(1, min(cpus * 3 // 4, int((memory_bytes() - 4e9) / JOB_MEMORY)))
    stamp = "%s %s" % (game_id, tools_id())
    zig = zig_command()
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(work, "zigcache"),
               ZIG_LOCAL_CACHE_DIR=os.path.join(work, "zigcache"))
    if not WINDOWS:
        os.nice(5)
    say("Working in: %s\n" % work)

    t0 = time.time()
    try:
        translate(exefs, gen, stamp, logs)
        objects = compile_all(zig, gen, obj, stamp, jobs, env, logs)
        lib = os.path.join(out, "libgame-%s.so" % game_id)
        link(zig, objects, work, lib, game_id, env, logs)
    except KeyboardInterrupt:
        stop_all()
        say("\nStopped. Run it again to carry on where it stopped.")
        sys.exit(1)
    say("\nDone in %s: %s (%.0f MB)" % (fmt_time(time.time() - t0), lib, os.path.getsize(lib) / 1e6))
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    if args.push:
        push(lib)
        say("Done. Start ZLAA: it takes the game code from there.")
        return
    say("\nAll done! Now:\n"
        "  1. Copy %s to your phone's Download folder.\n"
        "  2. Start ZLAA. The start screen shows Ready: press Play.\n"
        "ZLAA copies the file to its own storage, so you can delete it from Download afterwards."
        % os.path.basename(lib))
    if WINDOWS:                                      # (the file shown in Explorer, ready to copy)
        subprocess.run(["explorer", "/select,", lib])

if __name__ == "__main__":
    main()
