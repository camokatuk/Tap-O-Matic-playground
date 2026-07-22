#!/usr/bin/env python3
"""
Render the module panel SVG to the PNG the emulator uses as its background.

Why a raster: browsers substitute fonts on SVG <text>, mangling the panel
labels. We rasterize once, up front, so the browser just shows a picture.

Pipeline (all in-memory, source SVG untouched):
  1. Remap the panel's font-family names to a form fontconfig/pango understands
     (the SVG asks for "Futura Condensed Extra" etc., which fontconfig can't
     resolve and silently falls back to a wide font, overlapping the labels;
     "Futura + font-stretch:condensed + weight" selects the real condensed face).
  2. rsvg-convert the SVG onto a solid background (default black), at exact size
     — no letterboxing, no clipping.
  3. Autocrop the uniform background border so 0%/100% == panel edges in the CSS.

Edit the panel SVG, then just re-run:  python3 emulator/render_panel.py

Requirements: rsvg-convert (`brew install librsvg`), python3, Pillow (PIL).
The fonts referenced by the SVG must exist on the system (macOS ships Futura).
"""
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageChops

# ---- Config (tweak freely) -------------------------------------------------
REPO         = Path(__file__).resolve().parent.parent
SRC_SVG      = REPO / "panel" / "Fox-Tail.svg"
DST_PNG      = REPO / "emulator" / "web" / "Fox-Tail.png"
BG_COLOR     = "#000000"     # background behind the panel (rsvg color + trim)
RENDER_WIDTH = 2000          # output width in px before cropping (higher = sharper)

# SVG font-family -> a spec pango can resolve. Add entries if the panel art
# starts using other fonts. Right side is spliced into the CSS `font` shorthand.
FONT_REMAP = {
    "font-family:'Futura Condensed Extra'":
        "font-family:'Futura';font-stretch:condensed;font-weight:800",
    "font-family:Futura-CondensedMedium":
        "font-family:'Futura';font-stretch:condensed;font-weight:500",
}
# ---------------------------------------------------------------------------


def hex_to_rgb(h: str):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def trim(im: Image.Image, color, tol: int = 8) -> Image.Image:
    """Crop away a border that is within `tol` of `color` (per channel)."""
    diff = ImageChops.difference(im, Image.new("RGB", im.size, color)).convert("L")
    bbox = diff.point(lambda p: 255 if p > tol else 0).getbbox()
    return im.crop(bbox) if bbox else im


def main():
    svg = SRC_SVG.read_text()
    for old, new in FONT_REMAP.items():
        svg = svg.replace(old, new)

    with tempfile.TemporaryDirectory() as td:
        tmp_svg = Path(td) / "panel.svg"
        tmp_svg.write_text(svg)
        out_png = Path(td) / "panel.png"
        subprocess.run(
            ["rsvg-convert", "-b", BG_COLOR, "--width", str(RENDER_WIDTH),
             str(tmp_svg), "-o", str(out_png)],
            check=True,
        )
        im = trim(Image.open(out_png).convert("RGB"), hex_to_rgb(BG_COLOR))

    DST_PNG.parent.mkdir(parents=True, exist_ok=True)
    im.save(DST_PNG)
    w, h = im.size
    print(f"wrote {DST_PNG.relative_to(REPO)}  ({w}x{h}, aspect {w/h:.4f})")
    print(f"  -> CSS:  aspect-ratio: {w} / {h};")


if __name__ == "__main__":
    main()
