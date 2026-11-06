#pragma once
// grading.h - параметры цветокоррекции, расчёт 3D-LUT и значений для базового режима (BCS).
// Не зависит от Android/OpenXR, поэтому код можно тестировать на обычном ПК.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace grade {

// Порядок важен: первые kBasicParams работают и в базовом режиме.
enum Param {
    P_BRIGHTNESS = 0,  // яркость
    P_CONTRAST,        // контраст
    P_SATURATION,      // насыщенность
    P_GAMMA,           // гамма (средние тона)
    P_TEMPERATURE,     // температура: холодный <-> тёплый
    P_TINT,            // тинт: зелёный <-> пурпурный
    P_COUNT
};
constexpr int kBasicParams = 3;

struct Settings {
    float v[P_COUNT] = {0, 0, 0, 0, 0, 0};  // каждое значение -1..+1, 0 = без изменений
    bool advanced = true;                    // true = расширенный режим (LUT), если поддерживается

    bool isNeutral() const {
        for (int i = 0; i < P_COUNT; ++i)
            if (std::fabs(v[i]) > 1e-3f) return false;
        return true;
    }
    void clampAll() {
        for (int i = 0; i < P_COUNT; ++i) {
            if (!(v[i] == v[i])) v[i] = 0.0f;  // NaN
            v[i] = std::min(1.0f, std::max(-1.0f, v[i]));
        }
    }
};

// ---------------------------------------------------------------------------------------
// Базовый режим: системный фильтр XR_FB_passthrough (яркость / контраст / насыщенность)
// Нейтральные значения по спецификации: brightness = 0, contrast = 1, saturation = 1.
// ---------------------------------------------------------------------------------------
struct Bcs {
    float brightness;  // сдвиг L* в CIELAB, -100..100
    float contrast;    // множитель, 0..inf (1 = без изменений)
    float saturation;  // множитель, 0..inf (1 = без изменений)
};

inline Bcs toBcs(const Settings& s) {
    Bcs o;
    o.brightness = s.v[P_BRIGHTNESS] * 40.0f;          // -40 .. +40
    o.contrast = std::pow(2.5f, s.v[P_CONTRAST]);      // 0.4 .. 2.5
    const float sat = s.v[P_SATURATION];
    o.saturation = sat < 0.0f ? 1.0f + sat : 1.0f + 2.0f * sat;  // 0 .. 3
    return o;
}

// ---------------------------------------------------------------------------------------
// Расширенный режим: 3D-LUT (XR_META_passthrough_color_lut)
// Раскладка данных по спецификации Meta: индекс = R + G*N + B*N*N, 3 байта на точку (RGB),
// значения в пространстве sRGB. Для N=32 размер = 98304 байта.
// ---------------------------------------------------------------------------------------
constexpr int kLutRes = 32;
constexpr size_t kLutBytes = (size_t)kLutRes * kLutRes * kLutRes * 3;

