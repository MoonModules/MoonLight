"""Convert one BDF font into a WLED bitmap font (.wbf) for Text.

The bitmap is the reason to convert: each pixel is one bit, so the file is a
small fraction of the BDF text. The measured sizes are in
docs/reference/examples/bdf-to-wbf.md.

The faces at https://github.com/mcer12/PixelFonts are published as BDF under bdf/.
This writes the same layout src/light/powerfunctions/wbf.h reads: a 12-byte header,
a width byte per glyph when the face is variable-width, then each glyph as a
row-major bit stream with the high bit on the left.

A glyph's device width is its advance. Its bitmap is placed on the baseline
from FONT_ASCENT and FONT_DESCENT, so a mark that sits above the line keeps its
place. Code points from the first to the last are all present. A missing one is
a blank of the font's default advance. A code point past ASCII is stored the
way WLED stores one: the header's unicode base maps it onto the table after 127.

Run: python3 docs/reference/examples/bdf_to_wbf.py face.bdf face.wbf
"""

import argparse
import sys


def parse_bdf(text):
    """Glyphs by encoding, plus the ascent and descent the properties declare."""
    ascent = descent = None
    glyphs = {}
    glyph = None
    in_bitmap = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("FONT_ASCENT "):
            ascent = int(line.split()[1])
        elif line.startswith("FONT_DESCENT "):
            descent = int(line.split()[1])
        elif line == "STARTCHAR":
            glyph = {"rows": []}
            in_bitmap = False
        elif line.startswith("STARTCHAR "):
            glyph = {"rows": []}
            in_bitmap = False
        elif glyph is None:
            continue
        elif line.startswith("ENCODING "):
            glyph["enc"] = int(line.split()[1])
        elif line.startswith("DWIDTH "):
            glyph["advance"] = int(line.split()[1])
        elif line.startswith("BBX "):
            w, h, x, y = (int(p) for p in line.split()[1:5])
            glyph["bbx"] = (w, h, x, y)
        elif line == "BITMAP":
            in_bitmap = True
        elif line == "ENDCHAR":
            enc = glyph.get("enc", -1)
            if enc >= 0 and "bbx" in glyph:
                glyphs[enc] = glyph
            glyph = None
            in_bitmap = False
        elif in_bitmap and line:
            glyph["rows"].append(int(line, 16))
    if ascent is None or descent is None:
        ascent, descent = box_from_glyphs(glyphs)
    return glyphs, ascent, descent


def box_from_glyphs(glyphs):
    """Ascent and descent taken from the bitmaps when the BDF omits both."""
    ascent = descent = 0
    for glyph in glyphs.values():
        _, h, _, y = glyph["bbx"]
        ascent = max(ascent, y + h)
        descent = max(descent, -y)
    return max(ascent, 1), descent


def cell(glyph, height, ascent):
    """Ink of one glyph on a cell of `height` rows, baseline at `ascent`."""
    bw, bh, xoff, yoff = glyph["bbx"]
    advance = glyph.get("advance", bw)
    width = max(advance, xoff + bw, 1)
    if xoff < 0:
        width -= xoff
    bits = [[0] * width for _ in range(height)]
    # BDF pads each row to a whole byte. The high bit is the leftmost pixel.
    shift = ((bw + 7) // 8) * 8 - 1
    for row, word in enumerate(glyph["rows"]):
        y = yoff + bh - 1 - row
        cell_row = (ascent - 1) - y
        if cell_row < 0 or cell_row >= height:
            continue
        for col in range(bw):
            if word & (1 << (shift - col)):
                x = xoff + col
                if xoff < 0:
                    x -= xoff
                if 0 <= x < width:
                    bits[cell_row][x] = 1
    return width, bits


def pack(bits, width):
    """One glyph as a continuous bit stream, padded to a whole byte."""
    stream = []
    acc = n = 0
    for row in bits:
        for col in range(width):
            acc = (acc << 1) | row[col]
            n += 1
            if n == 8:
                stream.append(acc)
                acc = n = 0
    if n:
        stream.append(acc << (8 - n))
    return bytes(stream)


def build(glyphs, ascent, descent):
    """The .wbf bytes, or an error string."""
    height = ascent + descent
    if not glyphs:
        return None, "the BDF has no encoded glyphs"
    if height < 1 or height > 64:
        return None, f"the face is {height} px tall, and a .wbf face is 1 to 64"

    ascii_cps = [cp for cp in glyphs if cp <= 127]
    extra = sorted(cp for cp in glyphs if cp > 127)
    first = min(ascii_cps) if ascii_cps else 32
    unicode = extra[0] if extra else 0

    def index_of(cp):
        if cp <= 127:
            return cp - first
        return cp - unicode + 128 - first

    last_index = max(index_of(cp) for cp in glyphs)
    if last_index < 0 or last_index > 255:
        return None, "the face has more than 256 glyphs"
    count = last_index + 1
    last = first + last_index

    default = glyphs[ascii_cps[0]]["advance"] if ascii_cps else 4
    widths = []
    packed = []
    for i in range(count):
        cp = first + i if i <= 127 - first else unicode + (i - (128 - first))
        glyph = glyphs.get(cp)
        if glyph is None:
            widths.append(max(default, 1) if default else 1)
            packed.append(pack([[0] * widths[-1] for _ in range(height)], widths[-1]))
            continue
        width, bits = cell(glyph, height, ascent)
        if width > 64:
            return None, f"U+{cp:04X} is {width} px wide, and a .wbf glyph is at most 64"
        widths.append(width)
        packed.append(pack(bits, width))

    variable = len(set(widths)) > 1
    header = bytes((
        0x57,
        height,
        max(widths),
        0,
        0x01 if variable else 0x00,
        first,
        last,
        0,
    )) + unicode.to_bytes(4, "little")
    body = bytes(widths) if variable else b""
    return header + body + b"".join(packed), None


def main():
    parser = argparse.ArgumentParser(description="Convert a BDF font to a .wbf file.")
    parser.add_argument("bdf")
    parser.add_argument("wbf")
    args = parser.parse_args()
    glyphs, ascent, descent = parse_bdf(open(args.bdf, encoding="latin-1").read())
    data, err = build(glyphs, ascent, descent)
    if err:
        print(err, file=sys.stderr)
        return 1
    with open(args.wbf, "wb") as out:
        out.write(data)
    print(f"{args.wbf}: {len(data)} bytes, {len(glyphs)} glyphs, {ascent + descent} px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
