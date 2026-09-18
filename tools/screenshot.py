#!/usr/bin/env python3
"""Grab what is on the panel's glass and save it as a PNG.

    tools/screenshot.py --host 192.168.1.50 --token $TOK out.png

GET /screenshot returns the live framebuffer as a 16-bit BMP; this only
fetches it and re-encodes, so `curl` and any image viewer work just as well.
Needs Pillow for the PNG step.
"""
import argparse
import io
import sys
import urllib.request

from PIL import Image


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True, help="the panel's address")
    ap.add_argument("--token", required=True, help="X-Auth token shown under the gear button")
    ap.add_argument("out", help="output file; the extension picks the format")
    a = ap.parse_args()

    req = urllib.request.Request(f"http://{a.host}/screenshot", headers={"X-Auth": a.token})
    with urllib.request.urlopen(req, timeout=15) as r:
        body = r.read()

    img = Image.open(io.BytesIO(body)).convert("RGB")
    img.save(a.out)
    print(f"{a.out}: {img.width}x{img.height}")


if __name__ == "__main__":
    sys.exit(main())
