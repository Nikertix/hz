// main.cpp - Цветокоррекция passthrough (AR/MR) для Meta Quest 3 / 3S.
// OpenXR + NativeActivity + OpenGL ES 3. Видео с камер показывает системный слой passthrough
// (XR_FB_passthrough); цветокоррекцию применяет сам компоновщик Quest - с минимальной задержкой.
//  - Базовый режим:   XrPassthroughBrightnessContrastSaturationFB (яркость/контраст/насыщенность)
//  - Расширенный:     3D-LUT (XR_META_passthrough_color_lut) - + гамма, температура, тинт

#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "gles_renderer.h"
#include "grading.h"
#include "ui.h"

#define LOG_TAG "QuestColor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Вызов OpenXR внутри метода App, возвращающего bool: при ошибке запоминает текст и выходит.
#define XR_TRY(expr)                    \
    do {                                \
        const XrResult r_ = (expr);     \
        if (XR_FAILED(r_)) {            \
            setError(#expr, r_);        \
            return false;               \
        }                               \
    } while (0)

// ------------------------- Указатели на функции расширений -----------------------------
static PFN_xrGetOpenGLESGraphicsRequirementsKHR fn_xrGetOpenGLESGraphicsRequirementsKHR = nullptr;
static PFN_xrCreatePassthroughFB fn_xrCreatePassthroughFB = nullptr;
static PFN_xrDestroyPassthroughFB fn_xrDestroyPassthroughFB = nullptr;
static PFN_xrPassthroughStartFB fn_xrPassthroughStartFB = nullptr;
static PFN_xrCreatePassthroughLayerFB fn_xrCreatePassthroughLayerFB = nullptr;
static PFN_xrDestroyPassthroughLayerFB fn_xrDestroyPassthroughLayerFB = nullptr;
static PFN_xrPassthroughLayerSetStyleFB fn_xrPassthroughLayerSetStyleFB = nullptr;
static PFN_xrCreatePassthroughColorLutMETA fn_xrCreatePassthroughColorLutMETA = nullptr;
static PFN_xrDestroyPassthroughColorLutMETA fn_xrDestroyPassthroughColorLutMETA = nullptr;
static PFN_xrUpdatePassthroughColorLutMETA fn_xrUpdatePassthroughColorLutMETA = nullptr;

template <typename T>
static bool loadFn(XrInstance instance, const char* name, T& out) {
    return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out))) &&
           out != nullptr;
}

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------- Параметры панели --------------------------------------
static constexpr float kPanelWidthM = 0.90f;                                   // ширина панели, м
static constexpr float kPanelHeightM = kPanelWidthM * ui::kPanelH / ui::kPanelW;
static constexpr float kPanelDistM = 1.0f;                                     // расстояние до панели
static constexpr float kPanelDropM = 0.06f;                                    // ниже уровня глаз

class App {
public:
    explicit App(android_app* a) : android_(a) {}

    bool resumed = false;
    bool sessionRunning() const { return sessionRunning_; }

    // ------------------------------------ Инициализация ---------------------------------
    bool init() {
        const char* dataPath = android_->activity->internalDataPath;
        if (dataPath && dataPath[0]) settingsPath_ = std::string(dataPath) + "/settings.bin";
        if (!settingsPath_.empty() && grade::loadSettings(settingsPath_.c_str(), settings_)) LOGI("настройки загружены");

        if (!createInstance()) return false;
        if (!createSession()) return false;
        if (!createSwapchain()) return false;
        if (!createInput()) return false;
        paramsDirty_ = styleDirty_ = true;
        placePending_ = true;
        return true;
    }

