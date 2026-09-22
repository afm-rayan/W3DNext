#!/usr/bin/env python3
r"""Regenerate tangent-space normal maps for Generals / Zero Hour textures.

Output format matches what the W3DNext engine expects:
  - Terrain:  Art/Terrain/<name>_n.dds  -> uncompressed 24-bit RGB DDS
              (WorldHeightMap.cpp readTexClassNormal / readTilesDDS, only
              accepts DDPF_RGB 24bpp, no FOURCC), same size as the base tga.
  - Models:   Art/Textures/<name>_n.dds -> DXT5 (BC3) DDS, the format the
              engine's DDSFileClass requires and every image viewer opens.
              (legacy 24-bit path kept as --format dds; tga via --format tga)

The normal map is derived from the diffuse luminance using a Sobel gradient,
normalized to unit vectors and encoded in tangent space.

Standard (models, DXT5/DDSFileClass, verified working):
  R = (nx+1)/2, G = (ny+1)/2, B = (nz+1)/2.

Terrain 24-bit DDS is different: readTilesDDS + the atlas update() write into
an A8R8G8B8 surface (memory byte order B,G,R,A), so the shader ends up reading
Z from FILE BYTE 0, Y from byte 1, X from byte 2. Therefore terrain _n files
must be stored with the channels swapped relative to the standard encoding:
  R(byte0) = (nz+1)/2, G(byte1) = (ny+1)/2, B(byte2) = (nx+1)/2  (see write_dds).

Inputs: .tga/.png/.bmp/.jpg/.jpeg (via Pillow) and .dds (own parser;
uncompressed 24/32bpp and DXT1/3/5). Files whose name already looks like a
normal map (*_n/_N, normal, bump) and files that already have a *_n sibling
(tga or dds) are skipped (unless --force).

Usage:
    python gen_normalmaps.py <folder> [--dry-run] [--format dds|dxt5|tga]
                             [--strength 1.0] [--flip-y] [--force]
"""

import argparse
import os
import struct
import sys

try:
    import numpy as np
    import PIL.Image
except ImportError:
    print("requires: Pillow and numpy (pip install pillow numpy)")
    sys.exit(2)


