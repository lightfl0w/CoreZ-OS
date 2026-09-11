from __future__ import annotations

import argparse
import io
import os
import shlex
import shutil
import subprocess
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator, List, Optional, Sequence, Tuple
def _force_utf8_stdout() -> None:
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        if stream is None:
            continue
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, io.UnsupportedOperation):
            try:
                setattr(sys, stream_name,
                        io.TextIOWrapper(stream.buffer, encoding="utf-8",
                                         errors="replace"))
            except Exception:
                pass
_force_utf8_stdout()
ROOT       = Path(__file__).resolve().parent
BOOT_DIR   = ROOT / "arch" / "x86" / "boot"
APPS_DIR   = ROOT / "apps"
KERNEL_DIR = ROOT / "kernel"
BUILD_DIR  = ROOT / "build"
LINKER_DIR = ROOT / "linker"
SCRIPTS    = ROOT / "scripts"
MUSL_SRC   = ROOT / "third_modules" / "musl"
MUSL_ARCH  = "x86_64"
SHELL_SRC  = ROOT / "third_modules" / "mr_micro_shell"
NRSHELL    = APPS_DIR / "nr_shell"
CFLAGS_BASE = [
    "-ffreestanding", "-fno-builtin", "-fno-sanitize=all",
    "-I", str(ROOT / "includes"),
]
CFLAGS = CFLAGS_BASE
KERNEL_CFLAGS = CFLAGS_BASE + [
    "-mcmodel=large",
    "-mno-red-zone",
    "-mstackrealign",
    "-fstack-protector-strong",
    "-Wall", "-Wunused-function", "-Wunused-variable",
]
UP_CFLAGS = CFLAGS_BASE + [
    "-I", str(ROOT / "includes" / "libc" / "user"),
    "-I", str(ROOT / "includes" / "lib"),
    "-I", str(ROOT / "includes"),
]
UP_LDFLAGS = ["-s", "-m", "elf_i386", "-T", str(ROOT / "linker" / "user.ld"), "-e", "_start"]

UP_CFLAGS_64 = [
    "-ffreestanding", "-fno-builtin", "-fno-sanitize=all", "-fPIE",
    "-fstack-protector-strong",
    "-I", str(ROOT / "includes" / "libc" / "user"),
    "-I", str(ROOT / "includes" / "lib"),
    "-I", str(ROOT / "includes"),
]
UP_LDFLAGS_64 = ["-s", "-m", "elf_x86_64", "-T", str(ROOT / "linker" / "user.ld"),
                 "-e", "_start", "-static", "-pie", "--no-dynamic-linker",
                 "-z", "pack-relative-relocs"]

MUSL64_BASE = [
    "-ffreestanding", "-fno-builtin", "-fno-sanitize=all", "-fPIE",
]
MUSL_CFLAGS = MUSL64_BASE + [
    "-I", str(MUSL_SRC / "arch" / "x86_64"),
    "-I", str(MUSL_SRC / "arch" / "generic"),
    "-I", str(MUSL_SRC / "src" / "internal"),
    "-I", str(MUSL_SRC / "include"),
]

MUSL_PREFIX = BUILD_DIR / "musl"
MUSL_INC   = MUSL_PREFIX / "include"
MUSL_LIB   = MUSL_PREFIX / "lib"
MUSL_DEMO_CFLAGS = MUSL64_BASE + ["-fstack-protector-strong", "-I", str(MUSL_INC)]
LC_CFLAGS = MUSL64_BASE + ["-I", str(ROOT / "includes")]
class Ansi:
    RESET   = "\x1b[0m"
    BOLD    = "\x1b[1m"
    DIM     = "\x1b[2m"
    ITALIC  = "\x1b[3m"
    UNDER   = "\x1b[4m"
    REV     = "\x1b[7m"
    RED     = "\x1b[31m"
    GREEN   = "\x1b[32m"
    YELLOW  = "\x1b[33m"
    BLUE    = "\x1b[34m"
    MAGENTA = "\x1b[35m"
    CYAN    = "\x1b[36m"
    GRAY    = "\x1b[90m"
    BR_RED  = "\x1b[91m"
    BR_GRN  = "\x1b[92m"
    BR_YEL  = "\x1b[93m"
    BR_BLU  = "\x1b[94m"
    BR_MAG  = "\x1b[95m"
    BR_CYN  = "\x1b[96m"
    BR_WHT  = "\x1b[97m"
    CURSOR_HIDE = "\x1b[?25l"
    CURSOR_SHOW = "\x1b[?25h"
    ERASE_LINE  = "\x1b[2K"
    CR          = "\r"
def _enable_vt_on_windows() -> None:
    if os.name != "nt":
        return
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        for handle_id in (-11, -12):
            handle = kernel32.GetStdHandle(handle_id)
            mode = ctypes.c_uint32()
            if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
                kernel32.SetConsoleMode(handle, mode.value | 0x4)
    except Exception:
        pass
def color_enabled(no_color_flag: bool, stream=sys.stdout) -> bool:
    if no_color_flag:
        return False
    if not hasattr(stream, "isatty") or not stream.isatty():
        return True
    return True
@dataclass
class Tools:
    nasm:    str
    cc:      List[str]
    ld:      str
    objcopy: str
    python:  str
    is_zig:  bool = False
def _find(name: str, candidates: Sequence[str]) -> str:
    for c in candidates:
        path = shutil.which(c)
        if path is None:
            continue
        if os.name == "nt":
            base, ext = os.path.splitext(path)
            if ext.lower() != ".exe":
                exe_path = base + ".exe"
                if os.path.exists(exe_path):
                    path = exe_path
        return path
    raise FileNotFoundError(
        f"Could not find any of: {', '.join(candidates)} (needed for `{name}`)."
    )
def _resolve_cc() -> Tuple[List[str], bool]:
    zig = shutil.which("zig")
    if zig:
        return [zig, "cc"], True
    for c in ("x86_64-elf-gcc", "gcc", "cc", "i686-elf-gcc"):
        path = shutil.which(c)
        if path:
            return [path], False
    raise FileNotFoundError(
        "Could not find any C compiler (zig, x86_64-elf-gcc, gcc, cc)."
    )
def detect_tools() -> Tools:
    cc, is_zig = _resolve_cc()
    return Tools(
        nasm    = _find("nasm",    ["nasm"]),
        cc      = cc,
        ld      = _find("ld",      ["ld.lld", "lld-link", "x86_64-elf-ld", "ld"]),
        objcopy = _find("objcopy", ["objcopy", "llvm-objcopy", "x86_64-elf-objcopy"]),
        python  = sys.executable,
        is_zig  = is_zig,
    )
@dataclass
class CmdResult:
    returncode: int
    stdout: str
    stderr: str
    duration: float
    @property
    def ok(self) -> bool:
        return self.returncode == 0
