"""
gen_atlas.py : generates horn_font_atlas.h, the text atlas used by the result banner.

WHAT IT DOES
  Renders the five banner labels (Very Early, Slight Early, Excellent, Slight Late,
  Very Late) with a bold font, converts them to a signed-distance-field (SDF) image, and
  writes the pixels plus layout info into a C header. The SDF lets the shader draw sharp,
  glowing text at any size from one tiny texture (about 40 KB).

WHEN TO RUN IT
  Only if you want to change the label text, the font, or the text sharpness.
  The generated horn_font_atlas.h is already included next to horn_meter.cpp.

USAGE
  pip install pillow numpy scipy
  python3 gen_atlas.py          # writes horn_font_atlas.h and atlas_preview.png

SETTINGS (top of the script)
  FONT    : path to a .ttf/.otf font. Poppins Bold (SIL OFL) is used by default.
  TEXTS   : the labels. The ORDER must match the HornResult enum in horn_meter.cpp:
            0 too early, 1 slightly early, 2 excellent, 3 slightly late, 4 very late.
  SIZE    : font size in atlas pixels. Bigger = sharper text, larger header.
  SPREAD  : SDF range in pixels. It limits how far the glow can reach.
  ROUND   : how much the sharp glyph corners are softened.
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import distance_transform_edt as edt

FONT = "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"
SS = 4              # supersample
SIZE = 26           # atlas font size in px
SPREAD = 8          # SDF spread in atlas px
PAD = SPREAD + 1
ROUND = 1.1         # corner rounding radius, atlas px
# index = HornResult value
TEXTS = ["Very Early", "Slight Early", "Excellent", "Slight Late", "Very Late"]

font = ImageFont.truetype(FONT, SIZE * SS)
rows = []
for t in TEXTS:
    l, tp, r, b = font.getbbox(t)
    rows.append((t, l, tp, r, b))

cell_w = max(int(np.ceil((r - l) / SS)) for _, l, tp, r, b in rows) + 2 * PAD
row_h  = [int(np.ceil((b - tp) / SS)) + 2 * PAD for _, l, tp, r, b in rows]
W = cell_w
H = sum(row_h)

atlas = np.zeros((H, W), np.uint8)
meta = []
y0 = 0
for (t, l, tp, r, b), rh in zip(rows, row_h):
    img = Image.new("L", (W * SS, rh * SS), 0)
    d = ImageDraw.Draw(img)
    d.text((PAD * SS - l, PAD * SS - tp), t, font=font, fill=255)
    m = np.array(img) > 127

    # round convex corners: erode by R then dilate by R (morphological opening via EDT)
    R = ROUND * SS
    eroded = edt(m) > R
    m = edt(~eroded) <= R

    din = edt(m)            # distance to outside, for inside pixels
    dout = edt(~m)          # distance to inside, for outside pixels
    sd = (din - dout) / SS  # positive inside, atlas px
    sd = sd.reshape(rh, SS, W, SS).mean(axis=(1, 3))
    enc = np.clip(128 + sd / SPREAD * 127, 0, 255).astype(np.uint8)
    atlas[y0:y0 + rh, :] = enc

    tw = (r - l) / SS
    th = (b - tp) / SS
    meta.append((y0, rh, tw, th))
    y0 += rh

with open("horn_font_atlas.h", "w") as f:
    f.write("// horn_font_atlas.h : GENERATED FILE (see tools/gen_atlas.py). Do not edit by hand.\n")
    f.write("//\n// Signed-distance-field text atlas for the result banner:\n")
    f.write("//   Very Early, Slight Early, Excellent, Slight Late, Very Late  (index = HornResult)\n")
    f.write("// Font: Poppins Bold (SIL Open Font License), corners softened.\n")
    f.write("// Encoding: 8-bit, 128 = glyph edge, 255 = %d px inside, 0 = %d px outside.\n#pragma once\n\n" % (SPREAD, SPREAD))
    f.write("#define HORN_ATLAS_W %d\n#define HORN_ATLAS_H %d\n#define HORN_ATLAS_SPREAD %d.0f\n\n" % (W, H, SPREAD))
    f.write("// per text: top pixel row, row height, text width and height in atlas px (without padding)\n")
    f.write("struct HornTextRect { int y; int h; float tw; float th; };\n")
    f.write("static const HornTextRect kHornText[5] = {\n")
    for (y, rh, tw, th), t in zip(meta, TEXTS):
        f.write("    { %d, %d, %.2ff, %.2ff },   // %s\n" % (y, rh, tw, th, t))
    f.write("};\n#define HORN_ATLAS_PAD %d\n\n" % PAD)
    f.write("static const unsigned char kHornAtlas[HORN_ATLAS_W * HORN_ATLAS_H] = {\n")
    flat = atlas.flatten()
    for i in range(0, len(flat), 32):
        f.write("    " + ",".join(str(int(v)) for v in flat[i:i + 32]) + ",\n")
    f.write("};\n")

Image.fromarray(atlas).save("atlas_preview.png")
print("atlas", W, "x", H, "bytes", W * H)
for m_, t in zip(meta, TEXTS): print(t, m_)
