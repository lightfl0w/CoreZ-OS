import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
IMG = BUILD / "test_hd.img"
ESP = BUILD / "esp.img"
VARS = BUILD / "OVMF_VARS.fd"
LOG = Path("/tmp/win32_smoke.log")

MARKS = [
    "win_main: argc=",         
    "sum: 20 + 26",            
    "WIN_GUI_LAUNCH_OK",     
]
TIMEOUT = 300

OVMF_CODE_NAMES = ("OVMF_CODE.4m.fd", "OVMF_CODE.fd")
OVMF_VARS_NAMES = ("OVMF_VARS.4m.fd", "OVMF_VARS.fd")
OVMF_DIRS = ("/usr/share/edk2-ovmf/x64", "/usr/share/OVMF",
             "/usr/share/edk2/x64", "/usr/share/ovmf/x64")

AUTOEXEC = (b"/share/win_main.exe hello world\n"
            b"/bin/busybox echo WIN_MAIN_RAN\n"
            b"/share/win_gui.exe\n"
            b"/bin/busybox echo WIN_GUI_LAUNCH_OK\n")


def find_ovmf():
    for d in OVMF_DIRS:
        base = Path(d)
        for cn in OVMF_CODE_NAMES:
            for vn in OVMF_VARS_NAMES:
                code, vars_src = base / cn, base / vn
                if code.exists() and vars_src.exists():
                    return code, vars_src
    return None


def qemu_cmd(smp, uefi):
    if uefi:
        ovmf = find_ovmf()
        if ovmf is None:
            print("WIN32 SMOKE FAIL: OVMF 固件未找到")
            return None
        if not ESP.exists():
            print(f"WIN32 SMOKE FAIL: {ESP} 不存在")
            return None
        code, vars_src = ovmf
        shutil.copyfile(vars_src, VARS)
        return ["qemu-system-x86_64", "-machine", "pc",
                "-accel", "tcg,tb-size=256", "-m", "1G", "-smp", smp,
                "-vga", "std",
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,unit=1,file={VARS}",
                "-hda", str(ESP), "-hdb", str(IMG),
                "-serial", "file:/tmp/win32_smoke_ser.log",
                "-debugcon", f"file:{LOG}", "-display", "none", "-no-reboot"]
    return ["qemu-system-x86_64", "-accel", "tcg,tb-size=256", "-m", "1G",
            "-smp", smp, "-hda", str(IMG), "-debugcon", f"file:{LOG}",
            "-display", "none", "-no-reboot"]


def main():
    smp = "1"
    if "--smp" in sys.argv:
        smp = sys.argv[sys.argv.index("--smp") + 1]
    uefi = "--uefi" in sys.argv

    for exe in ("win_main.exe", "win_gui.exe"):
        if not (BUILD / exe).exists():
            print(f"WIN32 SMOKE FAIL: build/{exe} 不存在 "
                  f"(需要 x86_64-w64-mingw32-gcc)")
            return 1

    ae = Path("/tmp/win32_autoexec")
    ae.write_bytes(AUTOEXEC)
    subprocess.run(
        ["python3", "scripts/make_ext4.py", "build", "build/test_hd.img",
         "--smoke", "--autoexec", str(ae)], check=True, cwd=ROOT)

    cmd = qemu_cmd(smp, uefi)
    if cmd is None:
        return 1
    LOG.unlink(missing_ok=True)
    proc = subprocess.Popen(cmd, cwd=ROOT,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + TIMEOUT
        text = ""
        while time.time() < deadline:
            if LOG.exists():
                text = LOG.read_text(errors="replace")
                if all(m in text for m in MARKS):
                    print("WIN32 SMOKE PASS")
                    return 0

            if proc.poll() is not None:
                print(f"WIN32 SMOKE FAIL: qemu exited rc={proc.returncode}")
                return 1
            time.sleep(1)
        missing = [m for m in MARKS if m not in text]
        print(f"WIN32 SMOKE FAIL: timeout, missing {missing} in {TIMEOUT}s")
        return 1
    finally:
        proc.kill()
        proc.wait()


if __name__ == "__main__":
    sys.exit(main())
