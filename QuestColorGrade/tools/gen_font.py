#!/usr/bin/env python3
"""Генератор растрового шрифтового атласа для HUD (кириллица + латиница + стрелки).

Результат:
  app/src/main/assets/font.r8        - атлас 8 бит (R8), W*H байт
  app/src/main/cpp/font_data.h       - метрики глифов

Шрифт: Liberation Sans (SIL OFL 1.1). Запуск:  python3 tools/gen_font.py
Требуется Pillow:  pip install pillow
Если вы меняете тексты интерфейса и добавляете новые символы - допишите их в CHARS
и запустите скрипт заново.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
BOLD = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"
# (имя, файл, размер px)
FONTS = [("small", REG, 26), ("body", REG, 32), ("title", BOLD, 46)]

CHARS = [chr(c) for c in range(0x20, 0x7F)]
CHARS += [chr(c) for c in range(0x410, 0x450)] + ["Ё", "ё"]
CHARS += list("«»—–…°±×·•№←↑→↓↔↕")
CHARS = sorted(set(CHARS), key=ord)

PAD = 2
ATLAS_W = 1024


def build():
    tiles = []  # (font_idx, cp, img|None, xoff, yoff, adv)
    metrics = []
    for fi, (name, path, size) in enumerate(FONTS):
        if not os.path.exists(path):
            sys.exit("Нет шрифта: " + path)
        font = ImageFont.truetype(path, size)
        asc, desc = font.getmetrics()
        metrics.append((asc, desc))
        for ch in CHARS:
            adv = font.getlength(ch)
            x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
            w, h = x1 - x0, y1 - y0
            if w <= 0 or h <= 0:
                tiles.append((fi, ord(ch), None, 0, 0, adv))
                continue
            img = Image.new("L", (w + 2 * PAD, h + 2 * PAD), 0)
            ImageDraw.Draw(img).text((PAD - x0, PAD - y0), ch, font=font, fill=255, anchor="ls")
            tiles.append((fi, ord(ch), img, x0 - PAD, y0 - PAD, adv))
    # shelf packing, высокие сначала
    order = sorted(range(len(tiles)), key=lambda i: -(tiles[i][2].height if tiles[i][2] else 0))
    pos = {}
    x = y = shelf_h = 0
    for i in order:
        img = tiles[i][2]
        if img is None:
            continue
        w, h = img.size
        if x + w + 1 > ATLAS_W:
            x, y, shelf_h = 0, y + shelf_h + 1, 0
        pos[i] = (x, y)
        x += w + 1
        shelf_h = max(shelf_h, h)
    used_h = y + shelf_h + 8  # + место под белый блок 4x4
    atlas_h = 256
    while atlas_h < used_h:
        atlas_h *= 2
    atlas = Image.new("L", (ATLAS_W, atlas_h), 0)
    for i, (px, py) in pos.items():
        atlas.paste(tiles[i][2], (px, py))
    # белый блок для сплошных заливок
    wx, wy = ATLAS_W - 6, atlas_h - 6
    atlas.paste(255, (wx, wy, wx + 4, wy + 4))
    return tiles, pos, metrics, atlas, wx, wy


def main():
    tiles, pos, metrics, atlas, wx, wy = build()
    W, H = atlas.size
    with open(os.path.join(ROOT, "app/src/main/assets/font.r8"), "wb") as f:
        f.write(atlas.tobytes())
    atlas.save(os.path.join(ROOT, "tools/font_atlas_preview.png"))
    out = []
    out.append("// АВТОГЕНЕРАЦИЯ: tools/gen_font.py - не редактировать вручную\n#pragma once\n#include <cstdint>\n\n")
    out.append("namespace fontdata {\n")
    out.append(f"constexpr int kAtlasW = {W};\nconstexpr int kAtlasH = {H};\n")
    out.append(f"constexpr int kWhiteX = {wx};\nconstexpr int kWhiteY = {wy};\n\n")
    out.append("struct Glyph { uint32_t cp; uint16_t x, y, w, h; int16_t xoff, yoff; float adv; };\n")
    out.append("struct FontInfo { const Glyph* glyphs; int count; float ascent; float descent; };\n\n")
    for fi, (name, path, size) in enumerate(FONTS):
        out.append(f"// {name}: {os.path.basename(path)} {size}px\nstatic const Glyph kGlyphs{fi}[] = {{\n")
        for i, (f_i, cp, img, xoff, yoff, adv) in enumerate(tiles):
            if f_i != fi:
                continue
            if img is None:
                out.append(f"  {{{cp}, 0, 0, 0, 0, 0, 0, {adv:.3f}f}},\n")
            else:
                px, py = pos[i]
                out.append(f"  {{{cp}, {px}, {py}, {img.width}, {img.height}, {xoff}, {yoff}, {adv:.3f}f}},\n")
        out.append("};\n")
    out.append(f"static const FontInfo kFonts[{len(FONTS)}] = {{\n")
    for fi, (name, path, size) in enumerate(FONTS):
        asc, desc = metrics[fi]
        out.append(f"  {{kGlyphs{fi}, (int)(sizeof(kGlyphs{fi}) / sizeof(Glyph)), {asc}.0f, {desc}.0f}},\n")
    out.append("};\n")
    out.append("enum FontId { FONT_SMALL = 0, FONT_BODY = 1, FONT_TITLE = 2 };\n} // namespace fontdata\n")
    with open(os.path.join(ROOT, "app/src/main/cpp/font_data.h"), "w", encoding="utf-8") as f:
        f.write("".join(out))
    print(f"атлас {W}x{H}, символов на шрифт: {len(CHARS)}, white=({wx},{wy})")


if __name__ == "__main__":
    main()
