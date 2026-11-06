#pragma once
// ui.h - раскладка панели управления (HUD). Строит список вершин (треугольники) в пикселях
// текстуры панели; отрисовкой занимается gles_renderer.h. Не зависит от Android/OpenXR/GL.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "font_data.h"
#include "grading.h"

namespace ui {

constexpr int kPanelW = 1024;
constexpr int kPanelH = 768;

struct Color { float r, g, b, a; };
struct Vtx { float x, y, u, v; float r, g, b, a; };

// ------------------------------- Тексты (можно редактировать) ---------------------------
namespace txt {
constexpr const char* kTitle = "Цветокоррекция passthrough";
constexpr const char* kLabels[grade::P_COUNT] = {
    "Яркость", "Контраст", "Насыщенность", "Гамма",
    "Температура (холод ↔ тепло)", "Тинт (зелёный ↔ пурпурный)"};
constexpr const char* kModeAdvanced = "Режим: расширенный (3D-LUT)";
constexpr const char* kModeBasic = "Режим: базовый (фильтр системы)";
constexpr const char* kModeBasicNoLut = "Режим: базовый (LUT не поддерживается)";
constexpr const char* kOnlyAdvanced = "только расширенный режим";
constexpr const char* kOriginal = "ОРИГИНАЛ";
constexpr const char* kHints[4] = {
    "Стик ↑↓ — выбор параметра    ←→ — изменить (триггер — точнее)",
    "A — сброс параметра    Y — сброс всех    X — оригинал / коррекция",
    "B — скрыть / показать панель    Нажатие правого стика — панель сюда",
    "Нажатие левого стика — режим (базовый / расширенный)"};
}  // namespace txt

// ------------------------------------ Палитра -------------------------------------------
namespace col {
constexpr Color kPanel{0.055f, 0.065f, 0.085f, 0.88f};
constexpr Color kPanelEdge{1.0f, 1.0f, 1.0f, 0.10f};
constexpr Color kSelect{0.25f, 0.55f, 1.0f, 0.20f};
constexpr Color kAccent{0.30f, 0.62f, 1.0f, 1.0f};
constexpr Color kTrack{1.0f, 1.0f, 1.0f, 0.16f};
constexpr Color kTick{1.0f, 1.0f, 1.0f, 0.45f};
constexpr Color kText{1.0f, 1.0f, 1.0f, 0.96f};
constexpr Color kTextDim{1.0f, 1.0f, 1.0f, 0.38f};
constexpr Color kTextSoft{1.0f, 1.0f, 1.0f, 0.62f};
constexpr Color kWarn{1.0f, 0.72f, 0.20f, 1.0f};
constexpr Color kWarnText{0.10f, 0.08f, 0.02f, 1.0f};
constexpr Color kKnob{1.0f, 1.0f, 1.0f, 1.0f};
}  // namespace col

// ------------------------------- Состояние для отрисовки --------------------------------
struct View {
    const grade::Settings* s = nullptr;
    int selected = 0;
    bool lutSupported = false;  // расширение LUT доступно
    bool lutActive = false;     // сейчас используется LUT
    bool bypass = false;        // показываем оригинал
    const char* error = nullptr;
};

// ---------------------------------- Построитель вершин ----------------------------------
class Batch {
public:
    std::vector<Vtx> v;

    void clear() { v.clear(); }

    void tri(float x0, float y0, float x1, float y1, float x2, float y2, const Color& c) {
        const float u = (fontdata::kWhiteX + 2.0f) / fontdata::kAtlasW;
        const float w = (fontdata::kWhiteY + 2.0f) / fontdata::kAtlasH;
        v.push_back({x0, y0, u, w, c.r, c.g, c.b, c.a});
        v.push_back({x1, y1, u, w, c.r, c.g, c.b, c.a});
        v.push_back({x2, y2, u, w, c.r, c.g, c.b, c.a});
    }

    void rect(float x, float y, float w, float h, const Color& c) {
        tri(x, y, x + w, y, x + w, y + h, c);
        tri(x, y, x + w, y + h, x, y + h, c);
    }

