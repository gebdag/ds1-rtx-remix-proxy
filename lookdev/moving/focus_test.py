"""Switches the lookdev host window in and out of focus, the way alt-tab does. Never touches the game.
    focus_test.py <mode: activate|minimise> <seconds between> <count>"""
import ctypes
import sys
import time
from ctypes import wintypes as W

u = ctypes.WinDLL("user32", use_last_error=True)
mode, gap, count = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
PROC = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)


def host():
    found = []

    def cb(h, _):
        n = ctypes.create_unicode_buffer(200)
        u.GetWindowTextW(h, n, 200)
        if n.value.startswith("DS1 lookdev - Remix") and u.IsWindowVisible(h):
            found.append(h)
        return True
    u.EnumWindows(PROC(cb), 0)
    return found[0] if found else None


def front(h):
    # Alt press lets a background process take the foreground; it goes to nobody but the input queue of the foreground window.
    u.keybd_event(0x87, 0, 0, 0)
    u.keybd_event(0x87, 0, 2, 0)
    return u.SetForegroundWindow(h)


shell = u.GetShellWindow()
for i in range(count):
    h = host()
    if not h:
        print("host window gone at", i, time.strftime("%H:%M:%S"), flush=True)
        sys.exit(1)
    if mode == "minimise":
        u.ShowWindow(h, 6)
        time.sleep(gap)
        u.ShowWindow(h, 9)
        front(h)
    else:
        u.SetForegroundWindow(shell)
        time.sleep(gap)
        front(h)
    print(i, time.strftime("%H:%M:%S"), flush=True)
    time.sleep(gap)
print("survived", count, flush=True)
