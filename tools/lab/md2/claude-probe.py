#!/usr/bin/env python3
"""One PrintWindow capture + color classification for a game process.

Usage: claude-probe.py <exe-full-path> [out-png]
Prints one JSON line: {"kind": ..., "black": .., "warm": .., "cool": ..,
"title": ..., "saved": ...}. kind is "nowindow" when no visible window of
that exe exists yet. Capture/classify logic mirrors lab/scripts/sidecar.py.
No input is sent to the game; PrintWindow does not change focus. A minimized game
window is restored without activation before the capture.
"""

import ctypes
import ctypes.wintypes as wt
import json
import os
import sys

from PIL import Image

user32 = ctypes.WinDLL("user32")
gdi32 = ctypes.WinDLL("gdi32")
kernel32 = ctypes.WinDLL("kernel32")
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def window_title(hwnd):
    buffer = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buffer, 256)
    return buffer.value


def process_path(pid):
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        size = wt.DWORD(1024)
        buffer = ctypes.create_unicode_buffer(1024)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return buffer.value
        return None
    finally:
        kernel32.CloseHandle(handle)


def find_game(exe):
    found = []

    def callback(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        path = process_path(pid.value)
        if path and os.path.normcase(path) == os.path.normcase(exe):
            rect = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect))
            area = max(0, rect.right - rect.left) * max(0, rect.bottom - rect.top)
            found.append((area, hwnd))
        return True

    user32.EnumWindows(EnumWindowsProc(callback), 0)
    if not found:
        return None
    found.sort(reverse=True)
    return found[0][1]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


def capture(hwnd):
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    if width < 64 or height < 64:
        return None
    window_dc = user32.GetWindowDC(hwnd)
    memory_dc = gdi32.CreateCompatibleDC(window_dc)
    bitmap = gdi32.CreateCompatibleBitmap(window_dc, width, height)
    old = gdi32.SelectObject(memory_dc, bitmap)
    try:
        user32.PrintWindow(hwnd, memory_dc, 2)
        header = BITMAPINFOHEADER()
        header.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        header.biWidth = width
        header.biHeight = -height
        header.biPlanes = 1
        header.biBitCount = 32
        pixels = ctypes.create_string_buffer(width * height * 4)
        gdi32.GetDIBits(memory_dc, bitmap, 0, height, pixels, ctypes.byref(header), 0)
        return Image.frombuffer("RGBA", (width, height), pixels, "raw", "BGRA", 0, 1).convert("RGB")
    finally:
        gdi32.SelectObject(memory_dc, old)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(memory_dc)
        user32.ReleaseDC(hwnd, window_dc)


def classify(image):
    w, h = image.size
    small = image.crop((8, 34, w - 8, h - 8)).resize((240, 135))
    get = getattr(small, "get_flattened_data", None) or small.getdata
    px = list(get())
    n = len(px)
    black = sum(1 for r, g, b in px if max(r, g, b) < 24) / n
    warm = sum(1 for r, g, b in px if r > b + 30) / n
    cool = sum(1 for r, g, b in px if b > r + 30) / n
    if warm >= 0.5:
        kind = "gameplay" if black <= 0.10 and cool <= 0.03 else "dirty-gameplay"
    elif cool >= 0.5:
        kind = "blue"
    elif black >= 0.9:
        kind = "black"
    else:
        kind = "other"
    return kind, black, warm, cool


def main():
    exe = sys.argv[1]
    out_png = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None
    hwnd = find_game(exe)
    if hwnd is None:
        print(json.dumps({"kind": "nowindow", "black": 0, "warm": 0, "cool": 0, "title": "", "saved": ""}))
        return
    if user32.IsIconic(hwnd):
        # A minimized window cannot be captured, and the row's state machine would stall on the title: restore
        # it without activating it (SW_SHOWNOACTIVATE), so nothing takes the user's focus.
        user32.ShowWindow(hwnd, 4)
    image = capture(hwnd)
    if image is None:
        print(json.dumps({"kind": "unreadable", "black": 0, "warm": 0, "cool": 0, "title": window_title(hwnd), "saved": ""}))
        return
    kind, black, warm, cool = classify(image)
    saved = ""
    if out_png:
        image.save(out_png)
        saved = out_png
    print(json.dumps({"kind": kind, "black": round(black, 3), "warm": round(warm, 3),
                      "cool": round(cool, 3), "title": window_title(hwnd), "saved": saved}))


if __name__ == "__main__":
    main()
