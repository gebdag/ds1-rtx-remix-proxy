"""Runs a live host with Aftermath on, the warp simulator, and logs memory; keeps the logs when the host dies.
WARP_CDB=1 runs the host under cdb and logs who ends the process.

    warpwatch.py <tag> <seconds per capture> [warpsim args...]
"""
import os  # noqa: E402  (ds1paths: the local folders, see paths.example.ini)
import sys  # noqa: E402
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts")))
import ds1paths  # noqa: E402

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).parent
LOOKDEV = Path(__file__).resolve().parent.parent
DATA = Path(ds1paths.GAME + r"\DATA")
CDB = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"
tag = sys.argv[1]
OUT = HERE / f"warp_{tag}"
OUT.mkdir(exist_ok=True)
FWD = str(OUT).replace("\\", "/")
env = dict(os.environ, DXVK_ENABLE_AFTERMATH="1", DXVK_AFTERMATH_DUMP_PATH=FWD)
subprocess.run([sys.executable, "-c", "import lookdev; lookdev.stop_host()"], cwd=LOOKDEV)
if os.environ.get("WARP_CDB"):
    cmds = [
        'sxn -c2 ".echo SECOND_EH; kb 30; .dump /ma ' + FWD + '/host.dmp; q" eh',
        'sxn -c2 ".echo SECOND_AV; kb 30; .dump /ma ' + FWD + '/host.dmp; q" av',
        'bp ntdll!NtTerminateProcess ".echo TERMINATE; ~#; kb 40; gc"',
        'g',
    ]
    (OUT / "cdb_cmds.txt").write_text("\n".join(cmds) + "\n", encoding="ascii")
    subprocess.Popen([CDB, "-G", "-cf", str(OUT / "cdb_cmds.txt"), "-logo", str(OUT / "cdb.log"), str(DATA / "ds1_lookdev_host.exe"),
                      "--live", "--proxy", "d3d9_lookdev.dll", "-lookdev-host", "-nobeep"],
                     cwd=str(DATA), env=env, creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
else:
    subprocess.run([sys.executable, "lookdev.py", "host", "--live"], cwd=LOOKDEV, env=env, check=True)
sim = subprocess.Popen([sys.executable, str(HERE / "warpsim.py"), *sys.argv[2:]], stdout=open(OUT / "sim.log", "w", buffering=1), stderr=subprocess.STDOUT)
log = open(OUT / "mem.csv", "w", buffering=1)
log.write("t,vram_mb,host_mb,bridge_mb,host_ws,bridge_ws,title\n")
t0 = time.time()


def procs():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-Process ds1_lookdev_host,NvRemixBridge -ErrorAction SilentlyContinue | ForEach-Object { $_.ProcessName + ' ' + $_.PrivateMemorySize64 + ' ' + $_.WorkingSet64 + ' ' + $_.MainWindowTitle }"],
                       capture_output=True, text=True)
    out = {}
    for line in r.stdout.splitlines():
        p = line.split(" ", 3)
        if len(p) >= 3:
            out[p[0]] = (int(p[1]) >> 20, int(p[2]) >> 20, p[3] if len(p) > 3 else "")
    return out


def vram():
    r = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"], capture_output=True, text=True)
    return int(r.stdout.strip() or 0)


seen = False
while True:
    p = procs()
    h, b = p.get("ds1_lookdev_host"), p.get("NvRemixBridge")
    seen = seen or bool(h and b)
    log.write(f"{time.time() - t0:.0f},{vram()},{h[0] if h else 0},{b[0] if b else 0},{h[1] if h else 0},{b[1] if b else 0},{h[2][:90] if h else ''}\n")
    if seen and not (h and b):
        print("host or bridge gone at", round(time.time() - t0), flush=True)
        break
    if sim.poll() is not None:
        print("simulator ended", sim.returncode, flush=True)
        break
    time.sleep(5)
time.sleep(4)
for f in ("bridge32.log", "bridge64.log", "remix-dxvk.log", "crash-upload.log"):
    try:
        shutil.copy2(DATA / "rtx-remix" / "logs" / f, OUT / f)
    except OSError as e:
        print(e)
shutil.copy2(DATA / "rtx_comp" / "console_host.log", OUT / "console_host.log")
sim.kill()
print("logs kept in", OUT, flush=True)