    void shutdown() {
        saveNow();
        if (lut_ != XR_NULL_HANDLE && fn_xrDestroyPassthroughColorLutMETA) fn_xrDestroyPassthroughColorLutMETA(lut_);
        if (ptLayer_ != XR_NULL_HANDLE && fn_xrDestroyPassthroughLayerFB) fn_xrDestroyPassthroughLayerFB(ptLayer_);
        if (passthrough_ != XR_NULL_HANDLE && fn_xrDestroyPassthroughFB) fn_xrDestroyPassthroughFB(passthrough_);
        lut_ = XR_NULL_HANDLE;
        ptLayer_ = XR_NULL_HANDLE;
        passthrough_ = XR_NULL_HANDLE;
        renderer_.destroy();
        if (fbo_) glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
        if (panelSwapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(panelSwapchain_);
        if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
        if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
        if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
        if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
        if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
        panelSwapchain_ = XR_NULL_HANDLE;
        actionSet_ = XR_NULL_HANDLE;
        viewSpace_ = localSpace_ = XR_NULL_HANDLE;
        session_ = XR_NULL_HANDLE;
        instance_ = XR_NULL_HANDLE;
        if (eglDisplay_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface_ != EGL_NO_SURFACE) eglDestroySurface(eglDisplay_, eglSurface_);
            if (eglContext_ != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay_, eglContext_);
            eglTerminate(eglDisplay_);
        }
        eglDisplay_ = EGL_NO_DISPLAY;
    }

    void saveNow() {
        if (saveAt_ > 0.0 && !settingsPath_.empty()) {
            grade::saveSettings(settingsPath_.c_str(), settings_);
            saveAt_ = 0.0;
        }
    }

    // -------------------------------------- События -------------------------------------
    void pollEvents() {
        if (instance_ == XR_NULL_HANDLE) return;
        for (;;) {
            XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
            if (xrPollEvent(instance_, &ev) != XR_SUCCESS) break;
            switch (ev.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    const auto* e = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
                    sessionState_ = e->state;
                    LOGI("состояние сессии: %d", (int)e->state);
                    switch (e->state) {
                        case XR_SESSION_STATE_READY: {
                            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                            if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) {
                                sessionRunning_ = true;
                                if (passthrough_ == XR_NULL_HANDLE) {
                                    if (!createPassthrough()) LOGE("passthrough: %s", errorBuf_);
                                }
                            }
                            break;
                        }
                        case XR_SESSION_STATE_STOPPING:
                            xrEndSession(session_);
                            sessionRunning_ = false;
                            saveNow();
                            break;
                        case XR_SESSION_STATE_EXITING:
                        case XR_SESSION_STATE_LOSS_PENDING:
                            sessionRunning_ = false;
                            ANativeActivity_finish(android_->activity);
                            break;
                        default: break;
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    sessionRunning_ = false;
                    ANativeActivity_finish(android_->activity);
                    break;
                default: break;
            }
        }
    }

    // -------------------------------------- Кадр ----------------------------------------
    void frame() {
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session_, &wi, &fs))) return;
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(session_, &bi))) return;

        const double t = nowSec();
        const float dt = lastFrameTime_ > 0.0 ? (float)std::min(0.1, t - lastFrameTime_) : 0.0f;
        lastFrameTime_ = t;

        if (sessionState_ == XR_SESSION_STATE_FOCUSED) pollInput(dt);
        if (placePending_ && fs.predictedDisplayTime > 0) placePanel(fs.predictedDisplayTime);
        applyGrading();
        if (saveAt_ > 0.0 && t >= saveAt_) saveNow();

        XrCompositionLayerPassthroughFB ptLayerInfo{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        std::vector<const XrCompositionLayerBaseHeader*> layers;

        if (fs.shouldRender) {
            // 1) Слой passthrough - фон. Должен идти ПЕРЕД остальными слоями.
            if (ptLayer_ != XR_NULL_HANDLE) {
                ptLayerInfo.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                ptLayerInfo.space = XR_NULL_HANDLE;
                ptLayerInfo.layerHandle = ptLayer_;
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ptLayerInfo));
            }
            // 2) Панель управления поверх
            if (panelVisible_ && panelPlaced_) {
                if (renderPanel()) {
                    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    quad.space = localSpace_;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = panelSwapchain_;
                    quad.subImage.imageRect.offset = {0, 0};
                    quad.subImage.imageRect.extent = {ui::kPanelW, ui::kPanelH};
                    quad.subImage.imageArrayIndex = 0;
                    quad.pose = panelPose_;
                    quad.size = {kPanelWidthM, kPanelHeightM};
                    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
                }
            }
        }

        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;  // для Quest всегда OPAQUE
        ei.layerCount = (uint32_t)layers.size();
        ei.layers = layers.data();
        xrEndFrame(session_, &ei);
    }

