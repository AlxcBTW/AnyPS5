#!/usr/bin/env python3
"""Screenshot + screen-state classifier for the Minecraft Dungeons II navigation rows (agent gp).

  md2-shot.py shot  <exe-full-path> <out-png|-> [--no-match]
      One PrintWindow capture of the game window (no input, no focus change), classified against the state
      templates in lab/nav/md2/states.json. Prints one JSON line:
      {"kind", "title", "fps", "frame", "state", "scores": {name: score}, "saved"}.
  md2-shot.py match <png>
      Classify a saved screenshot (for testing templates offline).
  md2-shot.py learn <png> <state> <x0> <y0> <x1> <y1> [mode] [thr] [min]   (a state named _x is an attribute: scored, never the state)
      Add one region to a state: the box is in pixels of the saved screenshot (as the Read tool shows it); the crop
      is stored under lab/nav/md2/<state>-<n>.png and the box as fractions of the game area, so it still matches
      when the window has another size. A state matches when every one of its regions matches, each at its own
      minimum score (default 0.6, 0.97 for "dark"; the optional last argument sets it).

A region compares a mask of the crop against the live screen (cv2.matchTemplate, normalised correlation, searched in
a window a few percent larger than the box). Modes: "white" = pixels whose weakest channel is >= thr (default 200: UI
text), "bright" = luma >= thr, "gray" = plain grayscale, "dark" = no template, the score is the share of the box's pixels
whose brightest channel is below thr (default 16; letterbox bars, black pages; min 0.97). Score 1.0 is identical, ~0 is
unrelated; a region passes at its own min score (default 0.6).
"""

import importlib.util
import json
import os
import re
import sys

import cv2
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
NAV_DIR = os.path.normpath(os.path.join(HERE, "..", "nav", "md2"))
STATES_JSON = os.path.join(NAV_DIR, "states.json")
SEARCH_PAD = 0.03


