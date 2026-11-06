// Рендер панели на ПК (программный растеризатор) для проверки вёрстки без гарнитуры.
//   g++ -std=c++17 -O2 -I app/src/main/cpp tools/host_preview.cpp -o /tmp/host_preview && /tmp/host_preview
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include "ui.h"
using namespace ui;

static std::vector<uint8_t> atlas;
static float sampleAtlas(float u, float v) {
    const int W = fontdata::kAtlasW, H = fontdata::kAtlasH;
    float x = u * W - 0.5f, y = v * H - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y); float fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy) { xx = std::min(std::max(xx, 0), W - 1); yy = std::min(std::max(yy, 0), H - 1); return atlas[(size_t)yy * W + xx] / 255.0f; };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

struct Img { int w, h; std::vector<float> p; };  // RGBA premultiplied

static void raster(Img& im, const std::vector<Vtx>& v) {
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        const Vtx &a = v[i], &b = v[i + 1], &c = v[i + 2];
        float minx = std::min({a.x, b.x, c.x}), maxx = std::max({a.x, b.x, c.x});
        float miny = std::min({a.y, b.y, c.y}), maxy = std::max({a.y, b.y, c.y});
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) continue;
        for (int y = std::max(0, (int)std::floor(miny)); y <= std::min(im.h - 1, (int)std::ceil(maxy)); ++y)
            for (int x = std::max(0, (int)std::floor(minx)); x <= std::min(im.w - 1, (int)std::ceil(maxx)); ++x) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                float w2 = 1 - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                float u = a.u * w0 + b.u * w1 + c.u * w2, vv = a.v * w0 + b.v * w1 + c.v * w2;
                float cov = sampleAtlas(u, vv);
                float al = a.a * cov;                       // как во фрагментном шейдере
                float r = a.r * al, g = a.g * al, bl = a.b * al;
                float* d = &im.p[((size_t)y * im.w + x) * 4];
                d[0] = r + d[0] * (1 - al); d[1] = g + d[1] * (1 - al); d[2] = bl + d[2] * (1 - al); d[3] = al + d[3] * (1 - al);
            }
    }
}

static void save(const char* path, const Img& panel, int bg) {
    // фон - имитация вида комнаты (градиент), панель поверх (premultiplied over)
    FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", panel.w, panel.h);
    for (int y = 0; y < panel.h; ++y) for (int x = 0; x < panel.w; ++x) {
        float br = bg == 0 ? 0.35f + 0.30f * x / panel.w : 0.25f + 0.1f * ((x / 64 + y / 64) % 2);
        float r = br * 0.95f, g = br * 0.85f, b = br * 0.75f;
        const float* d = &panel.p[((size_t)y * panel.w + x) * 4];
        float R = d[0] + r * (1 - d[3]), G = d[1] + g * (1 - d[3]), B = d[2] + b * (1 - d[3]);
        unsigned char px[3] = {(unsigned char)(std::min(R, 1.f) * 255 + .5f), (unsigned char)(std::min(G, 1.f) * 255 + .5f), (unsigned char)(std::min(B, 1.f) * 255 + .5f)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main() {
    FILE* f = fopen("app/src/main/assets/font.r8", "rb"); if (!f) { puts("нет font.r8"); return 1; }
    atlas.resize((size_t)fontdata::kAtlasW * fontdata::kAtlasH);
    if (fread(atlas.data(), 1, atlas.size(), f) != atlas.size()) { puts("плохой размер атласа"); return 1; } fclose(f);

    // сценарий 1: расширенный режим, выбран "Контраст"
    grade::Settings s; s.v[0] = 0.15f; s.v[1] = 0.42f; s.v[2] = -0.30f; s.v[3] = 0.0f; s.v[4] = 0.65f; s.v[5] = -1.0f;
    View v; v.s = &s; v.selected = 1; v.lutSupported = true; v.lutActive = true;
    Batch b; buildPanel(b, v);
    Img im{kPanelW, kPanelH, std::vector<float>((size_t)kPanelW * kPanelH * 4, 0.f)};
    raster(im, b.v); save("/tmp/panel1.ppm", im, 0);
    printf("сценарий 1: %zu вершин\n", b.v.size());

    // сценарий 2: базовый режим + оригинал
    View v2 = v; v2.lutActive = false; v2.bypass = true; v2.selected = 2;
    buildPanel(b, v2); Img im2{kPanelW, kPanelH, std::vector<float>((size_t)kPanelW * kPanelH * 4, 0.f)};
    raster(im2, b.v); save("/tmp/panel2.ppm", im2, 1);

    // сценарий 3: ошибка
    View v3 = v; v3.error = "Passthrough не запустился (код -1)"; buildPanel(b, v3);
    Img im3{kPanelW, kPanelH, std::vector<float>((size_t)kPanelW * kPanelH * 4, 0.f)}; raster(im3, b.v); save("/tmp/panel3.ppm", im3, 0);

    // ширина самых длинных строк
    using namespace fontdata;
    for (int i = 0; i < 4; ++i) printf("подсказка %d: %.0f px (допустимо <= 928)\n", i + 1, Batch::textWidth(FONT_SMALL, txt::kHints[i]));
    printf("заголовок: %.0f px\n", Batch::textWidth(FONT_TITLE, txt::kTitle));
    for (int i = 0; i < grade::P_COUNT; ++i) printf("метка %d: %.0f px\n", i, Batch::textWidth(FONT_BODY, txt::kLabels[i]));
    return 0;
}
