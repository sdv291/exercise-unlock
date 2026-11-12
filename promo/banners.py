#!/usr/bin/env python3
"""Render the Instagram banners (banners.html) to PNG.

    python banners.py                       # all -> out/banners/<concept>-<format>-<lang>.png
    python banners.py --only hero-feed-uk   # one of them

Concepts: hero (the PC unlocks with exercise), earn (minutes per rep),
hour (a movement break every hour), ai (the AI skeleton on the boy, real
MoveNet joints), laptop (squats instead of a password, the skeleton on a
laptop screen), phone (no webcam - use a phone). Formats: feed 1080x1350 (4:5),
story 1080x1920 (9:16). Languages: uk, en.

Needs only Google Chrome (headless screenshots); the page loads Nunito
from Google Fonts.
"""
import argparse
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "out" / "banners"
CONCEPTS = ("hero", "earn", "hour", "ai", "laptop", "phone")
FORMATS = {"feed": (1080, 1350), "story": (1080, 1920)}
LANGS = ("uk", "en")


def chrome():
    for base in (os.environ.get("PROGRAMFILES"), os.environ.get("PROGRAMFILES(X86)"), os.environ.get("LOCALAPPDATA")):
        if base:
            exe = pathlib.Path(base) / "Google" / "Chrome" / "Application" / "chrome.exe"
            if exe.exists():
                return str(exe)
    sys.exit("Google Chrome not found")


def render(names):
    OUT.mkdir(parents=True, exist_ok=True)
    exe = chrome()
    for name in names:
        concept, fmt, lang = name.split("-")
        w, h = FORMATS[fmt]
        png = OUT / f"{name}.png"
        url = (HERE / "banners.html").as_uri() + f"?c={concept}&l={lang}&f={fmt}"
        # The virtual time budget lets the web font and the pictures load
        # (and the subtitle settle under the headline) before the shot.
        subprocess.run([exe, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                        f"--window-size={w},{h}", "--virtual-time-budget=8000", "--allow-file-access-from-files",
                        f"--screenshot={png}", url],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="concept-format-lang, e.g. hero-feed-uk")
    args = ap.parse_args()
    render([args.only] if args.only else [f"{c}-{f}-{l}" for c in CONCEPTS for f in FORMATS for l in LANGS])


if __name__ == "__main__":
    main()