def load_probe():
    spec = importlib.util.spec_from_file_location("claude_probe", os.path.join(HERE, "claude-probe.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def game_area(image):
    """The rendered area: the full window without title bar and borders (same crop claude-probe uses)."""
    w, h = image.size
    return image.crop((8, 34, w - 8, h - 8))


def make_mask(rgb, mode, thr):
    arr = np.asarray(rgb.convert("RGB"), dtype=np.uint8)
    if mode == "white":
        return (arr.min(axis=2) >= thr).astype(np.float32)
    if mode == "bright":
        luma = 0.299 * arr[..., 0] + 0.587 * arr[..., 1] + 0.114 * arr[..., 2]
        return (luma >= thr).astype(np.float32)
    gray = cv2.cvtColor(arr, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255.0
    return gray


def load_states():
    if not os.path.exists(STATES_JSON):
        return {}
    with open(STATES_JSON, encoding="utf-8") as handle:
        return json.load(handle)


def region_score(area, region):
    w, h = area.size
    x0, y0, x1, y1 = region["box"]
    if region.get("mode") == "dark":
        # no template: the share of pixels of the box whose brightest channel is below thr (letterbox bars, black pages)
        crop = area.crop((int(x0 * w), int(y0 * h), max(int(x0 * w) + 1, int(x1 * w)), max(int(y0 * h) + 1, int(y1 * h))))
        return float((np.asarray(crop.convert("RGB"), dtype=np.uint8).max(axis=2) < region.get("thr", 16)).mean())
    tw, th = max(4, int(round((x1 - x0) * w))), max(4, int(round((y1 - y0) * h)))
    ref = Image.open(os.path.join(NAV_DIR, region["ref"])).convert("RGB").resize((tw, th), Image.BILINEAR)
    mode, thr = region.get("mode", "white"), region.get("thr", 200)
    tpl = make_mask(ref, mode, thr)
    if mode != "gray" and (tpl.std() < 1e-6):
        return 0.0
    sx0, sy0 = max(0, int((x0 - SEARCH_PAD) * w)), max(0, int((y0 - SEARCH_PAD) * h))
    sx1, sy1 = min(w, int((x1 + SEARCH_PAD) * w) + 1), min(h, int((y1 + SEARCH_PAD) * h) + 1)
    search = make_mask(area.crop((sx0, sy0, sx1, sy1)), mode, thr)
    if search.shape[0] < tpl.shape[0] or search.shape[1] < tpl.shape[1]:
        return 0.0
    if search.std() < 1e-6:
        return 0.0
    result = cv2.matchTemplate(search, tpl, cv2.TM_CCOEFF_NORMED)
    value = float(np.nanmax(result))
    return 0.0 if value != value else value


def classify_states(image):
    states = load_states()
    area = game_area(image)
    scores, best, best_score = {}, "", 0.0
    for name, spec in states.items():
        worst = 1.0
        ok = True
        for region in spec["regions"]:
            score = region_score(area, region)
            worst = min(worst, score)
            if score < region.get("min", 0.6):
                ok = False
        scores[name] = round(worst, 3)
        # Names starting with "_" are attributes of a screen (e.g. which card has the focus): scored, never the state.
        if ok and worst > best_score and not name.startswith("_"):
            best, best_score = name, worst
    return best, scores


def parse_title(title):
    match = re.search(r"FPS:\s*([0-9.]+)\s*\((\d+)\)", title or "")
    if match:
        return float(match.group(1)), int(match.group(2))
    return None, None


def picture_signature(image):
    """36 hex digits: the game area as 12x3 grey cells of 16 levels. Two probes of a still screen give the same string; the
    navigation uses it to tell a still screen no template knows from a moving one."""
    cells = np.asarray(game_area(image).convert("L").resize((12, 3), Image.BOX), dtype=np.uint8).flatten()
    return "".join(f"{int(v) >> 4:x}" for v in cells)


def cmd_shot(argv):
    exe, out = argv[0], (argv[1] if len(argv) > 1 and argv[1] != "-" else None)
    no_match = "--no-match" in argv
    probe = load_probe()
    hwnd = probe.find_game(exe)
    if hwnd is None:
        print(json.dumps({"kind": "nowindow", "title": "", "fps": None, "frame": None, "state": "", "scores": {}, "saved": ""}))
        return
    if probe.user32.IsIconic(hwnd):
        probe.user32.ShowWindow(hwnd, 4)
    image = probe.capture(hwnd)
    title = probe.window_title(hwnd)
    if image is None:
        print(json.dumps({"kind": "unreadable", "title": title, "fps": None, "frame": None, "state": "", "scores": {}, "saved": ""}))
        return
    kind, black, warm, cool = probe.classify(image)
    state, scores = ("", {}) if no_match else classify_states(image)
    foreground = probe.user32.GetForegroundWindow()
    # Only whether the game window has the foreground: other windows' titles are the user's business.
    fg = "GAME" if foreground == hwnd else ("other" if foreground else "none")
    saved = ""
    if out:
        image.save(out)
        saved = out
    fps, frame = parse_title(title)
    print(json.dumps({"kind": kind, "black": round(black, 3), "title": title, "fps": fps, "frame": frame,
                      "state": state, "scores": scores, "saved": saved, "fg": fg, "sig": picture_signature(image)}))


def cmd_match(argv):
    image = Image.open(argv[0]).convert("RGB")
    state, scores = classify_states(image)
    print(json.dumps({"state": state, "scores": scores}))


def cmd_learn(argv):
    png, name = argv[0], argv[1]
    x0, y0, x1, y1 = (int(v) for v in argv[2:6])
    mode = argv[6] if len(argv) > 6 else "white"
    thr = int(argv[7]) if len(argv) > 7 else 200
    minimum = float(argv[8]) if len(argv) > 8 else (0.97 if mode == "dark" else 0.6)
    image = Image.open(png).convert("RGB")
    w, h = image.size
    area_w, area_h = w - 16, h - 42
    crop = image.crop((x0, y0, x1, y1))
    os.makedirs(NAV_DIR, exist_ok=True)
    states = load_states()
    spec = states.setdefault(name, {"regions": []})
    index = len(spec["regions"]) + 1
    ref = f"{name}-{index}.png"
    crop.save(os.path.join(NAV_DIR, ref))
    box = [round((x0 - 8) / area_w, 4), round((y0 - 34) / area_h, 4), round((x1 - 8) / area_w, 4), round((y1 - 34) / area_h, 4)]
    spec["regions"].append({"box": box, "ref": ref, "mode": mode, "thr": thr, "min": minimum})
    with open(STATES_JSON, "w", encoding="utf-8") as handle:
        json.dump(states, handle, indent=2)
    print(json.dumps({"state": name, "region": index, "box": box, "self_score": round(region_score(game_area(image), spec["regions"][-1]), 3)}))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    command, rest = sys.argv[1], sys.argv[2:]
    {"shot": cmd_shot, "match": cmd_match, "learn": cmd_learn}[command](rest)


if __name__ == "__main__":
    main()