private:
    // ============================ Создание объектов OpenXR ================================
    void setError(const char* expr, XrResult r) {
        char name[64];
        size_t n = 0;
        for (; expr[n] && expr[n] != '(' && n < sizeof(name) - 1; ++n) name[n] = expr[n];
        name[n] = 0;
        const char* nm = std::strncmp(name, "fn_", 3) == 0 ? name + 3 : name;
        std::snprintf(errorBuf_, sizeof(errorBuf_), "%s: ошибка %d", nm, (int)r);
        LOGE("%s", errorBuf_);
    }
    void setErrorText(const char* msg) {
        std::snprintf(errorBuf_, sizeof(errorBuf_), "%s", msg);
        LOGE("%s", errorBuf_);
    }

    bool createInstance() {
        // Загрузчик OpenXR на Android нужно инициализировать до любых других вызовов.
        PFN_xrInitializeLoaderKHR initLoader = nullptr;
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initLoader));
        if (initLoader) {
            XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            li.applicationVM = android_->activity->vm;
            li.applicationContext = android_->activity->clazz;
            XR_TRY(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)));
        }

        uint32_t extCount = 0;
        XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr));
        std::vector<XrExtensionProperties> props(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
        XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props.data()));
        auto has = [&](const char* name) {
            for (const auto& p : props)
                if (std::strcmp(p.extensionName, name) == 0) return true;
            return false;
        };

        std::vector<const char*> exts;
        const char* required[] = {"XR_KHR_opengl_es_enable", "XR_KHR_android_create_instance", "XR_FB_passthrough"};
        for (const char* r : required) {
            if (!has(r)) {
                std::snprintf(errorBuf_, sizeof(errorBuf_), "Нет расширения %s (нужна Quest с passthrough)", r);
                LOGE("%s", errorBuf_);
                return false;
            }
            exts.push_back(r);
        }
        if (has("XR_FB_triangle_mesh")) exts.push_back("XR_FB_triangle_mesh");
        if (has("XR_META_passthrough_color_lut")) {
            exts.push_back("XR_META_passthrough_color_lut");
            lutExt_ = true;
        }

        XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        androidInfo.applicationVM = android_->activity->vm;
        androidInfo.applicationActivity = android_->activity->clazz;

        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        ci.next = &androidInfo;
        std::strncpy(ci.applicationInfo.applicationName, "QuestColorGrade", XR_MAX_APPLICATION_NAME_SIZE - 1);
        ci.applicationInfo.applicationVersion = 1;
        std::strncpy(ci.applicationInfo.engineName, "native", XR_MAX_ENGINE_NAME_SIZE - 1);
        ci.applicationInfo.engineVersion = 1;
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = (uint32_t)exts.size();
        ci.enabledExtensionNames = exts.data();
        XR_TRY(xrCreateInstance(&ci, &instance_));

        loadFn(instance_, "xrGetOpenGLESGraphicsRequirementsKHR", fn_xrGetOpenGLESGraphicsRequirementsKHR);
        bool ok = loadFn(instance_, "xrCreatePassthroughFB", fn_xrCreatePassthroughFB);
        ok &= loadFn(instance_, "xrDestroyPassthroughFB", fn_xrDestroyPassthroughFB);
        ok &= loadFn(instance_, "xrPassthroughStartFB", fn_xrPassthroughStartFB);
        ok &= loadFn(instance_, "xrCreatePassthroughLayerFB", fn_xrCreatePassthroughLayerFB);
        ok &= loadFn(instance_, "xrDestroyPassthroughLayerFB", fn_xrDestroyPassthroughLayerFB);
        ok &= loadFn(instance_, "xrPassthroughLayerSetStyleFB", fn_xrPassthroughLayerSetStyleFB);
        if (!ok || !fn_xrGetOpenGLESGraphicsRequirementsKHR) {
            setErrorText("Не удалось загрузить функции OpenXR passthrough");
            return false;
        }
        if (lutExt_) {
            bool l = loadFn(instance_, "xrCreatePassthroughColorLutMETA", fn_xrCreatePassthroughColorLutMETA);
            l &= loadFn(instance_, "xrDestroyPassthroughColorLutMETA", fn_xrDestroyPassthroughColorLutMETA);
            l &= loadFn(instance_, "xrUpdatePassthroughColorLutMETA", fn_xrUpdatePassthroughColorLutMETA);
            if (!l) lutExt_ = false;
        }

        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XR_TRY(xrGetSystem(instance_, &sgi, &systemId_));

        // Свойства системы: поддержка passthrough и максимальное разрешение LUT
        XrSystemPassthroughProperties2FB pt2{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
        XrSystemPassthroughColorLutPropertiesMETA lutProps{XR_TYPE_SYSTEM_PASSTHROUGH_COLOR_LUT_PROPERTIES_META};
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        sp.next = &pt2;
        pt2.next = lutExt_ ? static_cast<void*>(&lutProps) : nullptr;
        if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &sp))) {
            LOGI("система: %s, passthrough capabilities=0x%x, LUT max=%u", sp.systemName,
                 (unsigned)pt2.capabilities, (unsigned)lutProps.maxColorLutResolution);
            if (lutExt_ && lutProps.maxColorLutResolution != 0 &&
                lutProps.maxColorLutResolution < (uint32_t)grade::kLutRes)
                lutExt_ = false;
        }
        return true;
    }

    bool createEgl() {
        eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay_ == EGL_NO_DISPLAY || !eglInitialize(eglDisplay_, nullptr, nullptr)) {
            setErrorText("EGL: не удалось инициализировать дисплей");
            return false;
        }
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif
        const EGLint cfgAttribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                     EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                     EGL_NONE};
        EGLint numCfg = 0;
        if (!eglChooseConfig(eglDisplay_, cfgAttribs, &eglConfig_, 1, &numCfg) || numCfg < 1) {
            setErrorText("EGL: нет подходящей конфигурации (нужен OpenGL ES 3)");
            return false;
        }
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, ctxAttribs);
        if (eglContext_ == EGL_NO_CONTEXT) {
            setErrorText("EGL: не удалось создать контекст ES 3");
            return false;
        }
        const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbAttribs);
        if (eglSurface_ == EGL_NO_SURFACE || !eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
            setErrorText("EGL: не удалось активировать контекст");
            return false;
        }
        return true;
    }

    bool createSession() {
        XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        XR_TRY(fn_xrGetOpenGLESGraphicsRequirementsKHR(instance_, systemId_, &req));
        if (!createEgl()) return false;

        XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        gb.display = eglDisplay_;
        gb.config = eglConfig_;
        gb.context = eglContext_;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &gb;
        sci.systemId = systemId_;
        XR_TRY(xrCreateSession(instance_, &sci, &session_));

        XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsi.poseInReferenceSpace = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
        rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XR_TRY(xrCreateReferenceSpace(session_, &rsi, &localSpace_));
        rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        XR_TRY(xrCreateReferenceSpace(session_, &rsi, &viewSpace_));
        return true;
    }

    bool createSwapchain() {
        uint32_t n = 0;
        XR_TRY(xrEnumerateSwapchainFormats(session_, 0, &n, nullptr));
        std::vector<int64_t> formats(n);
        XR_TRY(xrEnumerateSwapchainFormats(session_, n, &n, formats.data()));
        auto has = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
        int64_t chosen = formats.empty() ? 0 : formats[0];
        bool srgb = false;
        if (has(GL_RGBA8)) {
            chosen = GL_RGBA8;
        } else if (has(GL_SRGB8_ALPHA8)) {
            chosen = GL_SRGB8_ALPHA8;
            srgb = true;
        }

        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc.format = chosen;
        sc.sampleCount = 1;
        sc.width = ui::kPanelW;
        sc.height = ui::kPanelH;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XR_TRY(xrCreateSwapchain(session_, &sc, &panelSwapchain_));

        uint32_t imgCount = 0;
        XR_TRY(xrEnumerateSwapchainImages(panelSwapchain_, 0, &imgCount, nullptr));
        panelImages_.assign(imgCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        XR_TRY(xrEnumerateSwapchainImages(panelSwapchain_, imgCount, &imgCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(panelImages_.data())));

        glGenFramebuffers(1, &fbo_);
        if (!renderer_.init(android_->activity->assetManager, srgb)) {
            setErrorText("Не удалось инициализировать отрисовку панели");
            return false;
        }
        return true;
    }

    // Passthrough создаётся, когда сессия уже запущена (READY -> xrBeginSession).
    bool createPassthrough() {
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        XR_TRY(fn_xrCreatePassthroughFB(session_, &pci, &passthrough_));

        XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lci.passthrough = passthrough_;
        lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
        XR_TRY(fn_xrCreatePassthroughLayerFB(session_, &lci, &ptLayer_));
        if (fn_xrPassthroughStartFB) fn_xrPassthroughStartFB(passthrough_);  // на случай, если ещё не запущен

        if (lutExt_) {
            lutBuf_.assign(grade::kLutBytes, 0);
            grade::buildLut(settings_, lutBuf_.data());
            XrPassthroughColorLutCreateInfoMETA lc{XR_TYPE_PASSTHROUGH_COLOR_LUT_CREATE_INFO_META};
            lc.channels = XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META;
            lc.resolution = (uint32_t)grade::kLutRes;
            lc.data.bufferSize = (uint32_t)lutBuf_.size();
            lc.data.buffer = lutBuf_.data();
            const XrResult r = fn_xrCreatePassthroughColorLutMETA(passthrough_, &lc, &lut_);
            if (XR_FAILED(r)) {
                LOGW("LUT недоступна (код %d), работаем в базовом режиме", (int)r);
                lut_ = XR_NULL_HANDLE;
                lutExt_ = false;
            }
        }
        paramsDirty_ = styleDirty_ = true;
        LOGI("passthrough запущен, LUT: %s", lut_ != XR_NULL_HANDLE ? "да" : "нет");
        return true;
    }

    // ==================================== Ввод ===========================================
    bool createInput() {
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(asci.actionSetName, "main", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(asci.localizedActionSetName, "Main", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        XR_TRY(xrCreateActionSet(instance_, &asci, &actionSet_));

        auto make = [&](XrActionType type, const char* name, XrAction& out) -> bool {
            XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
            ai.actionType = type;
            std::strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
            std::strncpy(ai.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
            return XR_SUCCEEDED(xrCreateAction(actionSet_, &ai, &out));
        };
        bool ok = make(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick_left", stickL_);
        ok &= make(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick_right", stickR_);
        ok &= make(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_left", trigL_);
        ok &= make(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_right", trigR_);
        ok &= make(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", btnA_);
        ok &= make(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", btnB_);
        ok &= make(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", btnX_);
        ok &= make(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", btnY_);
        ok &= make(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_left", clickL_);
        ok &= make(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click_right", clickR_);
        if (!ok) {
            setErrorText("Не удалось создать действия ввода");
            return false;
        }

        auto path = [&](const char* s) {
            XrPath p = XR_NULL_PATH;
            xrStringToPath(instance_, s, &p);
            return p;
        };
        const XrActionSuggestedBinding bindings[] = {
            {stickL_, path("/user/hand/left/input/thumbstick")},
            {stickR_, path("/user/hand/right/input/thumbstick")},
            {trigL_, path("/user/hand/left/input/trigger/value")},
            {trigR_, path("/user/hand/right/input/trigger/value")},
            {btnA_, path("/user/hand/right/input/a/click")},
            {btnB_, path("/user/hand/right/input/b/click")},
            {btnX_, path("/user/hand/left/input/x/click")},
            {btnY_, path("/user/hand/left/input/y/click")},
            {clickL_, path("/user/hand/left/input/thumbstick/click")},
            {clickR_, path("/user/hand/right/input/thumbstick/click")},
        };
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        sb.suggestedBindings = bindings;
        sb.countSuggestedBindings = (uint32_t)(sizeof(bindings) / sizeof(bindings[0]));
        XR_TRY(xrSuggestInteractionProfileBindings(instance_, &sb));

        XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        ai.countActionSets = 1;
        ai.actionSets = &actionSet_;
        XR_TRY(xrAttachSessionActionSets(session_, &ai));
        return true;
    }

    bool getVec2(XrAction a, float& x, float& y) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &st)) && st.isActive) {
            x = st.currentState.x;
            y = st.currentState.y;
            return true;
        }
        x = y = 0.0f;
        return false;
    }
    float getFloat(XrAction a) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &gi, &st)) && st.isActive) return st.currentState;
        return 0.0f;
    }
    bool wasPressed(XrAction a) {  // событие "нажата" (фронт)
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &st)) && st.isActive && st.changedSinceLastSync &&
               st.currentState;
    }

    int maxSelectable() const { return lutActive() ? grade::P_COUNT - 1 : grade::kBasicParams - 1; }
    bool lutActive() const { return settings_.advanced && lut_ != XR_NULL_HANDLE; }

    void markChanged() {
        paramsDirty_ = true;
        saveAt_ = nowSec() + 1.0;  // сохраняем через секунду после последнего изменения
    }

    void pollInput(float dt) {
        XrActiveActionSet aas{actionSet_, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &aas;
        if (XR_FAILED(xrSyncActions(session_, &si))) return;
        selected_ = std::min(selected_, maxSelectable());

        // --- стики: берём тот, что отклонён сильнее ---
        float lx, ly, rx, ry;
        getVec2(stickL_, lx, ly);
        getVec2(stickR_, rx, ry);
        const bool useLeft = (lx * lx + ly * ly) >= (rx * rx + ry * ry);
        const float sx = useLeft ? lx : rx, sy = useLeft ? ly : ry;
        const float ax = std::fabs(sx), ay = std::fabs(sy);
        const float trig = std::max(getFloat(trigL_), getFloat(trigR_));

        // Вверх/вниз - выбор параметра (по одному шагу на отклонение)
        if (ay > 0.6f && ay > ax * 1.3f) {
            if (!navLatched_) {
                navLatched_ = true;
                selected_ = std::min(std::max(selected_ + (sy > 0.0f ? -1 : 1), 0), maxSelectable());
            }
        } else if (ay < 0.35f) {
            navLatched_ = false;
        }
        // Влево/вправо - значение (триггер = точная подстройка)
        constexpr float kDead = 0.18f;
        if (ax > kDead && ax >= ay) {
            const float mag = (ax - kDead) / (1.0f - kDead);
            const float speed = 0.7f * mag * (1.0f - 0.88f * trig);  // полный диапазон ~3 c
            float& v = settings_.v[selected_];
            const float nv = std::min(1.0f, std::max(-1.0f, v + (sx > 0.0f ? 1.0f : -1.0f) * speed * dt));
            if (nv != v) {
                v = nv;
                markChanged();
            }
        }

        // --- кнопки ---
        if (wasPressed(btnA_)) {  // сброс выбранного параметра
            settings_.v[selected_] = 0.0f;
            markChanged();
        }
        if (wasPressed(btnY_)) {  // сброс всех параметров
            for (float& v : settings_.v) v = 0.0f;
            markChanged();
        }
        if (wasPressed(btnX_)) {  // оригинал / коррекция
            bypass_ = !bypass_;
            styleDirty_ = true;
        }
        if (wasPressed(btnB_)) {  // показать / скрыть панель
            panelVisible_ = !panelVisible_;
            if (panelVisible_) placePending_ = true;
        }
        if (wasPressed(clickL_) && lutExt_ && lut_ != XR_NULL_HANDLE) {  // базовый <-> расширенный
            settings_.advanced = !settings_.advanced;
            selected_ = std::min(selected_, maxSelectable());
            paramsDirty_ = styleDirty_ = true;
            saveAt_ = nowSec() + 1.0;
        }
        if (wasPressed(clickR_)) {  // вернуть панель перед собой
            panelVisible_ = true;
            placePending_ = true;
        }
    }

    // ================================ Применение цветокоррекции ==========================
    void applyStyleNow() {
        if (ptLayer_ == XR_NULL_HANDLE) return;
        XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
        style.textureOpacityFactor = 1.0f;
        style.edgeColor = XrColor4f{0.0f, 0.0f, 0.0f, 0.0f};

        XrPassthroughBrightnessContrastSaturationFB bcs{XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB};
        XrPassthroughColorMapLutMETA lutMap{XR_TYPE_PASSTHROUGH_COLOR_MAP_LUT_META};

        if (lutActive()) {
            lutMap.colorLut = lut_;
            lutMap.weight = bypass_ ? 0.0f : 1.0f;  // weight = 0 - исходное изображение
            style.next = &lutMap;
        } else if (!bypass_) {
            const grade::Bcs b = grade::toBcs(settings_);
            bcs.brightness = b.brightness;
            bcs.contrast = b.contrast;
            bcs.saturation = b.saturation;
            style.next = &bcs;
        } else {
            style.next = nullptr;
        }
        const XrResult r = fn_xrPassthroughLayerSetStyleFB(ptLayer_, &style);
        if (XR_FAILED(r)) LOGW("xrPassthroughLayerSetStyleFB: %d", (int)r);
    }

    void applyGrading() {
        if (ptLayer_ == XR_NULL_HANDLE) return;
        if (!paramsDirty_ && !styleDirty_) return;
        const bool useLut = lutActive();
        if (paramsDirty_ && useLut) {
            grade::buildLut(settings_, lutBuf_.data());
            XrPassthroughColorLutUpdateInfoMETA up{XR_TYPE_PASSTHROUGH_COLOR_LUT_UPDATE_INFO_META};
            up.data.bufferSize = (uint32_t)lutBuf_.size();
            up.data.buffer = lutBuf_.data();
            const XrResult r = fn_xrUpdatePassthroughColorLutMETA(lut_, &up);
            if (XR_FAILED(r)) LOGW("xrUpdatePassthroughColorLutMETA: %d", (int)r);
        }
        if (styleDirty_ || (paramsDirty_ && !useLut)) applyStyleNow();
        paramsDirty_ = styleDirty_ = false;
    }

    // ===================================== Панель ========================================
    void placePanel(XrTime time) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(viewSpace_, localSpace_, time, &loc))) return;
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((loc.locationFlags & need) != need) return;
        const XrQuaternionf& q = loc.pose.orientation;
        // Горизонтальное направление взгляда (рыскание), без наклона головы
        const float yaw = std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        const float fx = -std::sin(yaw), fz = -std::cos(yaw);
        panelPose_.position.x = loc.pose.position.x + fx * kPanelDistM;
        panelPose_.position.y = loc.pose.position.y - kPanelDropM;
        panelPose_.position.z = loc.pose.position.z + fz * kPanelDistM;
        panelPose_.orientation = XrQuaternionf{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
        panelPlaced_ = true;
        placePending_ = false;
    }

    bool renderPanel() {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(panelSwapchain_, &ai, &index))) return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(panelSwapchain_, &wi))) {
            XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(panelSwapchain_, &rel);  // не оставляем изображение "захваченным"
            return false;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, panelImages_[index].image, 0);
        if (!fboChecked_) {
            fboChecked_ = true;
            const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) LOGE("framebuffer не готов: 0x%x", (unsigned)st);
        }
        glViewport(0, 0, ui::kPanelW, ui::kPanelH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ui::View view;
        view.s = &settings_;
        view.selected = std::min(selected_, maxSelectable());
        view.lutSupported = lutExt_ && lut_ != XR_NULL_HANDLE;
        view.lutActive = lutActive();
        view.bypass = bypass_;
        view.error = errorBuf_[0] ? errorBuf_ : nullptr;
        ui::buildPanel(batch_, view);
        renderer_.draw(batch_.v, ui::kPanelW, ui::kPanelH);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        return XR_SUCCEEDED(xrReleaseSwapchainImage(panelSwapchain_, &ri));
    }

    // ===================================== Данные ========================================
    android_app* android_;
    // OpenXR
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    // Passthrough
    XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB ptLayer_ = XR_NULL_HANDLE;
    XrPassthroughColorLutMETA lut_ = XR_NULL_HANDLE;
    bool lutExt_ = false;
    std::vector<uint8_t> lutBuf_;
    // EGL / панель
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    XrSwapchain panelSwapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> panelImages_;
    GLuint fbo_ = 0;
    bool fboChecked_ = false;
    gfx::Renderer renderer_;
    ui::Batch batch_;
    XrPosef panelPose_ = XrPosef{{0, 0, 0, 1}, {0, 0, -kPanelDistM}};
    bool panelVisible_ = true;
    bool panelPlaced_ = false;
    bool placePending_ = true;
    // Ввод
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction stickL_ = XR_NULL_HANDLE, stickR_ = XR_NULL_HANDLE, trigL_ = XR_NULL_HANDLE, trigR_ = XR_NULL_HANDLE;
    XrAction btnA_ = XR_NULL_HANDLE, btnB_ = XR_NULL_HANDLE, btnX_ = XR_NULL_HANDLE, btnY_ = XR_NULL_HANDLE;
    XrAction clickL_ = XR_NULL_HANDLE, clickR_ = XR_NULL_HANDLE;
    bool navLatched_ = false;
    int selected_ = 1;  // при запуске выбран "Контраст"
    // Состояние коррекции
    grade::Settings settings_;
    bool bypass_ = false;
    bool paramsDirty_ = true, styleDirty_ = true;
    std::string settingsPath_;
    double saveAt_ = 0.0;
    double lastFrameTime_ = 0.0;
    char errorBuf_[200] = {0};
};

