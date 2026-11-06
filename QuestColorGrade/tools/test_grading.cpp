// Тест расчёта LUT на ПК:  g++ -std=c++17 -O2 -I app/src/main/cpp tools/test_grading.cpp -o /tmp/test_grading
#include <cstdio>
#include <cstring>
#include <vector>
#include "grading.h"
using namespace grade;

static void sample(const std::vector<uint8_t>& lut, int r, int g, int b, int out[3]) {
    size_t i = ((size_t)r + (size_t)g * kLutRes + (size_t)b * kLutRes * kLutRes) * 3;
    out[0] = lut[i]; out[1] = lut[i + 1]; out[2] = lut[i + 2];
}
static void dump(const char* name, const Settings& s) {
    std::vector<uint8_t> lut(kLutBytes);
    buildLut(s, lut.data());
    FILE* f = fopen(name, "wb"); fwrite(lut.data(), 1, lut.size(), f); fclose(f);
}

int main() {
    int fails = 0;
    auto check = [&](bool cond, const char* msg) { printf("%s  %s\n", cond ? "OK  " : "FAIL", msg); if (!cond) ++fails; };

    // 1. Нейтральные настройки = тождественная LUT, и порядок осей (R быстрее всего)
    Settings neutral;
    std::vector<uint8_t> lut(kLutBytes);
    buildLut(neutral, lut.data());
    int c[3];
    sample(lut, 31, 0, 0, c); check(c[0] == 255 && c[1] == 0 && c[2] == 0, "identity: индекс R -> красный");
    sample(lut, 0, 31, 0, c); check(c[0] == 0 && c[1] == 255 && c[2] == 0, "identity: индекс G -> зелёный");
    sample(lut, 0, 0, 31, c); check(c[0] == 0 && c[1] == 0 && c[2] == 255, "identity: индекс B -> синий");
    sample(lut, 16, 16, 16, c); check(c[0] == 16 * 255 / 31, "identity: середина");
    // R меняется быстрее всего: второй триплет в буфере = (1/31, 0, 0)
    check(lut[3] == 255 / 31 && lut[4] == 0 && lut[5] == 0, "раскладка в памяти: следующая точка = R+1");

    // 2. Параметры влияют так, как ожидается
    { Settings s; s.v[P_CONTRAST] = 0.5f; buildLut(s, lut.data());
      sample(lut, 8, 8, 8, c);  // тёмно-серый
      int dark = c[0]; sample(lut, 24, 24, 24, c); int light = c[0];
      check(dark < 8 * 255 / 31 && light > 24 * 255 / 31, "контраст+ : тёмное темнее, светлое светлее");
      printf("      (8/31 было %d -> %d, 24/31 было %d -> %d)\n", 8*255/31, dark, 24*255/31, light); }
    { Settings s; s.v[P_CONTRAST] = -0.5f; buildLut(s, lut.data());
      sample(lut, 4, 4, 4, c); check(c[0] > 4 * 255 / 31, "контраст- : тёмное светлее"); }
    { Settings s; s.v[P_BRIGHTNESS] = 0.5f; buildLut(s, lut.data());
      sample(lut, 16, 16, 16, c); check(c[0] > 16 * 255 / 31, "яркость+ : середина светлее");
      sample(lut, 0, 0, 0, c); check(c[0] == 0, "яркость+ : чёрный остаётся чёрным");
      sample(lut, 31, 31, 31, c); check(c[0] == 255, "яркость+ : белый остаётся белым (ограничение)"); }
    { Settings s; s.v[P_SATURATION] = -1.0f; buildLut(s, lut.data());
      sample(lut, 31, 0, 0, c); check(abs(c[0] - c[1]) <= 1 && abs(c[1] - c[2]) <= 1, "насыщенность=-1 : красный -> серый");
      printf("      (чистый красный -> %d,%d,%d)\n", c[0], c[1], c[2]); }
    { Settings s; s.v[P_SATURATION] = 0.5f; buildLut(s, lut.data());
      sample(lut, 20, 14, 14, c); check(c[0] - c[1] > (20 - 14) * 255 / 31, "насыщенность+ : цвет ярче"); }
    { Settings s; s.v[P_TEMPERATURE] = 0.8f; buildLut(s, lut.data());
      sample(lut, 16, 16, 16, c); check(c[0] > c[1] && c[1] > c[2], "температура+ : серый -> тёплый (R>G>B)");
      printf("      (серый -> %d,%d,%d)\n", c[0], c[1], c[2]); }
    { Settings s; s.v[P_TEMPERATURE] = -0.8f; buildLut(s, lut.data());
      sample(lut, 16, 16, 16, c); check(c[2] > c[1] && c[1] > c[0], "температура- : серый -> холодный (B>G>R)"); }
    { Settings s; s.v[P_TINT] = 0.8f; buildLut(s, lut.data());
      sample(lut, 16, 16, 16, c); check(c[1] < c[0] && c[1] < c[2], "тинт+ : меньше зелёного (пурпурный)"); }
    { Settings s; s.v[P_GAMMA] = 0.5f; buildLut(s, lut.data());
      sample(lut, 16, 16, 16, c); check(c[0] > 16 * 255 / 31, "гамма+ : середина светлее");
      sample(lut, 0, 0, 0, c); check(c[0] == 0, "гамма+ : чёрный = 0");
      sample(lut, 31, 31, 31, c); check(c[0] == 255, "гамма+ : белый = 255"); }

    // 3. Монотонность по серой оси при сильных настройках
    { Settings s; s.v[P_CONTRAST] = 1; s.v[P_BRIGHTNESS] = 0.3f; s.v[P_GAMMA] = -0.4f; buildLut(s, lut.data());
      bool mono = true; int prev = -1;
      for (int i = 0; i < kLutRes; ++i) { sample(lut, i, i, i, c); if (c[0] < prev) mono = false; prev = c[0]; }
      check(mono, "монотонность серой оси при сильных настройках"); }

    // 4. BCS
    { Settings s; Bcs b = toBcs(s);
      check(b.brightness == 0 && std::fabs(b.contrast - 1) < 1e-6f && std::fabs(b.saturation - 1) < 1e-6f, "BCS: нейтрально = (0, 1, 1)"); }
    { Settings s; s.v[P_CONTRAST] = 1; s.v[P_SATURATION] = -1; s.v[P_BRIGHTNESS] = -1; Bcs b = toBcs(s);
      check(std::fabs(b.contrast - 2.5f) < 1e-5f && b.saturation == 0 && b.brightness == -40, "BCS: крайние значения"); }

    // 5. Сохранение/загрузка
    { Settings s; s.v[P_CONTRAST] = 0.37f; s.v[P_TINT] = -0.2f; s.advanced = false;
      check(saveSettings("/tmp/qcg_test.bin", s), "save");
      Settings t; check(loadSettings("/tmp/qcg_test.bin", t) && t.v[P_CONTRAST] == 0.37f && t.v[P_TINT] == -0.2f && !t.advanced, "load = save"); }

    // Файлы для визуальной проверки
    Settings a; a.v[P_CONTRAST] = 0.4f; a.v[P_SATURATION] = 0.2f; dump("/tmp/lut_contrast.bin", a);
    Settings b2; b2.v[P_TEMPERATURE] = 0.6f; b2.v[P_BRIGHTNESS] = 0.2f; dump("/tmp/lut_warm.bin", b2);
    Settings c2; c2.v[P_SATURATION] = -1; dump("/tmp/lut_gray.bin", c2);
    dump("/tmp/lut_identity.bin", neutral);

    printf("\nИтог: %s\n", fails ? "ЕСТЬ ОШИБКИ" : "все проверки пройдены");
    return fails ? 1 : 0;
}
