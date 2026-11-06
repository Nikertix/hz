#pragma once
// gles_renderer.h - отрисовка списка вершин панели (ui.h) в OpenGL ES 3.0.
// Один шейдер: цвет вершины * покрытие из атласа (R8), вывод с премультиплицированной альфой.

#include <GLES3/gl3.h>
#include <android/asset_manager.h>
#include <android/log.h>

#include <cstddef>
#include <vector>

#include "font_data.h"
#include "ui.h"

#define GFX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "QuestColor", __VA_ARGS__)

namespace gfx {

class Renderer {
public:
    // srgbTarget = true, если цель рендера - sRGB-формат (тогда цвета UI линеаризуются).
    bool init(AAssetManager* mgr, bool srgbTarget) {
        srgb_ = srgbTarget;
        if (!loadAtlas(mgr)) return false;

        static const char* kVs =
            "#version 300 es\n"
            "layout(location=0) in vec2 aPos;\n"
            "layout(location=1) in vec2 aUV;\n"
            "layout(location=2) in vec4 aColor;\n"
            "uniform vec2 uInvSize;\n"
            "out vec2 vUV;\n"
            "out vec4 vColor;\n"
            "void main() {\n"
            "  vUV = aUV; vColor = aColor;\n"
            "  gl_Position = vec4(aPos.x * uInvSize.x - 1.0, 1.0 - aPos.y * uInvSize.y, 0.0, 1.0);\n"
            "}\n";
        static const char* kFs =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec2 vUV;\n"
            "in vec4 vColor;\n"
            "uniform sampler2D uTex;\n"
            "uniform float uLinear;\n"
            "out vec4 oColor;\n"
            "void main() {\n"
            "  float cov = texture(uTex, vUV).r;\n"
            "  vec3 c = vColor.rgb;\n"
            "  if (uLinear > 0.5) c = pow(c, vec3(2.2));\n"
            "  float a = vColor.a * cov;\n"
            "  oColor = vec4(c * a, a);\n"
            "}\n";

        const GLuint vs = compile(GL_VERTEX_SHADER, kVs);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, kFs);
        if (!vs || !fs) return false;
        prog_ = glCreateProgram();
        glAttachShader(prog_, vs);
        glAttachShader(prog_, fs);
        glLinkProgram(prog_);
        GLint ok = 0;
        glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(prog_, sizeof(log), nullptr, log);
            GFX_LOGE("link error: %s", log);
            return false;
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
        uInvSize_ = glGetUniformLocation(prog_, "uInvSize");
        uTex_ = glGetUniformLocation(prog_, "uTex");
        uLinear_ = glGetUniformLocation(prog_, "uLinear");

        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        const GLsizei stride = (GLsizei)sizeof(ui::Vtx);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ui::Vtx, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ui::Vtx, u));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ui::Vtx, r));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return true;
    }

    // Рисует вершины в текущий framebuffer (размер w x h пикселей).
    void draw(const std::vector<ui::Vtx>& verts, int w, int h) {
        if (verts.empty() || !prog_) return;
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // премультиплицированная альфа

        glUseProgram(prog_);
        glUniform2f(uInvSize_, 2.0f / (float)w, 2.0f / (float)h);
        glUniform1f(uLinear_, srgb_ ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glUniform1i(uTex_, 0);

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(ui::Vtx)), verts.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_BLEND);
    }

    void destroy() {
        if (prog_) glDeleteProgram(prog_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (tex_) glDeleteTextures(1, &tex_);
        prog_ = vbo_ = vao_ = tex_ = 0;
    }

private:
    bool loadAtlas(AAssetManager* mgr) {
        AAsset* asset = AAssetManager_open(mgr, "font.r8", AASSET_MODE_STREAMING);
        if (!asset) {
            GFX_LOGE("asset font.r8 not found");
            return false;
        }
        const size_t expected = (size_t)fontdata::kAtlasW * fontdata::kAtlasH;
        std::vector<uint8_t> data(expected);
        size_t got = 0;
        while (got < expected) {
            const int n = AAsset_read(asset, data.data() + got, expected - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        AAsset_close(asset);
        if (got != expected) {
            GFX_LOGE("font.r8: size mismatch (%zu of %zu)", got, expected);
            return false;
        }
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, fontdata::kAtlasW, fontdata::kAtlasH, 0, GL_RED, GL_UNSIGNED_BYTE, data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return true;
    }

    static GLuint compile(GLenum type, const char* src) {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            GFX_LOGE("shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    GLuint prog_ = 0, vao_ = 0, vbo_ = 0, tex_ = 0;
    GLint uInvSize_ = -1, uTex_ = -1, uLinear_ = -1;
    bool srgb_ = false;
};

}  // namespace gfx