// ------------------------------- Жизненный цикл Android --------------------------------
static void onAppCmd(android_app* state, int32_t cmd) {
    App* app = static_cast<App*>(state->userData);
    switch (cmd) {
        case APP_CMD_RESUME: app->resumed = true; break;
        case APP_CMD_PAUSE:
            app->resumed = false;
            app->saveNow();
            break;
        default: break;
    }
}

void android_main(android_app* state) {
    JNIEnv* env = nullptr;
    state->activity->vm->AttachCurrentThread(&env, nullptr);

    App app(state);
    state->userData = &app;
    state->onAppCmd = onAppCmd;

    const bool ok = app.init();
    if (!ok) LOGE("инициализация не удалась");
    bool finishing = false;

    while (!state->destroyRequested) {
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            // идёт сессия - не ждём; активность на паузе - спим до события; ждём READY - опрашиваем раз в 50 мс
            const int timeout = app.sessionRunning() ? 0 : (app.resumed ? 50 : -1);
            if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) < 0) break;
            if (source) source->process(state, source);
        }
        if (!ok) {  // не смогли запуститься - закрываем активность (один раз)
            if (!finishing) {
                finishing = true;
                ANativeActivity_finish(state->activity);
            }
            continue;
        }
        app.pollEvents();
        if (app.sessionRunning()) app.frame();
    }

    app.shutdown();
    state->activity->vm->DetachCurrentThread();
}