inline float srgbDecode(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float srgbEncode(float c) {
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Эффективные множители для расчёта (вынесены для тестов).
struct LutParams {
    float gainR, gainG, gainB;  // экспозиция + баланс белого в линейном свете
    float invGamma;             // степень для кодированного значения
    float contrast;             // множитель вокруг середины 0.5
    float saturation;           // множитель
};

inline LutParams lutParams(const Settings& s) {
    LutParams p;
    const float exposure = std::exp2(1.5f * s.v[P_BRIGHTNESS]);  // +-1.5 EV
    const float t = s.v[P_TEMPERATURE], n = s.v[P_TINT];
    p.gainR = exposure * std::exp2(0.40f * t) * std::exp2(0.15f * n);
    p.gainG = exposure * std::exp2(-0.30f * n);
    p.gainB = exposure * std::exp2(-0.40f * t) * std::exp2(0.15f * n);
    p.invGamma = 1.0f / std::exp2(s.v[P_GAMMA]);  // gamma 0.5 .. 2
    p.contrast = std::pow(2.5f, s.v[P_CONTRAST]);
    const float sat = s.v[P_SATURATION];
    p.saturation = sat < 0.0f ? 1.0f + sat : 1.0f + 2.0f * sat;
    return p;
}

// Заполняет out (kLutBytes байт). Конвейер на каждую точку решётки:
//  sRGB -> линейный свет -> усиление (экспозиция, температура, тинт) -> sRGB -> гамма
//  -> контраст вокруг 0.5 -> насыщенность (относительно яркости) -> ограничение 0..1
inline void buildLut(const Settings& s, uint8_t* out) {
    const int N = kLutRes;
    if (s.isNeutral()) {  // точное тождественное преобразование
        size_t i = 0;
        for (int b = 0; b < N; ++b)
            for (int g = 0; g < N; ++g)
                for (int r = 0; r < N; ++r) {
                    out[i++] = (uint8_t)(r * 255 / (N - 1));
                    out[i++] = (uint8_t)(g * 255 / (N - 1));
                    out[i++] = (uint8_t)(b * 255 / (N - 1));
                }
        return;
    }
    const LutParams p = lutParams(s);

    // Таблица "линейный -> sRGB с гаммой", 2048 шагов + линейная интерполяция.
    constexpr int T = 2048;
    std::vector<float> enc(T + 1);
    for (int i = 0; i <= T; ++i) {
        float e = srgbEncode((float)i / T);
        enc[i] = (p.invGamma == 1.0f) ? e : std::pow(e, p.invGamma);
    }
    auto encode = [&](float lin) -> float {
        lin = clamp01(lin) * T;
        int i = (int)lin;
        if (i >= T) return enc[T];
        const float f = lin - (float)i;
        return enc[i] + (enc[i + 1] - enc[i]) * f;
    };

    float lin[kLutRes];
    for (int i = 0; i < N; ++i) lin[i] = srgbDecode((float)i / (N - 1));

    size_t idx = 0;
    for (int b = 0; b < N; ++b) {
        for (int g = 0; g < N; ++g) {
            for (int r = 0; r < N; ++r) {
                float R = encode(lin[r] * p.gainR);
                float G = encode(lin[g] * p.gainG);
                float B = encode(lin[b] * p.gainB);
                R = (R - 0.5f) * p.contrast + 0.5f;
                G = (G - 0.5f) * p.contrast + 0.5f;
                B = (B - 0.5f) * p.contrast + 0.5f;
                const float l = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                R = l + (R - l) * p.saturation;
                G = l + (G - l) * p.saturation;
                B = l + (B - l) * p.saturation;
                out[idx++] = (uint8_t)(clamp01(R) * 255.0f + 0.5f);
                out[idx++] = (uint8_t)(clamp01(G) * 255.0f + 0.5f);
                out[idx++] = (uint8_t)(clamp01(B) * 255.0f + 0.5f);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// Сохранение настроек между запусками
// ---------------------------------------------------------------------------------------
constexpr uint32_t kMagic = 0x31474351;  // 'QCG1'

inline bool saveSettings(const char* path, const Settings& s) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t magic = kMagic;
    uint8_t adv = s.advanced ? 1 : 0;
    bool ok = std::fwrite(&magic, sizeof(magic), 1, f) == 1 &&
              std::fwrite(s.v, sizeof(float), P_COUNT, f) == (size_t)P_COUNT &&
              std::fwrite(&adv, 1, 1, f) == 1;
    std::fclose(f);
    return ok;
}

inline bool loadSettings(const char* path, Settings& s) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    Settings t;
    uint8_t adv = 1;
    bool ok = std::fread(&magic, sizeof(magic), 1, f) == 1 && magic == kMagic &&
              std::fread(t.v, sizeof(float), P_COUNT, f) == (size_t)P_COUNT &&
              std::fread(&adv, 1, 1, f) == 1;
    std::fclose(f);
    if (!ok) return false;
    t.advanced = adv != 0;
    t.clampAll();
    s = t;
    return true;
}

}  // namespace grade
