#!/usr/bin/env python
"""depng_ico.py - rewrite .ico files so every image is a classic BMP/DIB entry.

Icon editors happily store images inside an .ico as PNG streams. Windows only
learned to decode those in Vista; on 95/98/ME/2000/XP a PNG entry is not an
icon at all, so LoadIcon either fails or falls back to a lesser image.

Since scrctl.exe must run from Windows 95 up, no PNG entry may survive into
the resource. This decodes each PNG entry and re-encodes it as a 32-bpp BGRA
DIB with an AND mask derived from the alpha channel, so the artwork is
unchanged while remaining readable by every Windows version. BMP entries that
are already present are copied through byte for byte.

    python tools/depng_ico.py icons/enabled.ico icons/disabled.ico

Files are rewritten in place; a .png-backup copy is left beside each one.
"""
import struct
import sys
import os
import shutil

try:
    from PIL import Image
except ImportError:
    sys.exit("error: this needs Pillow (pip install Pillow)")

if sys.version_info[0] >= 3:
    import io as _io
    BytesIO = _io.BytesIO
else:
    import StringIO as _s
    BytesIO = _s.StringIO


def png_to_dib(png_bytes):
    """Decode a PNG icon image and return it as a 32-bpp DIB with AND mask."""
    im = Image.open(BytesIO(png_bytes)).convert("RGBA")
    w, h = im.size
    px = im.load()

    xor = bytearray()
    for y in range(h - 1, -1, -1):                 # DIB rows run bottom-up
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes(bytearray((b, g, r, a)))  # BGRA

    # 1-bpp AND mask, rows padded to 4 bytes, 1 = transparent.
    # Modern Windows uses the alpha channel and ignores this, but 9x/NT4 do not.
    stride = ((w + 31) // 32) * 4
    andmask = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(stride)
        for x in range(w):
            if px[x, y][3] < 128:
                row[x >> 3] |= 0x80 >> (x & 7)
        andmask += row

    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0,
                      len(xor) + len(andmask), 0, 0, 0, 0)
    return bytes(hdr) + bytes(xor) + bytes(andmask), w, h


def convert(path):
    data = open(path, "rb").read()
    reserved, typ, count = struct.unpack("<HHH", data[:6])
    if reserved != 0 or typ != 1:
        sys.exit("%s: not an .ico file" % path)

    images, converted = [], 0
    for i in range(count):
        w, h, ncol, res, planes, bpp, size, off = struct.unpack(
            "<BBBBHHII", data[6 + i * 16:22 + i * 16])
        blob = data[off:off + size]
        if blob[:4] == b"\x89PNG":
            blob, rw, rh = png_to_dib(blob)
            w, h, bpp, ncol = rw & 0xFF, rh & 0xFF, 32, 0
            converted += 1
        images.append((w, h, ncol, res, planes, bpp, blob))

    if not converted:
        print("  %-24s already PNG-free, untouched" % os.path.basename(path))
        return

    shutil.copy2(path, path + ".png-backup")

    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    body = b""
    for w, h, ncol, res, planes, bpp, blob in images:
        out += struct.pack("<BBBBHHII", w, h, ncol, res, planes, bpp,
                           len(blob), offset)
        offset += len(blob)
        body += blob
    open(path, "wb").write(out + body)
    print("  %-24s %d PNG entries -> DIB, %d images, %d bytes"
          % (os.path.basename(path), converted, len(images), len(out + body)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        convert(p)