# ---------------------------------------------------------------- DDS reader
def _dxt1_block(data, bc, out):
    c0 = struct.unpack_from("<H", data, bc)[0]
    c1 = struct.unpack_from("<H", data, bc + 2)[0]
    r0, g0, b0 = _rgb565(c0)
    r1, g1, b1 = _rgb565(c1)
    colors = [(r0, g0, b0), (r1, g1, b1)]
    if c0 > c1:
        colors += [((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3),
                   ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3)]
    else:
        colors += [((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2),
                   (0, 0, 0)]
    bits = struct.unpack_from("<I", data, bc + 4)[0]
    for y in range(4):
        for x in range(4):
            out[y * 4 + x] = colors[(bits >> (2 * (y * 4 + x))) & 3]


def _dxt3_block(data, bc, out):
    _dxt1_block(data, bc + 8, out)


def _dxt5_block(data, bc, out):
    _dxt1_block(data, bc + 8, out)


def _rgb565(v):
    return ((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31


def load_image(path):
    """Return RGB float array (H, W, 3) in 0..1, or None on failure."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".dds":
        return _load_dds(path)
    try:
        img = PIL.Image.open(path).convert("RGB")
        arr = np.asarray(img, dtype=np.float32) / 255.0
        return arr
    except Exception:
        return None


def _load_dds(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 128 or data[:4] != b"DDS ":
        return None
    dwSize, dwFlags, dwHeight, dwWidth = struct.unpack_from("<4I", data, 4)
    ddpfFlags, ddpfFourCC, ddpfRGBBitCount = struct.unpack_from("<3I", data, 80)
    if not (dwFlags & 0x1000):  # DDSD_PIXELFORMAT
        return None
    w, h = int(dwWidth), int(dwHeight)
    if w <= 0 or h <= 0:
        return None
    pix = None
    if (ddpfFlags & 0x4):  # DDPF_FOURCC
        fourcc = ddpfFourCC
        bpp = 4
        if fourcc == 0x31545844:  # DXT1
            decoder = _dxt1_block
        elif fourcc == 0x33545844:  # DXT3
            decoder = _dxt3_block
        elif fourcc == 0x35545844:  # DXT5
            decoder = _dxt5_block
        else:
            return None
        rgb = np.empty((h, w, 3), dtype=np.uint8)
        blocks_x = (w + 3) // 4
        blocks_y = (h + 3) // 4
        for by in range(blocks_y):
            for bx in range(blocks_x):
                bc = 128 + (by * blocks_x + bx) * (8 if decoder is _dxt1_block else 16)
                out = np.empty((16, 3), dtype=np.uint8)
                decoder(data, bc, out)
                y0 = by * 4
                x0 = bx * 4
                for yy in range(4):
                    for xx in range(4):
                        if y0 + yy < h and x0 + xx < w:
                            rgb[y0 + yy, x0 + xx] = out[yy * 4 + xx]
        pix = rgb.astype(np.float32) / 255.0
    elif (ddpfFlags & 0x40) and ddpfRGBBitCount in (24, 32):
        bpp = ddpfRGBBitCount // 8
        row_len = (w * bpp + 3) & ~3
        off = 128
        arr = np.frombuffer(data, dtype=np.uint8, count=row_len * h, offset=off).reshape(h, row_len)
        if ddpfRGBBitCount == 24:
            rgb = np.empty((h, w, 3), dtype=np.uint8)
            rgb[:, :, 0] = arr[:, 0::3][:, :w]
            rgb[:, :, 1] = arr[:, 1::3][:, :w]
            rgb[:, :, 2] = arr[:, 2::3][:, :w]
        else:
            bgra = arr.reshape(h, -1, 4)
            rgb = bgra[:, :, 2], bgra[:, :, 1], bgra[:, :, 0]
            rgb = np.stack(rgb, axis=-1)[:, :w]
        pix = rgb.astype(np.float32) / 255.0
    else:
        return None
    return np.ascontiguousarray(pix)


# ---------------------------------------------------------- normal generation
def make_normal_map(rgb, strength, flip_y):
    gray = rgb.astype(np.float32).mean(axis=2)
    h, w = gray.shape
    pad = np.pad(gray, 1, mode="edge")
    dx = (pad[1:-1, 2:] - pad[1:-1, :-2]) / 2.0
    dy = (pad[2:, 1:-1] - pad[:-2, 1:-1]) / 2.0
    if flip_y:
        dy = -dy
    nx = -dx * strength
    ny = -dy * strength
    nz = np.ones_like(nx)
    inv = 1.0 / np.sqrt(np.maximum(1e-8, nx * nx + ny * ny + nz * nz))
    nx, ny, nz = nx * inv, ny * inv, nz * inv
    r = (nx * 0.5 + 0.5) * 255.0
    g = (ny * 0.5 + 0.5) * 255.0
    b = (nz * 0.5 + 0.5) * 255.0
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def write_dds(path, rgb):
    """Uncompressed 24-bit RGB DDS, top-down scanlines (matches readTilesDDS).

    Terrain-only format. The engine maps FILE BYTE 0 -> shader Z (see module
    docstring), so channels are stored as byte0=Z, byte1=Y, byte2=X, i.e. the
    prepared rgb[:, :, 0]=R(nx), [1]=G(ny), [2]=B(nz) is written swapped."""
    h, w, _ = rgb.shape
    pitch = (w * 3 + 3) & ~3
    hdr = bytearray(128)
    struct.pack_into("<4s", hdr, 0, b"DDS ")
    struct.pack_into("<I", hdr, 4, 124)
    struct.pack_into("<I", hdr, 8, 0x1000 | 0x8 | 0x4 | 0x2 | 0x1)  # PIXELFORMAT|PITCH|WIDTH|HEIGHT|CAPS
    struct.pack_into("<I", hdr, 12, h)
    struct.pack_into("<I", hdr, 16, w)
    struct.pack_into("<I", hdr, 20, pitch)
    struct.pack_into("<I", hdr, 24, 0)
    struct.pack_into("<I", hdr, 28, 1)
    struct.pack_into("<I", hdr, 76, 32)   # DDPIXELFORMAT.dwSize
    struct.pack_into("<I", hdr, 80, 0x40)  # DDPIXELFORMAT.dwFlags = DDPF_RGB
    struct.pack_into("<I", hdr, 84, 0)     # DDPIXELFORMAT.dwFourCC
    struct.pack_into("<I", hdr, 88, 24)    # DDPIXELFORMAT.dwRGBBitCount
    struct.pack_into("<I", hdr, 92, 0x00FF0000)  # dwRBitMask
    struct.pack_into("<I", hdr, 96, 0x0000FF00)  # dwGBitMask
    struct.pack_into("<I", hdr, 100, 0x000000FF) # dwBBitMask
    struct.pack_into("<I", hdr, 104, 0x00000000) # dwABitMask
    struct.pack_into("<I", hdr, 108, 0x1000)  # DDSCAPS_TEXTURE
    data = np.zeros((h, pitch), dtype=np.uint8)
    px = w * 3
    data[:, 0:px:3] = rgb[:, :, 2]  # byte0 = nz
    data[:, 1:px:3] = rgb[:, :, 1]  # byte1 = ny
    data[:, 2:px:3] = rgb[:, :, 0]  # byte2 = nx
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(data.tobytes())


def write_tga(path, rgb):
    h, w, _ = rgb.shape
    arr = b""
    for y in range(h):
        for x in range(w):
            arr += bytes((rgb[y, x, 0], rgb[y, x, 1], rgb[y, x, 2]))
    hdr = bytes((0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0)) + \
          struct.pack("<HH", w, h) + bytes((24, 0x20))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(arr)


NORMAL_RE = ("_n", "_N", "_nrm", "_normal", "_bump", "normal", "bump")


def _l565(r, g, b):
    return ((int(r) >> 3) << 11) | ((int(g) >> 2) << 5) | (int(b) >> 3)


def _un565(c):
    return ((c >> 11) & 0x1F) * 255 // 31, ((c >> 5) & 0x3F) * 255 // 63, (c & 0x1F) * 255 // 31


def _encode_dxt1_color(sub, H, W, y0, x0):
    """DXT1 8-byte color block for a 4x4 (clamped) region. 4-color mode."""
    bh, bw = min(4, H - y0), min(4, W - x0)
    lo = sub.reshape(-1, 3).min(axis=0)
    hi = sub.reshape(-1, 3).max(axis=0)
    c_lo = _l565(*lo)
    c_hi = _l565(*hi)
    if c_hi < c_lo:
        c0, c1 = c_lo, c_hi
    else:
        c0, c1 = c_hi, c_lo
    r0, g0, b0 = _un565(c0)
    r1, g1, b1 = _un565(c1)
    pal = np.array([[r0, g0, b0], [r1, g1, b1],
                    [(2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3],
                    [(r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3]], dtype=np.float32)
    f = sub.astype(np.float32)  # (bh,bw,3)
    d = f[:, :, None, :] - pal[None, None, :, :]
    idx = (d * d).sum(-1).argmin(-1)  # (bh,bw)
    bits = 0
    for yy in range(bh):
        for xx in range(bw):
            bits |= int(idx[yy, xx]) << (2 * (yy * 4 + xx))
    return struct.pack("<2HI", c0, c1, bits)


def write_dxt5(path, rgb):
    """DXT5 (BC3) DDS - the format the engine's DDSFileClass requires for
    regular textures and every image viewer opens."""
    h, w, _ = rgb.shape
    bx = (w + 3) // 4
    by = (h + 3) // 4
    body = bytearray()
    for y in range(by):
        for x in range(bx):
            sub = rgb[y * 4:y * 4 + 4, x * 4:x * 4 + 4]
            body += b"\xff\x00" + b"\x00" * 6  # alpha constant 255 (a0=255,a1=0, idx 0)
            body += _encode_dxt1_color(sub, h, w, y * 4, x * 4)
    hdr = bytearray(128)
    struct.pack_into("<4s", hdr, 0, b"DDS ")
    struct.pack_into("<I", hdr, 4, 124)
    struct.pack_into("<I", hdr, 8, 0x1000 | 0x80000 | 0x6 | 0x1)  # PIXELFORMAT|LINEARSIZE|WIDTH|HEIGHT|CAPS
    struct.pack_into("<I", hdr, 12, h)
    struct.pack_into("<I", hdr, 16, w)
    struct.pack_into("<I", hdr, 20, bx * by * 16)
    struct.pack_into("<I", hdr, 24, 0)
    struct.pack_into("<I", hdr, 28, 1)
    struct.pack_into("<I", hdr, 76, 32)   # DDPIXELFORMAT.dwSize
    struct.pack_into("<I", hdr, 80, 0x4)  # DDPF_FOURCC
    struct.pack_into("<I", hdr, 84, 0x35545844)  # 'DXT5'
    struct.pack_into("<I", hdr, 88, 0)
    struct.pack_into("<I", hdr, 108, 0x1000)  # DDSCAPS_TEXTURE
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(body)


def is_normal_name(name):
    base = os.path.splitext(name)[0].lower()
    return base.endswith("_n") or any(tok in base for tok in ("_nrm", "_normal", "_bump", "normal", "bump"))


def _matches_dds_format(path, fmt):
    """True if an existing file already has the target DDS encoding (for resume)."""
    try:
        with open(path, "rb") as f:
            if f.read(4) != b"DDS ":
                return False
            f.seek(80)
            flags = f.read(4)[0]
            fourcc = struct.unpack("<I", f.read(4))[0]
            f.seek(84 + 4)
            bits = struct.unpack("<I", f.read(4))[0]
    except (OSError, struct.error):
        return False
    if fmt == "dxt5":
        return fourcc == 0x35545844 and (flags & 0x4)
    if fmt == "dds":
        return fourcc == 0 and (flags & 0x40) and bits == 24
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("folder")
    ap.add_argument("--dry-run", action="store_true", help="only report what would be generated")
    ap.add_argument("--format", choices=("dds", "dxt5", "tga"), default="dds",
                    help="dds=uncompressed 24-bit (terrain/readTilesDDS), "
                         "dxt5=BC3 (models/DDSFileClass + opens everywhere), tga")
    ap.add_argument("--strength", type=float, default=1.0, help="gradient strength (default 1.0)")
    ap.add_argument("--flip-y", action="store_true", help="invert the green channel")
    ap.add_argument("--ext", action="append", default=None,
                    help="restrict to a file extension (.tga, .dds, ...); repeatable")
    ap.add_argument("--force", action="store_true",
                    help="regenerate even if a *_n normal map sibling already exists")
    ap.add_argument("--force-all", action="store_true",
                    help="with --force, re-encode even when the existing file already has the target format")
    args = ap.parse_args()
    if args.force_all:
        args.force = True

    folder = os.path.abspath(args.folder)
    if not os.path.isdir(folder):
        print("not a folder:", folder)
        sys.exit(2)

    exts = {e.lower() if e.startswith(".") else "." + e.lower() for e in args.ext} if args.ext else None
    files = []
    for root, _, names in os.walk(folder):
        for n in names:
            ext = os.path.splitext(n)[1].lower()
            if ext not in (".tga", ".png", ".bmp", ".jpg", ".jpeg", ".dds"):
                continue
            if exts and ext not in exts:
                continue
            if is_normal_name(n):
                continue
            base = os.path.splitext(n)[0]
            out_name = base + ("_n.dds" if args.format in ("dds", "dxt5") else "_n.tga")
            out_path = os.path.join(root, out_name)
            if args.force:
                if not args.force_all and args.format in ("dds", "dxt5") and _matches_dds_format(out_path, args.format):
                    continue
            elif os.path.exists(out_path):
                continue
            files.append(os.path.join(root, n))

    if args.dry_run:
        print("would generate %d normal maps in %s" % (len(files), folder))
        for f in files[:20]:
            print("  ", os.path.relpath(f, folder))
        if len(files) > 20:
            print("   ... and %d more" % (len(files) - 20))
        return

    gen = failed = skipped = 0
    for path in files:
        base = os.path.splitext(path)[0]
        out = base + ("_n.dds" if args.format in ("dds", "dxt5") else "_n.tga")
        rgb = load_image(path)
        if rgb is None:
            failed += 1
            print("FAIL  ", os.path.relpath(path, folder))
            continue
        nm = make_normal_map(rgb, args.strength, args.flip_y)
        if args.format == "dds":
            write_dds(out, nm)
        elif args.format == "dxt5":
            write_dxt5(out, nm)
        else:
            write_tga(out, nm)
        gen += 1
        if gen % 25 == 0:
            print("  ... %d generated" % gen)
    print("done: %d generated, %d skipped/failed, %d total candidates" % (gen, failed, len(files)))


if __name__ == "__main__":
    main()