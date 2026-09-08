import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMG = ROOT / "build" / "test_hd.img"
LOG = Path("/tmp/nit_smoke.log")
MARKS = ["[abi] ALL PASS", "child: fork returned", "dev_demo: PASS",
         "TOYBOX_ECHO_OK", "uid=0(root) gid=0(root)", "1 root root",
         "uid=1000(user) gid=1000(user)", "Uid:\t0 0 0"]
TIMEOUT = 300


def main():
    subprocess.run(
        ["python3", "scripts/make_ext2.py", "build", "build/test_hd.img",
         "--smoke"], check=True, cwd=ROOT)
    LOG.unlink(missing_ok=True)
    proc = subprocess.Popen(
        ["qemu-system-x86_64", "-accel", "tcg,tb-size=256", "-m", "1G",
         "-smp", "1", "-hda", str(IMG), "-debugcon", f"file:{LOG}",
         "-display", "none", "-no-reboot"], cwd=ROOT,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            if LOG.exists():
                text = LOG.read_text(errors="replace")
                if all(m in text for m in MARKS):
                    print("SMOKE PASS")
                    return 0
            if proc.poll() is not None:
                print(f"SMOKE FAIL: qemu exited rc={proc.returncode}")
                return 1
            time.sleep(1)
        print(f"SMOKE FAIL: timeout, missing {MARKS} in {TIMEOUT}s")
        return 1
    finally:
        proc.kill()
        proc.wait()


if __name__ == "__main__":
    sys.exit(main())