def run(cmd: Sequence[str], cwd: Optional[Path] = None,
        env: Optional[dict] = None, quiet: bool = False) -> CmdResult:
    start = time.perf_counter()
    proc = subprocess.run(
        list(cmd),
        cwd=str(cwd) if cwd else None,
        env={**os.environ, **(env or {})},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    dur = time.perf_counter() - start
    return CmdResult(proc.returncode, proc.stdout, proc.stderr, dur)
class Console:
    def __init__(self, use_color: bool):
        self.use_color = use_color
    def _c(self, code: str) -> str:
        return code if self.use_color else ""
    def write(self, s: str) -> None:
        sys.stdout.write(s)
        sys.stdout.flush()
    def writeln(self, s: str = "") -> None:
        self.write(s + "\n")
    def banner(self, version: str) -> None:
        c = self
        bar = "═" * 70
        c.writeln()
        c.writeln(f"{c._c(Ansi.CYAN)}{c._c(Ansi.BOLD)}{bar}{c._c(Ansi.RESET)}")
        c.writeln(f"{c._c(Ansi.BR_CYN)}{c._c(Ansi.BOLD)}  CoreZ OS   "
                  f"{c._c(Ansi.DIM)}{c._c(Ansi.GRAY)}build system  "
                  f"{c._c(Ansi.RESET)}{c._c(Ansi.DIM)}·  {version}{c._c(Ansi.RESET)}")
        c.writeln(f"{c._c(Ansi.CYAN)}{c._c(Ansi.BOLD)}{bar}{c._c(Ansi.RESET)}")
        c.writeln()
    def step_header(self, idx: int, total: int, title: str, hint: str = "") -> None:
        c = self
        bullet = f"{c._c(Ansi.BR_YEL)}▶{c._c(Ansi.RESET)}"
        idx_str = f"{c._c(Ansi.GRAY)}[{idx}/{total}]{c._c(Ansi.RESET)}"
        title_str = f"{c._c(Ansi.BOLD)}{c._c(Ansi.BR_WHT)}{title}{c._c(Ansi.RESET)}"
        hint_str = (f"  {c._c(Ansi.DIM)}{c._c(Ansi.GRAY)}{hint}{c._c(Ansi.RESET)}" if hint else "")
        c.writeln(f"  {bullet} {idx_str}  {title_str}{hint_str}")
        c.writeln(f"  {c._c(Ansi.GRAY)}{'─' * 66}{c._c(Ansi.RESET)}")
    def ok(self, msg: str) -> None:
        self.writeln(f"      {self._c(Ansi.GREEN)}✓{self._c(Ansi.RESET)}  {msg}")
    def info(self, msg: str) -> None:
        self.writeln(f"      {self._c(Ansi.CYAN)}·{self._c(Ansi.RESET)}  {msg}")
    def warn(self, msg: str) -> None:
        self.writeln(f"      {self._c(Ansi.YELLOW)}!{self._c(Ansi.RESET)}  {msg}")
    def fail(self, msg: str) -> None:
        self.writeln(f"      {self._c(Ansi.RED)}✗{self._c(Ansi.RESET)}  {msg}")
    @contextmanager
    def progress(self, total: int, label: str, color: str = Ansi.BR_CYN):
        c = self
        width = 38
        started = time.perf_counter()
        state = {"done": 0, "msg": ""}
        def render() -> None:
            pct = (state["done"] / total) if total else 1.0
            fill = int(round(width * pct))
            bar = "█" * fill + "░" * (width - fill)
            elapsed = time.perf_counter() - started
            eta = (elapsed / state["done"]) * (total - state["done"]) if state["done"] else 0.0
            tail = state["msg"][:40]
            sys.stdout.write(
                f"\r      {c._c(color)}▐{bar}▌{c._c(Ansi.RESET)} "
                f"{c._c(Ansi.BOLD)}{pct*100:5.1f}%{c._c(Ansi.RESET)} "
                f"{c._c(Ansi.GRAY)}{state['done']:>3}/{total:<3} "
                f"{c._c(Ansi.DIM)}{tail:<40} "
                f"{c._c(Ansi.GRAY)}elap {elapsed:5.1f}s eta {eta:4.1f}s{c._c(Ansi.RESET)}"
            )
            sys.stdout.flush()
        def update(done: int, msg: str = "") -> None:
            state["done"] = done
            state["msg"] = msg
            render()
        c.write(c._c(Ansi.CURSOR_HIDE))
        try:
            update(0, label)
            yield update
        finally:
            update(total, label)
            sys.stdout.write("\n")
            sys.stdout.flush()
            c.write(c._c(Ansi.CURSOR_SHOW))
    def summary(self, rows: Sequence[Tuple[str, str, str]]) -> None:
        c = self
        c.writeln()
        import json
        with open(ROOT / "compile_commands.json", "w", encoding="utf-8") as f:
            json.dump(COMPILE_COMMANDS, f, indent=1)
        c.writeln(f"  {c._c(Ansi.BOLD)}{c._c(Ansi.BR_WHT)}Summary{c._c(Ansi.RESET)}")
        c.writeln(f"  {c._c(Ansi.GRAY)}{'─' * 66}{c._c(Ansi.RESET)}")
        for label, value, color in rows:
            c.writeln(f"      {c._c(Ansi.GRAY)}{label:<14}{c._c(Ansi.RESET)}"
                      f" {c._c(color)}{c._c(Ansi.BOLD)}{value}{c._c(Ansi.RESET)}")
        c.writeln()
@dataclass
class Task:
    name: str
    cmd:  List[str]
    cwd:  Optional[Path] = None
    out:  Optional[Path] = None
    deps: List[Task] = field(default_factory=list)
    description: str = ""
    group: str = ""
    env:  Optional[dict] = None
    optional: bool = False
    def is_stale(self) -> bool:
        if not self.out:
            return True
        if not self.out.exists():
            return True
        out_mtime = self.out.stat().st_mtime
        for d in self.deps:
            if d.is_stale():
                return True
            for path in d.dep_paths():
                if path.exists() and path.stat().st_mtime > out_mtime:
                    return True
        return False
    def dep_paths(self) -> Iterable[Path]:
        base = self.cwd or Path.cwd()
        for d in self.deps:
            if isinstance(d, Path):
                if d.exists():
                    yield d
                continue
        for tok in self.cmd:
            if tok.startswith("-"):
                continue
            p = Path(tok)
            if not p.is_absolute():
                p = base / p
            if p.exists() and p.is_file():
                yield p
        if self.out is not None and any(t == "-c" for t in self.cmd):
            dfile = self.out.with_suffix(".d")
            if dfile.exists():
                text = dfile.read_text(errors="replace")
                for line in text.splitlines():
                    if ":" not in line:
                        continue
                    for tok in line.split(":", 1)[1].replace("\\\n", " ").split():
                        if not tok:
                            continue
                        p = Path(tok)
                        if not p.is_absolute():
                            p = base / p
                        if p.exists() and p.is_file():
                            yield p
@dataclass
class BuildStats:
    compiled: int = 0
    cache_hit: int = 0
    failed:   int = 0
    timings: dict = field(default_factory=dict)
def task_assemble_bin(name: str, src: Path, out: Path, tools: Tools) -> Task:
    return Task(
        name=name, cmd=[tools.nasm, "-f", "bin", str(src), "-o", str(out)],
        out=out, deps=[], description=str(src.relative_to(ROOT)),
        group="asm",
    )
def task_assemble_elf(name: str, src: Path, out: Path, tools: Tools) -> Task:
    return Task(
        name=name, cmd=[tools.nasm, "-f", "elf32", str(src), "-o", str(out)],
        out=out, deps=[], description=str(src.relative_to(ROOT)),
        group="asm",
    )
def task_assemble_elf64(name: str, src: Path, out: Path, tools: Tools) -> Task:
    return Task(
        name=name, cmd=[tools.nasm, "-f", "elf64", str(src), "-o", str(out)],
        out=out, deps=[], description=str(src.relative_to(ROOT)),
        group="asm",
    )
COMPILE_COMMANDS = []

def task_cc(name: str, src: Path, out: Path, tools: Tools, flags: List[str], target: str = "x86_64-freestanding") -> Task:
    cmd = [*tools.cc]
    if tools.is_zig and target:
        cmd.append(f"--target={target}")
        if any(f.startswith("-fstack-protector") for f in flags):
            cmd += ["-nostdinc", "-lc"]
    cmd += ["-c", str(src), *flags, "-MMD", "-MP", "-o", str(out)]
    COMPILE_COMMANDS.append({
        "directory": str(ROOT),
        "arguments": cmd,
        "file": str(src),
    })
    return Task(
        name=name,
        cmd=cmd,
        out=out, deps=[], description=str(src.relative_to(ROOT)),
        group="cc",
    )
def task_link(name: str, out: Path, tools: Tools,
              objs: Sequence[Path], script: Optional[Path] = None,
              flags: Sequence[str] = ()) -> Task:
    cmd = [tools.ld]
    if script:
        cmd += ["-T", str(script)]
    cmd += [*flags, "-o", str(out), *map(str, objs)]
    return Task(name=name, cmd=cmd, cwd=BUILD_DIR, out=out,
                deps=[], description=f"link → {out.name}", group="link")
def task_objcopy_binary(name: str, src_elf: Path, out: Path, tools: Tools,
                        symbol: str, elf_arch: str = "i386",
                        out_fmt: str = "elf32-i386", out_arch: str = "i386") -> Task:
    return Task(
        name=name,
        cmd=[tools.objcopy, "-I", "binary", "-O", out_fmt, "-B", out_arch,
             src_elf.name, out.name],
        cwd=BUILD_DIR, out=out, deps=[],
        description=f"objcopy {symbol}", group="objcopy",
    )
def task_python(name: str, script: Path, args: Sequence[str],
                out: Optional[Path] = None) -> Task:
    return Task(
        name=name,
        cmd=[sys.executable, str(script), *args],
        out=out, deps=[], description=str(script.name), group="python",
    )
def _musl_buildenv(tools: Tools) -> Optional[dict]:
    if os.name == "nt":
        return None
    sh = shutil.which("sh") or shutil.which("bash")
    make = shutil.which("make") or "/usr/bin/make"
    if not sh or not os.path.exists(make):
        return None
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc is None:
        cc = list(tools.cc)
    else:
        cc = [cc]

    if len(cc) > 1:
        wrapper = BUILD_DIR / "musl-cc"
        wrapper.parent.mkdir(parents=True, exist_ok=True)
        body = "#!/bin/sh\nexec " + " ".join(shlex.quote(c) for c in cc) + ' "$@"\n'
        wrapper.write_text(body, encoding="utf-8")
        try:
            os.chmod(wrapper, 0o755)
        except OSError:
            pass
        cc = [str(wrapper)]
    return {"sh": sh, "make": make, "cc": cc}


def make_plan(tools: Tools, with_musl_lib: bool = False):
    tasks: List[Task] = []
    user_elves: List[Task] = []
    tasks.append(task_assemble_bin("boot.bin",
                          BOOT_DIR / "boot.asm", BUILD_DIR / "boot.bin", tools))
    tasks.append(task_assemble_bin("vbr.bin",
                          BOOT_DIR / "vbr.asm", BUILD_DIR / "vbr.bin", tools))
    tasks.append(task_assemble_bin("loader.bin",
                          BOOT_DIR / "loader.asm", BUILD_DIR / "loader.bin", tools))
    for stem in ("func", "io", "stub", "entry", "switch", "idle"):
        tasks.append(task_assemble_elf64(
            f"{stem}.o",
            ROOT / "arch" / "x86" / "asm" / f"{stem}.asm",
            BUILD_DIR / f"{stem}.o", tools,
        ))
    tasks.append(task_assemble_bin(
        "ap_trampoline.bin",
        ROOT / "arch" / "x86" / "asm" / "ap_trampoline.asm",
        BUILD_DIR / "ap_trampoline.bin", tools))
    tasks.append(task_objcopy_binary(
        "ap_tramp.o", BUILD_DIR / "ap_trampoline.bin",
        BUILD_DIR / "ap_tramp.o", tools,
        "_binary_ap_trampoline_bin_start",
        out_fmt="elf64-x86-64", out_arch="i386:x86-64"))
    tasks.append(task_assemble_elf64(
        "up_start.o", APPS_DIR / "start.asm", BUILD_DIR / "up_start.o", tools,
    ))
    kernel_c_sources = [
        ("ioc.o",        ROOT / "drivers" / "char" / "console" / "io.c"),
        ("pit.o",        KERNEL_DIR / "init" / "pit" / "pit.c"),
        ("pic.o",        KERNEL_DIR / "init" / "pic" / "pic.c"),
        ("apic.o",       KERNEL_DIR / "init" / "apic" / "apic.c"),
        ("acpi.o",     KERNEL_DIR / "init" / "acpi" / "acpi.c"),
        ("idt.o",        ROOT / "arch" / "x86" / "interrupt" / "idt.c"),
        ("interrupt.o",  ROOT / "arch" / "x86" / "interrupt" / "interrupt.c"),
        ("kernel.o",     KERNEL_DIR / "init" / "main.c"),
        ("mb2.o",        KERNEL_DIR / "init" / "mb2.c"),
        ("assert.o",     KERNEL_DIR / "init" / "assert.c"),
        ("ssp.o",        KERNEL_DIR / "init" / "ssp.c"),
        ("str.o",        ROOT / "lib" / "str" / "str.c"),
        ("rand.o",       ROOT / "lib" / "rand" / "rand.c"),
        ("rbtree.o",     ROOT / "lib" / "rbtree" / "rbtree.c"),
        ("bitmap.o",     KERNEL_DIR / "mm" / "bitmap" / "bitmap.c"),
        ("pool.o",       KERNEL_DIR / "mm" / "pool" / "pool.c"),
        ("access.o",     KERNEL_DIR / "mm" / "access.c"),
        ("list.o",       ROOT / "lib" / "list" / "list.c"),
        ("thread.o",     KERNEL_DIR / "sched" / "thread.c"),
        ("sync.o",       KERNEL_DIR / "sched" / "sync.c"),
        ("percpu.o",     KERNEL_DIR / "sched" / "percpu.c"),
        ("smp.o",        KERNEL_DIR / "init" / "smp" / "smp.c"),
        ("ioqueue.o",    ROOT / "drivers" / "char" / "ioqueue.c"),
        ("tty.o",        ROOT / "drivers" / "char" / "tty.c"),
        ("keyboard.o",   ROOT / "drivers" / "char" / "keyboard.c"),
        ("ide.o",        ROOT / "drivers" / "block" / "ide.c"),
        ("ext2.o",       KERNEL_DIR / "fs" / "ext2.c"),
        ("fs.o",         KERNEL_DIR / "fs" / "fs.c"),
        ("inode.o",      KERNEL_DIR / "fs" / "inode.c"),
        ("dir.o",        KERNEL_DIR / "fs" / "dir.c"),
        ("file.o",       KERNEL_DIR / "fs" / "file.c"),
        ("proc.o",       KERNEL_DIR / "fs" / "proc.c"),
        ("gdt.o",        KERNEL_DIR / "init" / "gdt" / "gdt.c"),
        ("tss.o",        KERNEL_DIR / "init" / "tss" / "tss.c"),
        ("process.o",    KERNEL_DIR / "userprog" / "process.c"),
        ("exec.o",       KERNEL_DIR / "userprog" / "exec.c"),
        ("shell.o",      KERNEL_DIR / "shell" / "shell.c"),
        ("buildin_cmd.o",KERNEL_DIR / "shell" / "buildin_cmd.c"),
        ("pipe.o",       KERNEL_DIR / "shell" / "pipe.c"),
        ("ksyscall.o",   KERNEL_DIR / "syscall" / "syscall.c"),
        ("signal.o",     KERNEL_DIR / "syscall" / "signal.c"),
        ("file_syscall.o",KERNEL_DIR / "syscall" / "file_syscall.c"),
        ("mmap.o",       KERNEL_DIR / "syscall" / "mmap.c"),
        ("futex.o",      KERNEL_DIR / "syscall" / "futex.c"),
        ("linux_compat.o", KERNEL_DIR / "syscall" / "linux_compat.c"),
        ("usyscall.o",   ROOT / "libc" / "user" / "syscall.c"),
        ("ustdio.o",     ROOT / "libc" / "user" / "stdio.c"),
        ("wait_exit.o",  KERNEL_DIR / "userprog" / "wait_exit.c"),
        ("fork.o",       KERNEL_DIR / "userprog" / "fork.c"),
        ("clone.o",      KERNEL_DIR / "userprog" / "clone.c"),
        ("mouse.o",      ROOT / "drivers" / "char" / "mouse.c"),
        ("gfx.o",        KERNEL_DIR / "gui" / "gfx.c"),
        ("font.o",       KERNEL_DIR / "gui" / "font.c"),
        ("shm.o",        KERNEL_DIR / "gui" / "shm.c"),
        ("guiserver.o",  KERNEL_DIR / "gui" / "server.c"),
        ("layout.o",     KERNEL_DIR / "gui" / "layout.c"),
        ("wm.o",         KERNEL_DIR / "gui" / "wm.c"),
        ("guiclients.o", KERNEL_DIR / "gui" / "clients.c"),
        ("gui.o",        KERNEL_DIR / "gui" / "gui.c"),
    ]

    tasks.append(Task(
        name="musl-headers",
        cmd=[sys.executable, str(SCRIPTS / "gen_musl_headers.py"),
             "--src", str(MUSL_SRC), "--out", str(MUSL_INC), "--arch", MUSL_ARCH],
        out=MUSL_INC / "bits" / "alltypes.h",
        group="musl-headers",
        description="assemble musl headers into build/musl/include (cross-platform)"))
    for stem, src in kernel_c_sources:
        tasks.append(task_cc(stem, src, BUILD_DIR / stem, tools, KERNEL_CFLAGS))
    user_programs = [
        ("prog_no_arg", "prog_no_arg.c", "_start", []),
        ("prog_arg",    "prog_arg.c",    "_start", []),
        ("cat",         "cat.c",         "_start", []),
        ("fork_demo",   "fork_demo.c",   "_start", []),
        ("orphan",      "orphan_demo.c", "_start", []),
        ("tls_test",    "tls_test.c",    "_start", []),
        ("sig_test",    "sig_test.c",    "_start", []),
        ("badptr_test", "badptr_test.c", "_start", []),
        ("cwd_test",    "cwd_test.c",    "_start", []),
        ("echocat",     "echocat.c",     "_start", []),
        ("prog_pipe",   "prog_pipe.c",   "_start", []),
        ("font_demo",   "font_demo.c",   "_start", ["-Os"]),
        ("heap_demo",   "heap_demo.c",   "_start", []),
        ("signal_demo", "signal_demo.c", "_start", []),
        ("mmap_demo",   "mmap_demo.c",   "_start", []),
        ("mmap2_demo",  "mmap2_demo.c",  "_start", []),
        ("dev_demo",    "dev_demo.c",    "_start", []),
        ("dev_demo",    "dev_demo.c",    "_start", []),
        ("futex_demo",  "futex_demo.c",  "_start", []),
        ("fsyscall_demo","fsyscall_demo.c","_start", []),
        ("clone_demo",  "clone_demo.c",  "_start", []),
        ("ping",        "ping.c",        "_start", []),
        ("udp_echo",    "udp_echo.c",    "_start", []),
        ("cow_stress",  "cow_stress.c",  "_start", []),
        ("canary_test", "canary_test.c", "_start", []),
    ]
    user_lib_sources = [
        (ROOT / "libc" / "user", "stdio.c",   "up_stdio.o"),
        (ROOT / "libc" / "user", "syscall.c", "up_syscall.o"),
        (ROOT / "lib" / "str",  "str.c",     "up_str.o"),
        (ROOT / "lib" / "rbtree", "rbtree.c",  "up_rbtree.o"),
        (ROOT / "libc" / "user", "stdlib.c",  "up_stdlib.o"),
        (ROOT / "libc" / "user", "ssp.c",     "up_ssp.o"),
    ]
    def compile_user_lib(out_dir: Path) -> List[Path]:
        outs = []
        for _dir, fname, oname in user_lib_sources:
            src = _dir / fname
            obj = out_dir / oname
            tasks.append(task_cc(oname, src, obj, tools, UP_CFLAGS_64))
            outs.append(obj)
        return outs
    lib_objs = compile_user_lib(BUILD_DIR)
    for prog_name, src_c, entry_flag, opt_flags in user_programs:
        nick_map = {"prog_no_arg": "up_no_arg", "prog_arg": "up_arg",
                    "cat": "up_cat", "fork_demo": "up_fork",
                    "prog_pipe": "up_pipe", "font_demo": "up_font",
                    "heap_demo": "up_heap_demo", "signal_demo": "up_signal",
                    "mmap_demo": "up_mmap_demo", "mmap2_demo": "up_mmap2_demo",
                    "futex_demo": "up_futex_demo", "fsyscall_demo": "up_fsyscall_demo",
                    "clone_demo": "up_clone_demo"}
        nick = nick_map.get(prog_name, f"up_{prog_name}")
        prog_obj = BUILD_DIR / f"{nick}.o"
        tasks.append(task_cc(nick, APPS_DIR / src_c, prog_obj, tools,
                             UP_CFLAGS_64 + opt_flags))
        elf_flags = list(UP_LDFLAGS_64)
        if entry_flag:
            elf_flags[elf_flags.index("-e") + 1] = entry_flag
        elf = BUILD_DIR / f"{prog_name}.elf"
        elf_task = task_link(
            f"{prog_name}.elf", elf, tools,
            [BUILD_DIR / "up_start.o", prog_obj, *lib_objs],
            flags=elf_flags,
        )
        tasks.append(elf_task)
        user_elves.append(elf_task)
    tasks.append(task_assemble_elf64(
        "lc_start.o", APPS_DIR / "lc_crt0.asm", BUILD_DIR / "lc_start.o", tools,
    ))
    lc_libc = task_cc("lc_libc.o", ROOT / "libc" / "compat" / "lc_libc.c",
                      BUILD_DIR / "lc_libc.o", tools, LC_CFLAGS)
    tasks.append(lc_libc)
    lc_demo_c = task_cc("lc_demo.o", APPS_DIR / "lc_demo.c",
                        BUILD_DIR / "lc_demo.o", tools, LC_CFLAGS)
    tasks.append(lc_demo_c)
    lc_elf = task_link("lc_demo.elf", BUILD_DIR / "lc_demo.elf", tools,
                       [BUILD_DIR / "lc_start.o", BUILD_DIR / "lc_demo.o",
                        BUILD_DIR / "lc_libc.o"],
       flags=["-s", "-m", "elf_x86_64", "-T", str(ROOT / "linker" / "user.ld"),
              "-e", "_lc_start", "-static", "-pie", "--no-dynamic-linker",
              "-z", "pack-relative-relocs"])
    tasks.append(lc_elf)
    user_elves.append(lc_elf)
    gui_launch_c = task_cc("gui_launch.o", APPS_DIR / "gui_launch.c",
                           BUILD_DIR / "gui_launch.o", tools, LC_CFLAGS)
    tasks.append(gui_launch_c)
    gui_elf = task_link(
        "gui.elf", BUILD_DIR / "gui.elf", tools,
        [BUILD_DIR / "lc_start.o", BUILD_DIR / "gui_launch.o",
         BUILD_DIR / "lc_libc.o"],
        flags=["-s", "-m", "elf_x86_64", "-T", str(ROOT / "linker" / "user.ld"),
               "-e", "_lc_start", "-static", "-pie", "--no-dynamic-linker",
               "-z", "pack-relative-relocs"])
    tasks.append(gui_elf)
    user_elves.append(gui_elf)
    shell_cflags = UP_CFLAGS_64 + [
        "-I", str(SHELL_SRC / "inc"),
        "-I", str(NRSHELL),
    ]
    shell_objs = [
        ("shell_core.o",    SHELL_SRC / "src" / "nr_micro_shell_core.c"),
        ("shell_cmds.o",    SHELL_SRC / "src" / "nr_micro_shell_cmds.c"),
        ("nr_shell_main.o", NRSHELL / "nr_shell_main.c"),
    ]
    for stem, src in shell_objs:
        tasks.append(task_cc(stem, src, BUILD_DIR / stem, tools, shell_cflags))

    nr_shell_elf = task_link("nr_shell.elf", BUILD_DIR / "nr_shell.elf", tools,
        [BUILD_DIR / "up_start.o",
         BUILD_DIR / "shell_core.o", BUILD_DIR / "shell_cmds.o",
         BUILD_DIR / "nr_shell_main.o", *lib_objs],
        flags=["-s", "-m", "elf_x86_64", "-T", str(ROOT / "linker" / "user.ld"), "-e", "_start",
               "-static", "-pie", "--no-dynamic-linker",
               "-z", "pack-relative-relocs"])
    tasks.append(nr_shell_elf)
    user_elves.append(nr_shell_elf)
    net_dir = ROOT / "drivers" / "net"
    net_cflags = KERNEL_CFLAGS + ["-I", str(net_dir)]
    net_c_sources = [
        ("rtl8139.o", net_dir / "rtl8139.c"),
        ("e1000.o", net_dir / "e1000.c"),
        ("arp.o", net_dir / "arp.c"),
        ("ip.o", net_dir / "ip.c"),
        ("eth.o", net_dir / "eth.c"),
        ("icmp.o", net_dir / "icmp.c"),
        ("tcp.o", net_dir / "tcp.c"),
        ("udp.o", net_dir / "udp.c"),
        ("socket.o", net_dir / "socket.c"),
        ("net.o", net_dir / "net.c"),
    ]
    for stem, src in net_c_sources:
        tasks.append(task_cc(stem, src, BUILD_DIR / stem, tools, net_cflags))
    font_src = ROOT / "lib" / "assets" / "font.ttf"
    font_kernel_ttf = BUILD_DIR / "font_kernel.ttf"
    tasks.append(task_python(
        "font_kernel.ttf",
        SCRIPTS / "make_font_subset.py",
        [str(font_src), str(font_kernel_ttf), "--charset=latin"],
        out=font_kernel_ttf,
    ))
    font_subset = BUILD_DIR / "font_subset.ttf"
    tasks.append(task_python(
        "font_subset.ttf",
        SCRIPTS / "make_font_subset.py",
        [str(font_src), str(font_subset), "--charset=gb2312-l1"],
        out=font_subset,
    ))
    tasks.append(Task(
        name="font_kernel.o",
        cmd=[tools.objcopy, "-I", "binary", "-O", "elf64-x86-64",
             "-B", "i386:x86-64", "--set-section-alignment", ".data=64",
             "font_kernel.ttf", "font_kernel.o"],
        cwd=BUILD_DIR, out=BUILD_DIR / "font_kernel.o", deps=[],
        description="embed font_kernel.ttf", group="objcopy",
    ))
    kernel_objs_names = [
        "entry.o", "kernel.o", "mb2.o", "func.o", "ioc.o", "io.o", "idle.o", "acpi.o",
        "apic.o", "pit.o", "stub.o", "idt.o", "interrupt.o", "pic.o",
        "assert.o", "ssp.o", "str.o", "rand.o", "rbtree.o", "bitmap.o", "pool.o", "access.o", "list.o",
        "switch.o", "thread.o", "sync.o", "percpu.o", "smp.o",
        "ap_tramp.o", "ioqueue.o", "tty.o", "keyboard.o",
        "ide.o", "ext2.o", "fs.o", "inode.o", "dir.o", "file.o", "proc.o",
        "gdt.o", "tss.o", "process.o", "exec.o", "shell.o",
        "buildin_cmd.o", "pipe.o", "ksyscall.o", "mmap.o", "futex.o",
        "linux_compat.o", "signal.o", "file_syscall.o",
        "usyscall.o", "ustdio.o", "wait_exit.o", "fork.o", "clone.o",
        "mouse.o", "gfx.o", "font.o", "font_kernel.o", "shm.o", "guiserver.o",
        "layout.o", "wm.o", "guiclients.o", "gui.o",
        "rtl8139.o", "e1000.o", "arp.o", "ip.o", "eth.o", "icmp.o",
        "tcp.o", "udp.o", "socket.o", "net.o",
    ]
    kernel_link_objs = [BUILD_DIR / n for n in kernel_objs_names]
    kernel_elf = BUILD_DIR / "kernel.elf"
    tasks.append(task_link(
        "kernel.elf", kernel_elf, tools, kernel_link_objs,
        script=LINKER_DIR / "kernel.ld",
    ))
    kernel_bin = BUILD_DIR / "kernel.bin"
    tasks.append(Task(
        name="kernel.bin",
        cmd=[tools.objcopy, "-O", "binary", str(kernel_elf), str(kernel_bin)],
        out=kernel_bin, deps=[], description="strip ELF → flat binary",
        group="objcopy",
    ))
    floppy_img = BUILD_DIR / "floppy.img"
    tasks.append(task_python(
        "floppy.img",
        SCRIPTS / "mkfloppy.py",
        [str(BUILD_DIR / "boot.bin"),
         str(BUILD_DIR / "loader.bin"),
         str(BUILD_DIR / "kernel.bin"),
         str(floppy_img)],
        out=floppy_img,
    ))

    musl_env = _musl_buildenv(tools) if with_musl_lib else None
    plan_musl_enabled = musl_env is not None
    if plan_musl_enabled:
        sh, make, cc = musl_env["sh"], musl_env["make"], musl_env["cc"]
        cc_str = " ".join(cc)

        musl_script = (
            "set -e; set -o pipefail; "
            f"rm -rf {shlex.quote(str(MUSL_PREFIX))}; "
            f"mkdir -p {shlex.quote(str(MUSL_PREFIX))}; "
            f"cd {shlex.quote(str(MUSL_SRC))}; "
            f"rm -rf obj config.mak; "
            f"mkdir -p obj/include/bits; "
            f"sed -f tools/mkalltypes.sed arch/x86_64/bits/alltypes.h.in "
            f"include/alltypes.h.in > obj/include/bits/alltypes.h; "
            f"CC={shlex.quote(cc_str)} "
            f"./configure --prefix={shlex.quote(str(MUSL_PREFIX))} "
            f"--disable-shared --enable-static --disable-option-checking; "
            f"make -j\"$(nproc 2>/dev/null || echo 4)\" "
            f"2>&1 | tee {shlex.quote(str(MUSL_PREFIX / 'build.log'))}; "
            f"make install"
        )
        tasks.append(Task(
            name="musl-native-lib",
            cmd=[sh, "-c", musl_script],
            out=MUSL_PREFIX / "lib" / "libc.a",
            optional=True,
            group="musl-lib",
            description="configure+make+install native musl 1.2.6",
        ))

        TOYBOX_DIR = ROOT / "third_modules" / "toybox"
        _musl_gcc = MUSL_PREFIX / "bin" / "musl-gcc"
        _toybox_pre = f"test -x {shlex.quote(str(_musl_gcc))} || exit 0; "
        tasks.append(Task(
            name="toybox-config",
            cmd=[sh, "-c",
                 _toybox_pre +
                 f"cd {shlex.quote(str(TOYBOX_DIR))} && "
                 f"[ -f .config ] || make defconfig >/dev/null 2>&1; "
                 f"make oldconfig >/dev/null 2>&1; true"],
            out=TOYBOX_DIR / ".config",
            deps=[TOYBOX_DIR / "Makefile"],
            optional=True, group="toybox",
            description="toybox defconfig",
        ))
        tasks.append(Task(
            name="toybox-abitag",
            cmd=[sh, "-c",
                 _toybox_pre +
                 f"{shlex.quote(str(_musl_gcc))} -c "
                 f"{shlex.quote(str(TOYBOX_DIR / 'abitag.c'))} -o "
                 f"{shlex.quote(str(TOYBOX_DIR / 'abitag.o'))}"],
            out=TOYBOX_DIR / "abitag.o",
            deps=[TOYBOX_DIR / "abitag.c"],
            optional=True, group="toybox",
            description="toybox GNU ABI-tag note",
        ))
        _toybox_ld = shlex.quote("-static " +
                                 str(TOYBOX_DIR / "abitag.o") +
                                 " -Wl,-Ttext-segment=0x8048000")
        tasks.append(Task(
            name="toybox-build",
            cmd=[sh, "-c",
                 _toybox_pre +
                 f"cd {shlex.quote(str(TOYBOX_DIR))} && "
                 f"CC={shlex.quote(str(_musl_gcc))} "
                 f"CFLAGS={shlex.quote('-static -Os')} "
                 f"LDFLAGS={_toybox_ld} "
                 f"make -j4 > toybox.log 2>&1 || "
                 f"(tail -20 toybox.log; false) && "
                 f"cp toybox {shlex.quote(str(BUILD_DIR / 'toybox'))} && "
                 f"cp toybox {shlex.quote(str(BUILD_DIR / 'suidsh'))}"],
            out=BUILD_DIR / "toybox",
            deps=[TOYBOX_DIR / ".config", TOYBOX_DIR / "toybox"],
            optional=True, group="toybox",
            description="build toybox (static musl)",
        ))

        musl_demo_c = task_cc("musl_demo.o", APPS_DIR / "musl_demo.c",
                              BUILD_DIR / "musl_demo.o", tools, MUSL_DEMO_CFLAGS)
        musl_demo_c.optional = True
        tasks.append(musl_demo_c)
        libc_tests_main = task_cc("libc_tests_main.o", APPS_DIR / "libc_tests_main.c",
                                  BUILD_DIR / "libc_tests_main.o", tools, MUSL_DEMO_CFLAGS)
        libc_tests_main.optional = True
        tasks.append(libc_tests_main)
        tests_sources = [
            ("test_string.o", ROOT / "third_modules" / "libc-testsuite" / "string.c"),
            ("test_qsort.o", ROOT / "third_modules" / "libc-testsuite" / "qsort.c"),
            ("test_strtol.o", ROOT / "third_modules" / "libc-testsuite" / "strtol.c"),
            ("test_strtod.o", ROOT / "third_modules" / "libc-testsuite" / "strtod.c"),
            ("test_basename.o", ROOT / "third_modules" / "libc-testsuite" / "basename.c"),
            ("test_dirname.o", ROOT / "third_modules" / "libc-testsuite" / "dirname.c"),
            ("test_fnmatch.o", ROOT / "third_modules" / "libc-testsuite" / "fnmatch.c"),
        ]
        for stem, src in tests_sources:
            t = task_cc(stem, src, BUILD_DIR / stem, tools, MUSL_DEMO_CFLAGS)
            t.optional = True
            tasks.append(t)
        def link_musl_user(name, elf, objs):
            ld = shutil.which("ld") or "/usr/bin/ld"
            crt1 = MUSL_LIB / "crt1.o"
            crti = MUSL_LIB / "crti.o"
            crtn = MUSL_LIB / "crtn.o"
            cmd = [ld, "-nostdlib", "-static", "-pie", "--no-dynamic-linker",
                   "-z", "pack-relative-relocs",
                   "-T", str(ROOT / "linker" / "user.ld"), "-e", "_start",
                   str(crt1), str(crti), *map(str, objs), str(crtn),
                   "-L", str(MUSL_LIB), "--start-group", "-lc", "--end-group",
                   "-o", str(elf)]
            return Task(name=name, cmd=cmd, out=elf, optional=True,
                        group="musl", description=f"link {elf.name} (native musl)",
                        cwd=BUILD_DIR)
        musl_demo_elf = link_musl_user(
            "musl_demo.elf", BUILD_DIR / "musl_demo.elf",
            [BUILD_DIR / "musl_demo.o"])
        tasks.append(musl_demo_elf)
        musl_abi_test_c = task_cc("musl_abi_test.o",
                                  APPS_DIR / "musl_abi_test.c",
                                  BUILD_DIR / "musl_abi_test.o", tools,
                                  MUSL_DEMO_CFLAGS)
        musl_abi_test_c.optional = True
        tasks.append(musl_abi_test_c)
        musl_abi_test_elf = link_musl_user(
            "musl_abi_test.elf", BUILD_DIR / "musl_abi_test.elf",
            [BUILD_DIR / "musl_abi_test.o"])
        tasks.append(musl_abi_test_elf)
        libc_tests_elf = link_musl_user(
            "libc_testsuite.elf", BUILD_DIR / "libc_testsuite.elf",
            [BUILD_DIR / "libc_tests_main.o",
             BUILD_DIR / "test_string.o", BUILD_DIR / "test_qsort.o",
             BUILD_DIR / "test_strtol.o", BUILD_DIR / "test_strtod.o",
             BUILD_DIR / "test_basename.o", BUILD_DIR / "test_dirname.o",
             BUILD_DIR / "test_fnmatch.o"])
        tasks.append(libc_tests_elf)
    return BuildPlan(tasks=tasks, user_elves=user_elves,
                     musl_enabled=plan_musl_enabled)
@dataclass
class BuildPlan:
    tasks: List[Task]
    user_elves: List[Task]
    musl_enabled: bool = False
    def all(self) -> List[Task]:
        return list(self.tasks) + list(self.user_elves)
def execute_plan(plan: BuildPlan, tools: Tools, console: Console,
                 jobs: int = 1) -> BuildStats:
    stats = BuildStats()
    overall_t0 = time.perf_counter()
    def run_task(task: Task) -> None:
        t0 = time.perf_counter()
        if task.out and task.out.exists():
            out_mtime = task.out.stat().st_mtime
            stale = False
            for dep_path in task.dep_paths():
                if dep_path.exists() and dep_path.stat().st_mtime > out_mtime:
                    stale = True
                    break
            if not stale:
                stats.cache_hit += 1
                stats.timings[task.name] = (time.perf_counter() - t0, "cached")
                return
        try:
            res = run(task.cmd, cwd=task.cwd, env=task.env)
        except FileNotFoundError as exc:
            if task.optional:
                console.warn(f"{task.name} 跳过: {exc}")
                return
            stats.failed += 1
            console.fail(f"{task.name}: {exc}")
            raise
        if not res.ok:
            if task.optional:
                console.warn(f"{task.name} 跳过 (exit {res.returncode}, "
                             f"原生 musl 不可用不影响其它构建)")
                return
            stats.failed += 1
            console.fail(f"{task.name} (exit {res.returncode})")
            for line in (res.stderr or res.stdout).splitlines()[-30:]:
                console.writeln(f"          {line}")
            raise SystemExit(res.returncode)
        stats.compiled += 1
        stats.timings[task.name] = (time.perf_counter() - t0, "built")
    def fmt_dur(seconds: float) -> str:
        if seconds < 0.05:
            return "(<0.1s)"
        if seconds < 10:
            return f"({seconds:.2f}s)"
        return f"({seconds/60:.1f}min)"
    def c_dim(s: str) -> str:
        return f"{console._c(Ansi.DIM)}{console._c(Ansi.GRAY)}{s}{console._c(Ansi.RESET)}"
    total_steps = 11
    s = 1
    console.step_header(s, total_steps, "Assembling boot sectors")
    for t in plan.tasks[:3]:
        run_task(t)
        console.ok(f"{t.description}  {c_dim(fmt_dur(stats.timings[t.name][0]))}")
    s += 1
    console.step_header(s, total_steps, "Compiling kernel assembly")
    asm_kern = [t for t in plan.tasks if t.group == "asm" and t.name in
                ("func.o", "io.o", "stub.o", "entry.o", "switch.o", "idle.o",
                 "ap_trampoline.bin")]
    for t in asm_kern:
        run_task(t)
        console.ok(f"{t.description}  {c_dim(fmt_dur(stats.timings[t.name][0]))}")
    for t in (t for t in plan.tasks if t.name == "ap_tramp.o"):
        run_task(t)
    s += 1
    console.step_header(s, total_steps, "Compiling kernel C objects")
    for t in (t for t in plan.tasks if t.group == "musl-headers"):
        run_task(t)
        console.ok(f"{t.description}  {c_dim(fmt_dur(stats.timings[t.name][0]))}")
    cc_kern = [t for t in plan.tasks if t.group == "cc" and t.name.endswith(".o")
               and not t.name.startswith("up_") and not t.name.startswith("lc_")
               and not t.name.startswith("musl_") and not t.name.startswith("test_")
               and not t.name.startswith("libc_tests_main")]
    with console.progress(len(cc_kern), "kernel C", Ansi.BR_CYN) as update:
        for i, t in enumerate(cc_kern, 1):
            run_task(t)
            update(i, t.description)
    s += 1
    console.step_header(s, total_steps, "Compiling user & lib objects")
    user_lib = [t for t in plan.tasks if t.group == "cc" and t.name.startswith("up_")]
    prog_obj = [t for t in plan.tasks if t.group == "cc" and t.name.startswith("up_")
                and t.name not in ("up_start.o",)]
    up_start = [t for t in plan.tasks if t.name == "up_start.o"]
    with console.progress(len(user_lib) + len(prog_obj) + len(up_start),
                          "user C", Ansi.BR_YEL) as update:
        i = 0
        for grp in (up_start, user_lib, prog_obj):
            for t in grp:
                i += 1
                run_task(t)
                update(i, t.description)
    s += 1
    console.step_header(s, total_steps, "Building lc_demo")
    lc_tasks = [t for t in plan.tasks if t.group in ("asm","cc","link","objcopy") and "lc_" in t.name]
    with console.progress(len(lc_tasks), "lc_demo", Ansi.BR_MAG) as update:
        for i, t in enumerate(lc_tasks, 1):
            run_task(t)
            update(i, t.description)
    s += 1

    console.step_header(s, total_steps, "Building native musl (configure+make)")
    if not plan.musl_enabled:
        console.info("原生 musl 编译已跳过 (未开 --with-musl-lib 或缺少 sh/make/cc)")
    else:
        musl_lib_tasks = [t for t in plan.tasks if t.group == "musl-lib"]
        with console.progress(len(musl_lib_tasks), "musl lib", Ansi.BR_BLU) as update:
            for i, t in enumerate(musl_lib_tasks, 1):
                run_task(t)
                update(i, t.description)
    s += 1

    console.step_header(s, total_steps, "Building musl demos & libc testsuite")
    musl_lib_ok = (MUSL_PREFIX / "lib" / "libc.a").exists()
    if not plan.musl_enabled:
        console.info("musl demo/testsuite 跳过 (原生 musl 未启用)")
    elif not musl_lib_ok:
        console.info("musl 库未生成 (configure/make 失败或被跳过), "
                     "demo/testsuite 跳过 — 不影响内核/用户程序")
    else:
        musl_tasks = [t for t in plan.tasks if t.group in ("cc", "link", "musl") and
                      ("musl_" in t.name or "test_" in t.name or
                       "libc_tests" in t.name)]
        with console.progress(len(musl_tasks), "musl", Ansi.BR_BLU) as update:
            for i, t in enumerate(musl_tasks, 1):
                run_task(t)
                update(i, t.description)
    s += 1

    console.step_header(s, total_steps, "Building toybox")
    toybox_tasks = [t for t in plan.tasks if t.group == "toybox"]
    with console.progress(len(toybox_tasks), "toybox", Ansi.BR_BLU) as update:
        for i, t in enumerate(toybox_tasks, 1):
            run_task(t)
            update(i, t.description)
    s += 1
    console.step_header(s, total_steps, "Generating font subset")
    font_py = [t for t in plan.tasks if t.group == "python" and "font" in t.name]
    with console.progress(len(font_py), "font", Ansi.BR_MAG) as update:
        i = 0
        for t in font_py:
            i += 1
            run_task(t)
            update(i, t.description)
    s += 1
    console.step_header(s, total_steps, "Linking user ELFs (remaining)")
    user_elf_tasks = list(plan.user_elves)
    with console.progress(len(user_elf_tasks), "user ELFs", Ansi.BR_GRN) as update:
        for i, t in enumerate(user_elf_tasks, 1):
            run_task(t)
            update(i, t.description)
    s += 1
    console.step_header(s, total_steps, "Linking kernel image")
    kernel_link = [t for t in plan.tasks
                   if t.group in ("link", "objcopy") and "kernel" in t.name]
    with console.progress(len(kernel_link), "kernel link", Ansi.BR_CYN) as update:
        for i, t in enumerate(kernel_link, 1):
            run_task(t)
            update(i, t.description)
    s += 1
    console.step_header(s, total_steps, "Packing floppy image")
    floppy_task = [t for t in plan.tasks if t.name == "floppy.img"][0]
    t0 = time.perf_counter()
    res = run(floppy_task.cmd)
    if not res.ok:
        console.fail(f"mkfloppy.py failed (exit {res.returncode})")
        for line in (res.stderr or res.stdout).splitlines()[-20:]:
            console.writeln(f"          {line}")
        raise SystemExit(res.returncode)
    dur = time.perf_counter() - t0
    console.ok(f"build/floppy.img  {c_dim(fmt_dur(dur))}")
    s += 1
    stats.timings["__total__"] = (time.perf_counter() - overall_t0, "overall")
    return stats
def show_failure_hint(console: Console, missing: List[str]) -> None:
    console.writeln()
    console.fail("Missing required toolchain components:")
    for m in missing:
        console.writeln(f"          • {m}")
    console.writeln()
    console.info("Install one of the supported toolchains:")
    console.writeln("          • Linux   : apt install nasm zig         OR  apt install nasm lld gcc")
    console.writeln("          • macOS   : brew install nasm zig")
    console.writeln("          • Windows : install zig, nasm, llvm (winget/choco/scoop)")
    console.writeln()
    console.info("Cross-compiler priority: zig cc > x86_64-elf-gcc > gcc > cc")
    console.writeln()
def do_clean(console: Console) -> None:
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
        console.ok(f"removed {BUILD_DIR}")
    else:
        console.info(f"{BUILD_DIR} already absent")
def do_run(console: Console, stats: BuildStats,
           smp: int, gdb: bool, no_net: bool, boot_floppy: bool,
           kvm: bool) -> None:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        console.warn("qemu-system-x86_64 not found on PATH; build is up-to-date.")
        return

    if kvm and not os.path.exists("/dev/kvm"):
        console.warn("未找到 /dev/kvm, KVM 不可用 (需 Linux/WSL2 且开启嵌套虚拟化); "
                     "回退 TCG 软件模拟")
        kvm = False
    if kvm:
        console.info("KVM 加速已启用 (-enable-kvm -cpu host), 可在 ring0 测 MWAIT")
        cmd = [qemu, "-enable-kvm", "-cpu", "host", "-m", "1G",
               "-smp", str(max(1, smp))]
    else:
        cmd = [qemu, "-accel", "tcg,tb-size=256", "-m", "1G",
               "-smp", str(max(1, smp))]
    if boot_floppy:
        cmd += ["-fda", str(BUILD_DIR / "floppy.img")]
    else:
        hd_img = BUILD_DIR / "test_hd.img"
        mkdisk = SCRIPTS / "make_ext2.py"
        if mkdisk.exists():
            console.info("generating test_hd.img via make_ext2.py")
            run([sys.executable, str(mkdisk), str(BUILD_DIR), str(hd_img)])
        cmd += ["-hda", str(hd_img)]
    cmd += ["-debugcon", "stdio"]
    if not no_net:
        cmd += ["-netdev", "user,id=net0,hostfwd=tcp::8765-:8765",
                "-device", "e1000,netdev=net0"]
    if gdb:
        cmd += ["-s", "-S"]
    console.writeln()
    console.writeln(f"  {console._c(Ansi.BR_GRN)}▶ launching qemu..."
                    f"{'（SMP' if smp > 1 else ''}"
                    f"{' · KVM' if kvm else ''}"
                    f"{' · GDB 等待' if gdb else ''}"
                    f"{' · floppy 引导' if boot_floppy else ''}"
                    f"{console._c(Ansi.RESET)}")
    console.writeln()
    subprocess.run(cmd)
def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="build.py",
        description="Cross-platform CoreZ OS build system",
    )
    parser.add_argument("target", nargs="?", default="floppy",
                        choices=["all", "floppy", "run", "clean"],
                        help="build target (default: floppy)")
    parser.add_argument("--no-color", action="store_true",
                        help="disable ANSI colour output")
    parser.add_argument("--jobs", "-j", type=int, default=1,
                        help="parallel compile jobs (default: 1)")
    parser.add_argument("--version", action="version", version="corez-build 1.0")
    parser.add_argument("--sm", type=int, default=1, metavar="N",
                        help="SMP CPU 数(qemu -smp N, 多核启动验证; 默认 1)")
    parser.add_argument("--gdb", action="store_true",
                        help="QEMU GDB stub(-s -S, 等待 gdb 连接 kernel.elf)")
    parser.add_argument("--no-net", action="store_true",
                        help="不挂虚拟网卡/后端(默认挂 e1000 + user 后端)")
    parser.add_argument("--boot-floppy", action="store_true",
                        help="以 floppy.img 作为引导软盘(-fda); 默认用 test_hd.img(-hda)")
    parser.add_argument("--kvm", action="store_true",
                        help="启用 KVM 硬件加速(-enable-kvm -cpu host), 用于测试 "
                             "需在 ring0 执行的 MWAIT 等真实 CPU 特性(需 Linux/WSL2)")
    parser.add_argument("--with-musl-lib", action="store_true",
                        help="编完整原生 musl libc.a 并链接 musl_demo.elf/libc_testsuite.elf "
                             "(默认关闭; 日常构建只兼容 musl 头子集, 不 make 完整 libc)")
    args = parser.parse_args(argv)
    _enable_vt_on_windows()
    console = Console(use_color=color_enabled(args.no_color))
    console.banner("v1.0  ·  Windows / Linux / macOS")
    if args.target == "clean":
        do_clean(console)
        return 0
    try:
        tools = detect_tools()
    except FileNotFoundError as exc:
        show_failure_hint(console, [str(exc)])
        return 2
    console.info(f"cc      = {tools.cc}  {'(zig)' if tools.is_zig else ''}")
    console.info(f"ld      = {tools.ld}")
    console.info(f"nasm    = {tools.nasm}")
    console.info(f"objcopy = {tools.objcopy}")
    console.writeln()
    plan = make_plan(tools, with_musl_lib=args.with_musl_lib)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    try:
        stats = execute_plan(plan, tools, console, jobs=args.jobs)
    except SystemExit as exc:
        return int(exc.code) if exc.code is not None else 1
    floppy = BUILD_DIR / "floppy.img"
    kernel = BUILD_DIR / "kernel.bin"
    elf    = BUILD_DIR / "kernel.elf"
    def human_size(p: Path) -> str:
        try:
            n = p.stat().st_size
        except FileNotFoundError:
            return "—"
        for unit in ("B", "KB", "MB", "GB"):
            if n < 1024 or unit == "GB":
                return f"{n:.1f} {unit}" if unit != "B" else f"{n} {unit}"
            n /= 1024
        return f"{n:.1f} GB"
    total_dur = stats.timings.get("__total__", (0.0, ""))[0]
    def fmt_dur(seconds: float) -> str:
        if seconds < 0.05:
            return "(<0.1s)"
        if seconds < 10:
            return f"({seconds:.2f}s)"
        return f"({seconds/60:.1f}min)"
    console.summary([
        ("artefacts",   f"floppy.img · kernel.bin · kernel.elf", Ansi.BR_WHT),
        ("floppy size", human_size(floppy),                       Ansi.BR_CYN),
        ("kernel size", human_size(kernel),                       Ansi.BR_CYN),
        ("kernel.elf",  human_size(elf),                          Ansi.BR_CYN),
        ("compiled",    str(stats.compiled),                      Ansi.BR_GRN),
        ("cached",      str(stats.cache_hit),                     Ansi.BR_YEL),
        ("failed",      str(stats.failed),                        Ansi.BR_RED if stats.failed else Ansi.GRAY),
        ("total",       fmt_dur(total_dur),                       Ansi.BR_WHT),
    ])
    console.writeln(f"  {console._c(Ansi.GREEN)}{console._c(Ansi.BOLD)}"
                    f"✔  build complete{console._c(Ansi.RESET)}")
    console.writeln()
    if args.target == "run":
        do_run(console, stats, 4, args.gdb, args.no_net, args.boot_floppy, args.kvm)
    return 0
if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