    // Круг / сектор (веер треугольников)
    void fan(float cx, float cy, float r, float a0, float a1, int segs, const Color& c) {
        for (int i = 0; i < segs; ++i) {
            const float t0 = a0 + (a1 - a0) * (float)i / segs;
            const float t1 = a0 + (a1 - a0) * (float)(i + 1) / segs;
            tri(cx, cy, cx + r * std::cos(t0), cy + r * std::sin(t0), cx + r * std::cos(t1),
                cy + r * std::sin(t1), c);
        }
    }
    void circle(float cx, float cy, float r, const Color& c) { fan(cx, cy, r, 0.0f, 6.2831853f, 24, c); }

    // Скруглённый прямоугольник
    void rrect(float x, float y, float w, float h, float r, const Color& c) {
        constexpr float kPi = 3.14159265f;
        if (r > w * 0.5f) r = w * 0.5f;
        if (r > h * 0.5f) r = h * 0.5f;
        rect(x + r, y, w - 2 * r, h, c);
        rect(x, y + r, r, h - 2 * r, c);
        rect(x + w - r, y + r, r, h - 2 * r, c);
        fan(x + r, y + r, r, kPi, 1.5f * kPi, 6, c);
        fan(x + w - r, y + r, r, 1.5f * kPi, 2.0f * kPi, 6, c);
        fan(x + w - r, y + h - r, r, 0.0f, 0.5f * kPi, 6, c);
        fan(x + r, y + h - r, r, 0.5f * kPi, kPi, 6, c);
    }

    // ------------------------------------- Текст ----------------------------------------
    static uint32_t nextCp(const char*& p) {
        const unsigned char c = (unsigned char)*p++;
        if (c < 0x80) return c;
        int extra = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else return '?';
        while (extra-- > 0) {
            if ((*p & 0xC0) != 0x80) return '?';
            cp = (cp << 6) | ((unsigned char)*p++ & 0x3F);
        }
        return cp;
    }

    static const fontdata::Glyph* findGlyph(int font, uint32_t cp) {
        const fontdata::FontInfo& f = fontdata::kFonts[font];
        int lo = 0, hi = f.count - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (f.glyphs[mid].cp == cp) return &f.glyphs[mid];
            if (f.glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
        }
        return nullptr;
    }

    static float textWidth(int font, const char* s) {
        float w = 0;
        while (*s) {
            uint32_t cp = nextCp(s);
            const fontdata::Glyph* g = findGlyph(font, cp);
            if (!g) g = findGlyph(font, '?');
            if (g) w += g->adv;
        }
        return w;
    }

    // x - левый край, y - базовая линия
    void text(int font, float x, float y, const char* s, const Color& c) {
        float pen = x;
        while (*s) {
            uint32_t cp = nextCp(s);
            const fontdata::Glyph* g = findGlyph(font, cp);
            if (!g) g = findGlyph(font, '?');
            if (!g) continue;
            if (g->w > 0 && g->h > 0) {
                const float x0 = std::floor(pen + g->xoff + 0.5f);
                const float y0 = std::floor(y + g->yoff + 0.5f);
                const float u0 = (float)g->x / fontdata::kAtlasW, u1 = (float)(g->x + g->w) / fontdata::kAtlasW;
                const float v0 = (float)g->y / fontdata::kAtlasH, v1 = (float)(g->y + g->h) / fontdata::kAtlasH;
                const float x1 = x0 + g->w, y1 = y0 + g->h;
                v.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a});
                v.push_back({x1, y0, u1, v0, c.r, c.g, c.b, c.a});
                v.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
                v.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a});
                v.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
                v.push_back({x0, y1, u0, v1, c.r, c.g, c.b, c.a});
            }
            pen += g->adv;
        }
    }
    void textRight(int font, float xRight, float y, const char* s, const Color& c) {
        text(font, xRight - textWidth(font, s), y, s, c);
    }
};

// ---------------------------------- Раскладка панели ------------------------------------
constexpr float kRowsTop = 142.0f;
constexpr float kRowH = 76.0f;
constexpr float kLeft = 48.0f;
constexpr float kRight = 976.0f;

inline int maxSelectable(const View& view) { return view.lutActive ? grade::P_COUNT - 1 : grade::kBasicParams - 1; }

inline void formatValue(float v, char* buf, size_t n) {
    const int p = (int)std::lround(v * 100.0f);
    if (p > 0) std::snprintf(buf, n, "+%d", p);
    else std::snprintf(buf, n, "%d", p);
}

inline void buildPanel(Batch& b, const View& view) {
    using namespace fontdata;
    b.clear();
    // Фон
    b.rrect(6, 6, kPanelW - 12, kPanelH - 12, 30, col::kPanelEdge);
    b.rrect(8, 8, kPanelW - 16, kPanelH - 16, 28, col::kPanel);

    // Заголовок и строка статуса
    b.text(FONT_TITLE, kLeft, 66, txt::kTitle, col::kText);
    const char* mode = view.lutActive ? txt::kModeAdvanced : (view.lutSupported ? txt::kModeBasic : txt::kModeBasicNoLut);
    b.text(FONT_SMALL, kLeft, 106, mode, col::kTextSoft);
    if (view.bypass) {
        const float tw = Batch::textWidth(FONT_BODY, txt::kOriginal);
        const float px = kRight - tw - 36.0f;
        b.rrect(px, 78, tw + 36.0f, 40, 20, col::kWarn);
        b.text(FONT_BODY, px + 18, 108, txt::kOriginal, col::kWarnText);
    }

    // Строки с ползунками
    const float cx = (kLeft + kRight) * 0.5f;
    const float half = (kRight - kLeft) * 0.5f;
    for (int i = 0; i < grade::P_COUNT; ++i) {
        const float y = kRowsTop + i * kRowH;
        const bool enabled = i <= maxSelectable(view);
        const bool selected = (i == view.selected) && enabled;
        const float val = view.s->v[i];

        if (selected) {
            b.rrect(24, y, kPanelW - 48, kRowH - 6, 14, col::kSelect);
            b.rrect(24, y + 10, 6, kRowH - 26, 3, col::kAccent);
        }
        b.text(FONT_BODY, kLeft, y + 32, txt::kLabels[i], enabled ? (selected ? col::kText : col::kTextSoft) : col::kTextDim);

        if (enabled) {
            char buf[16];
            formatValue(val, buf, sizeof(buf));
            b.textRight(FONT_BODY, kRight, y + 32, buf, selected ? col::kAccent : col::kText);
        } else {
            b.textRight(FONT_SMALL, kRight, y + 30, txt::kOnlyAdvanced, col::kTextDim);
        }

        // Дорожка ползунка
        const float ty = y + 50.0f;
        const float kx = cx + val * half;
        b.rrect(kLeft, ty, kRight - kLeft, 8, 4, enabled ? col::kTrack : Color{1, 1, 1, 0.07f});
        if (enabled) {
            const float fx0 = std::min(cx, kx), fx1 = std::max(cx, kx);
            if (fx1 - fx0 > 1.0f) b.rect(fx0, ty + 1, fx1 - fx0, 6, selected ? col::kAccent : Color{0.30f, 0.62f, 1.0f, 0.55f});
            b.rect(cx - 1.0f, ty - 7.0f, 2.0f, 22.0f, col::kTick);
            b.circle(kx, ty + 4.0f, selected ? 14.0f : 11.0f, col::kKnob);
        }
    }

    // Подсказки
    const float hy = kRowsTop + grade::P_COUNT * kRowH + 14.0f;
    for (int i = 0; i < 4; ++i) b.text(FONT_SMALL, kLeft, hy + 24.0f + i * 33.0f, txt::kHints[i], col::kTextSoft);

    // Сообщение об ошибке поверх всего
    if (view.error && view.error[0]) {
        b.rrect(40, 300, kPanelW - 80, 170, 20, Color{0.35f, 0.06f, 0.06f, 1.0f});
        b.text(FONT_BODY, 64, 360, "Ошибка", col::kWarn);
        b.text(FONT_SMALL, 64, 410, view.error, col::kText);
    }
}

}  // namespace ui
