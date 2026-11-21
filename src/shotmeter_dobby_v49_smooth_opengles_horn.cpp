#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <time.h>

#include "dobby.h"
#include "greengiant_ima_b64.h"

#define LOG_TAG "ShotmeterDobbyV49"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define FILES_DIR "/data/user/0/com.t2ksports.nba2k20and/files"
#define DIAG_NAME "shotmeter.dobby-v49-smooth-opengles-horn-diagnostic"

static constexpr uintptr_t kManagerOffset = 0x3c700e0ULL;
static constexpr uintptr_t kSavedViewOffset = 0x90ULL;
static constexpr size_t kViewSize = 0x240U;
static const char kSoname[] = "libnba_clean_opt.so";
static const char kFilename[] = "libnba2k20_clean_opt.so";
static const char kDoDrawSymbol[] = "_ZN22ShotIndicatorComponent6DoDrawEP12AI_NBA_ACTOR";
static const char kHeadSymbol[] = "_Z30PlayerIconUtil_GetHeadLocationP12AI_NBA_ACTORRDv4_f";
static const char kSavedGetterSymbol[] = "_Z32PlayerIconManager3D_GetSavedViewv";
static const char kProjectSymbol[] = "_Z14VCView_ProjectPK6VCVIEWPKDv4_fPS2_";
static const char kComponentSymbol[] = "g_ShotIndicatorComponent";
static const char kSwapSymbol[] = "eglSwapBuffers";
static const char kReleaseEventSymbol[] = "_Z20EVT_UserShotReleasedP12AI_NBA_ACTOR23MVS_SHOT_RELEASE_TIMINGff";

using DoDrawFn = void (*)(void *, void *);
using HeadFn = void (*)(void *, float *);
using ProjectFn = float (*)(const void *, const float *, float *);
using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using TestFn = int (*)(int);
using ReleaseFn = void (*)(void *, int32_t, float, float);

static void *g_game = nullptr;
static const char *g_game_name = "not-found";
static uintptr_t g_game_base = 0;
static uintptr_t g_saved_view = 0;
static volatile uint8_t *g_component = nullptr;
static void *g_do_draw_address = nullptr;
static void *g_head_address = nullptr;
static void *g_saved_getter_address = nullptr;
static void *g_project_address = nullptr;
static void *g_swap_address = nullptr;
static void *g_release_event_address = nullptr;
static DoDrawFn g_original_do_draw = nullptr;
static HeadFn g_get_head = nullptr;
static ProjectFn g_project = nullptr;
static SwapFn g_original_swap = nullptr;
static ReleaseFn g_original_release_event = nullptr;

static int g_self_code = -999, g_self_base = -999;
static int g_self_original = -999, g_self_patched = -999;
static int g_do_draw_code = -999, g_swap_code = -999, g_release_event_code = -999;
static std::atomic<uint32_t> g_attach_calls{0}, g_duplicate_attach_calls{0};
static std::atomic<uint32_t> g_init_state{0};
static std::atomic<uint32_t> g_do_draw_calls{0}, g_projection_calls{0};
static std::atomic<uint32_t> g_projection_valid{0}, g_swap_calls{0};
static std::atomic<uint32_t> g_active_meter_frames{0}, g_draw_calls{0};
static std::atomic<uint32_t> g_release_event_calls{0}, g_perfect_release_events{0}, g_perfect_flash_frames{0};
static std::atomic<int32_t> g_last_release_timing{-1};
static std::atomic<uint64_t> g_perfect_until_ns{0};
static std::atomic<uint64_t> g_result_until_ns{0};
static std::atomic<uint32_t> g_result_sequence{0};
static std::atomic<int32_t> g_result_timing{-1};
static std::atomic<uint32_t> g_result_raw_bits{0};
static std::atomic<uint32_t> g_anchor_valid{0};
static std::atomic<uint32_t> g_anchor_x_bits{0}, g_anchor_y_bits{0};
static std::atomic<uint32_t> g_anchor_z_bits{0}, g_clip_w_bits{0};
static std::atomic<uint32_t> g_meter_bits{0}, g_marker_bits{0};
static std::atomic<int32_t> g_component_state{-1};
static std::atomic<uint32_t> g_tracked_player{0};
static std::atomic<int32_t> g_surface_w{0}, g_surface_h{0};
static std::atomic<int32_t> g_viewport_w{0}, g_viewport_h{0};
static std::atomic<int32_t> g_framebuffer{-1};
static std::atomic<uint32_t> g_sound_play_count{0};
static std::atomic<int32_t> g_sound_init_code{-999};
static std::atomic<int32_t> g_shader_init_code{-999};
static std::atomic<uint64_t> g_last_sound_ns{0};
static pthread_mutex_t g_sound_mutex = PTHREAD_MUTEX_INITIALIZER;

static SLObjectItf g_sl_engine_object = nullptr;
static SLEngineItf g_sl_engine = nullptr;
static SLObjectItf g_sl_output_mix = nullptr;
static SLObjectItf g_sl_player_object = nullptr;
static SLPlayItf g_sl_play = nullptr;
static SLAndroidSimpleBufferQueueItf g_sl_queue = nullptr;
static constexpr uint32_t kGreenGiantSamples = 43247;
static int16_t g_green_giant_pcm[kGreenGiantSamples]{};

static volatile int g_test_bias = 100;
__attribute__((noinline, optnone)) static int test_target(int x) { return x + g_test_bias; }
static TestFn g_test_original = nullptr;
__attribute__((noinline, optnone)) static int test_proxy(int x) {
    return g_test_original ? g_test_original(x) + 7 : -9999;
}
__attribute__((noinline, optnone)) static int invoke_test(int x) {
    TestFn volatile fn = &test_target;
    return fn(x);
}

static void store_float(std::atomic<uint32_t> &dst, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    dst.store(bits, std::memory_order_relaxed);
}
static float load_float(const std::atomic<uint32_t> &src) {
    const uint32_t bits = src.load(std::memory_order_relaxed);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static void *open_game(void) {
    void *h = dlopen(kSoname, RTLD_NOW | RTLD_NOLOAD);
    if (h) { g_game_name = kSoname; return h; }
    h = dlopen(kFilename, RTLD_NOW | RTLD_NOLOAD);
    if (h) g_game_name = kFilename;
    return h;
}
static void *resolve(void *handle, const char *name) {
    if (!handle) return nullptr;
    dlerror();
    void *p = dlsym(handle, name);
    const char *e = dlerror();
    if (e) { LOGE("dlsym %s failed: %s", name, e); return nullptr; }
    LOGI("resolved %s at %p", name, p);
    return p;
}
static bool derive_saved_view(void) {
    if (!g_saved_getter_address) return false;
    Dl_info info{};
    if (dladdr(g_saved_getter_address, &info) == 0 || !info.dli_fbase) return false;
    g_game_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    g_saved_view = g_game_base + kManagerOffset + kSavedViewOffset;
    return true;
}

static void capture_anchor(void *actor) {
    if (!actor || !g_get_head || !g_project || !g_saved_view) return;
    float head[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_get_head(actor, head);
    alignas(16) uint8_t view[kViewSize] = {};
    memcpy(view, reinterpret_cast<const void *>(g_saved_view), sizeof(view));
    float screen[4] = {NAN, NAN, NAN, NAN};
    const float reciprocal_clip_w = g_project(view, head, screen);
    g_projection_calls.fetch_add(1, std::memory_order_relaxed);
    store_float(g_anchor_x_bits, screen[0]);
    store_float(g_anchor_y_bits, screen[1]);
    store_float(g_anchor_z_bits, screen[2]);
    store_float(g_clip_w_bits, reciprocal_clip_w);
    const bool valid = std::isfinite(screen[0]) && std::isfinite(screen[1]) &&
        std::isfinite(screen[2]) && std::isfinite(reciprocal_clip_w) &&
        reciprocal_clip_w > 1.0e-8f && screen[2] >= 0.0f && screen[2] <= 1.0f;
    if (valid) {
        g_projection_valid.fetch_add(1, std::memory_order_relaxed);
        g_anchor_valid.store(1, std::memory_order_release);
    } else {
        g_anchor_valid.store(0, std::memory_order_release);
    }
}
__attribute__((noinline))
static void do_draw_proxy(void *component, void *actor) {
    g_do_draw_calls.fetch_add(1, std::memory_order_relaxed);
    capture_anchor(actor);
    const DoDrawFn original = g_original_do_draw;
    if (original) original(component, actor);
}

static constexpr float kHornAngBot = 1.1135f;
static constexpr float kHornAngTop = -0.9931f;
static constexpr float kHornWinLo = 0.900f;
static constexpr float kProgressScale = 4.0f; // v47 observation: raw 0.25 at a class-3 release.

static const char kHornVS[] = R"(
attribute vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

static const char kHornFS[] = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec2 uOrigin;
uniform vec2 uDirFill, uDirCap;
uniform vec3 uTint, uLabelTint;
uniform float uScale, uFill, uFlash, uAlpha;
uniform vec2 uLabelSize;
uniform sampler2D uLabelTex;

const vec2 OUT_C = vec2(50.0, 143.0); const float OUT_R = 136.0;
const vec2 IN_C = vec2(-3.0, 170.0); const float IN_R = 147.0;
const vec2 CUT_P = vec2(98.0, 67.0);
const vec2 CUT_N = vec2(-0.823, -0.57);
float W;
float cov(float x) { return 1.0 - smoothstep(-W, W, x); }
float dOuter(vec2 p) { return length(p - OUT_C) - OUT_R; }
float dInner(vec2 p) { return IN_R - length(p - IN_C); }
float dCut(vec2 p) { return dot(p - CUT_P, CUT_N); }
float horn(vec2 p) { return max(max(dOuter(p), dInner(p)), dCut(p)); }
float roundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - (b - vec2(r));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    W = 0.8 / uScale;
    vec2 p = (gl_FragCoord.xy - uOrigin) / uScale;
    p.y = 319.0 - p.y;

    float labelW = uLabelSize.x + 8.0;
    float labelLeft = 247.0 - labelW - 5.0;
    vec2 labelCenter = vec2(labelLeft + labelW * 0.5, 17.5);
    float labelD = roundBox(p - labelCenter, vec2(labelW * 0.5, 7.5), 2.0);
    float chip = cov(labelD) * step(0.5, uLabelSize.x);
    float d = horn(p);
    if (d > 20.0 && chip < 0.01) { gl_FragColor = vec4(0.0); return; }

    float a = cov(d);
    float body = cov(d + 3.0);
    float sh = 0.0;
#if HORN_SHADOW
    sh = (1.0 - smoothstep(-4.0, 12.0, horn(p - vec2(4.0, 6.0)))) * 0.35;
#endif
    vec3 ink = vec3(0.04);
    vec3 col = mix(ink, mix(vec3(0.20), uTint, uFlash), body);

    vec2 v = p - OUT_C;
    float fm = cov(-(uDirFill.x * v.y - uDirFill.y * v.x));
    col = mix(col, mix(vec3(1.0), uTint, uFlash), fm * body * step(0.001, uFill));

    float cap = cov(uDirCap.x * v.y - uDirCap.y * v.x) * body;
    col = mix(col, mix(vec3(0.25, 0.90, 0.20), uTint, uFlash), cap);

    float baseA = (a + sh * (1.0 - a)) * uAlpha;
    vec3 rgb = col * baseA;
    float alpha = baseA;

    float border = max(0.0, chip - cov(labelD + 1.25) * step(0.5, uLabelSize.x));
    vec3 bg = vec3(0.014, 0.018, 0.022);
    rgb = mix(rgb, bg * chip, chip);
    alpha = alpha + chip * 0.96 * (1.0 - alpha);
    rgb = mix(rgb, uLabelTint * alpha, border);

    vec2 tp = p - vec2(labelLeft + 4.0, 14.0);
    float inText = step(0.0, tp.x) * step(tp.x, uLabelSize.x) *
                   step(0.0, tp.y) * step(tp.y, 7.0);
    vec2 tuv = vec2(tp.x / max(1.0, uLabelSize.x), tp.y / 8.0);
    float textA = texture2D(uLabelTex, tuv).a * inText * step(0.5, uLabelSize.x);
    rgb = mix(rgb, uLabelTint * alpha, textA);
    alpha = alpha + textA * (1.0 - alpha);

    gl_FragColor = vec4(rgb, alpha);
}
)";

static struct {
    EGLContext ctx = EGL_NO_CONTEXT;
    GLuint prog = 0, vbo = 0, vao = 0, labelTex = 0;
    GLint uOrigin = -1, uScale = -1, uFill = -1, uFlash = -1, uAlpha = -1;
    GLint uDirFill = -1, uDirCap = -1, uTint = -1, uLabelTint = -1;
    GLint uLabelSize = -1, uLabelTex = -1;
    bool ok = false, es3 = false;
    int labelTiming = -99;
    float labelWidth = 0.0f;
} G;

static GLuint compile_horn_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        LOGE("horn shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool init_horn_gl(void) {
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    G.es3 = version && strstr(version, "OpenGL ES 3") != nullptr;

    GLint oldProgram = 0, oldBuffer = 0, oldVao = 0, oldActive = GL_TEXTURE0, oldTexture = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    if (G.es3) glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);

    GLuint vs = compile_horn_shader(GL_VERTEX_SHADER, kHornVS);
    const char *fragmentSources[] = {"#define HORN_SHADOW 0\\n", kHornFS};
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (fs) {
        glShaderSource(fs, 2, fragmentSources, nullptr);
        glCompileShader(fs);
        GLint compiled = GL_FALSE;
        glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            char log[1024] = {}; GLsizei len = 0;
            glGetShaderInfoLog(fs, sizeof(log), &len, log);
            LOGE("horn fragment compile failed: %s", log);
            glDeleteShader(fs); fs = 0;
        }
    }
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActive);
        glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
        glUseProgram(oldProgram);
        return false;
    }

    G.prog = glCreateProgram();
    glAttachShader(G.prog, vs);
    glAttachShader(G.prog, fs);
    glBindAttribLocation(G.prog, 0, "aPos");
    glLinkProgram(G.prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(G.prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {}; GLsizei len = 0;
        glGetProgramInfoLog(G.prog, sizeof(log), &len, log);
        LOGE("horn program link failed: %s", log);
        glDeleteProgram(G.prog); G.prog = 0;
        glBindTexture(GL_TEXTURE_2D, oldTexture);
        glActiveTexture(oldActive);
        glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
        glUseProgram(oldProgram);
        return false;
    }

    G.uOrigin = glGetUniformLocation(G.prog, "uOrigin");
    G.uScale = glGetUniformLocation(G.prog, "uScale");
    G.uFill = glGetUniformLocation(G.prog, "uFill");
    G.uFlash = glGetUniformLocation(G.prog, "uFlash");
    G.uAlpha = glGetUniformLocation(G.prog, "uAlpha");
    G.uDirFill = glGetUniformLocation(G.prog, "uDirFill");
    G.uDirCap = glGetUniformLocation(G.prog, "uDirCap");
    G.uTint = glGetUniformLocation(G.prog, "uTint");
    G.uLabelTint = glGetUniformLocation(G.prog, "uLabelTint");
    G.uLabelSize = glGetUniformLocation(G.prog, "uLabelSize");
    G.uLabelTex = glGetUniformLocation(G.prog, "uLabelTex");

    static const float quad[8] = {-1.0f,-1.0f, 1.0f,-1.0f, -1.0f,1.0f, 1.0f,1.0f};
    glGenBuffers(1, &G.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    if (G.es3) {
        glGenVertexArrays(1, &G.vao);
        glBindVertexArray(G.vao);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(oldVao);
    }

    glGenTextures(1, &G.labelTex);
    glBindTexture(GL_TEXTURE_2D, G.labelTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    static const uint8_t blank[42 * 8 * 4] = {};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 42, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, blank);

    glUseProgram(G.prog);
    const float capAngle = kHornAngBot + (kHornAngTop - kHornAngBot) * kHornWinLo;
    glUniform2f(G.uDirCap, cosf(capAngle), sinf(capAngle));
    glUniform1f(G.uAlpha, 1.0f);
    glUniform1i(G.uLabelTex, 1);

    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldActive);
    glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
    glUseProgram(oldProgram);
    G.ok = true;
    return true;
}

static int glyph_index(char c) {
    switch (c) {
        case 'P': return 0; case 'E': return 1; case 'R': return 2;
        case 'F': return 3; case 'C': return 4; case 'T': return 5;
        case 'A': return 6; case 'L': return 7; case 'Y': return 8;
        default: return -1;
    }
}

static void update_horn_label(int timing) {
    GLint previousActive = GL_TEXTURE0, previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    static const uint8_t glyphs[9][7] = {
        {30,17,17,30,16,16,16}, // P
        {31,16,16,30,16,16,31}, // E
        {30,17,17,30,20,18,17}, // R
        {31,16,16,30,16,16,16}, // F
        {14,17,16,16,16,17,14}, // C
        {31,4,4,4,4,4,4},       // T
        {14,17,17,31,17,17,17}, // A
        {16,16,16,16,16,16,31}, // L
        {17,17,10,4,4,4,4}      // Y
    };
    const char *label = "";
    float tint[3] = {1.0f, 1.0f, 1.0f};
    if (timing == 3) { label = "PERFECT"; tint[0]=0.21f; tint[1]=0.82f; tint[2]=0.23f; }
    else if (timing == 2) { label = "EARLY"; tint[0]=1.0f; tint[1]=0.60f; tint[2]=0.10f; }
    else if (timing == 4) { label = "LATE"; tint[0]=1.0f; tint[1]=0.60f; tint[2]=0.10f; }
    else if (timing == 1) { label = "EARLY"; tint[0]=0.90f; tint[1]=0.15f; tint[2]=0.15f; }
    else if (timing == 5) { label = "LATE"; tint[0]=0.90f; tint[1]=0.15f; tint[2]=0.15f; }
    const size_t chars = strlen(label);
    uint8_t pixels[42 * 8 * 4] = {};
    for (size_t x=0; x<chars; ++x) {
        const int gi = glyph_index(label[x]);
        if (gi < 0) continue;
        for (int y=0; y<7; ++y) {
            for (int col=0; col<5; ++col) {
                if ((glyphs[gi][y] & (1u << (4-col))) == 0) continue;
                const size_t at = (static_cast<size_t>(y) * 42 + x*6 + col) * 4;
                pixels[at+0]=255; pixels[at+1]=255; pixels[at+2]=255; pixels[at+3]=255;
            }
        }
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, G.labelTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 42, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    G.labelWidth = static_cast<float>(chars * 6);
    G.labelTiming = timing;
    glBindTexture(GL_TEXTURE_2D, previousTexture);
    glActiveTexture(previousActive);
}

static void draw_horn(int surfaceW, int surfaceH, int anchorX, int anchorY,
                      float progress, bool perfect, int resultTiming) {
    (void)surfaceW;
    if (!G.ok) return;
    if (G.labelTiming != resultTiming) update_horn_label(resultTiming);

    GLint oldProgram=0, oldViewport[4]={0,0,0,0}, oldBuffer=0, oldVao=0;
    GLint srcRgb=GL_ONE, dstRgb=GL_ZERO, srcA=GL_ONE, dstA=GL_ZERO;
    GLint eqRgb=GL_FUNC_ADD, eqA=GL_FUNC_ADD, oldActive=GL_TEXTURE0, oldTexture=0;
    GLint a0Enabled=0, a0Size=4, a0Type=GL_FLOAT, a0Normalized=0, a0Stride=0, a0Buffer=0;
    void *a0Pointer=nullptr;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb); glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA); glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqA);
    GLboolean wasBlend=glIsEnabled(GL_BLEND), wasDepth=glIsEnabled(GL_DEPTH_TEST);
    GLboolean wasScissor=glIsEnabled(GL_SCISSOR_TEST), wasCull=glIsEnabled(GL_CULL_FACE);
    GLboolean wasStencil=glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    if (G.es3) glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    else {
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a0Enabled);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0Size);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0Type);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0Normalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0Stride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0Buffer);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0Pointer);
    }

    const float scale = (160.0f * std::max(0.72f, std::min(1.35f, surfaceH / 720.0f))) / 319.0f;
    const float bx = anchorX + 43.0f*scale - 123.5f*scale;
    const float topY = anchorY + 48.0f*scale;
    const float by = topY - 319.0f*scale;
    const GLint vx = static_cast<GLint>(floorf(bx + 60.0f*scale));
    const GLint vy = static_cast<GLint>(floorf(by + 19.0f*scale));
    const GLsizei vw = static_cast<GLsizei>(ceilf(187.0f*scale)+1.0f);
    const GLsizei vh = static_cast<GLsizei>(ceilf(300.0f*scale)+1.0f);

    if (wasDepth) glDisable(GL_DEPTH_TEST);
    if (wasScissor) glDisable(GL_SCISSOR_TEST);
    if (wasCull) glDisable(GL_CULL_FACE);
    if (wasStencil) glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(vx, vy, vw, vh);
    glUseProgram(G.prog);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, G.labelTex);
    glUniform1i(G.uLabelTex, 1);
    glUniform2f(G.uOrigin, bx, by);
    glUniform1f(G.uScale, scale);
    glUniform1f(G.uFill, progress);
    glUniform1f(G.uFlash, perfect ? 1.0f : 0.0f);
    glUniform2f(G.uDirFill,
        cosf(kHornAngBot + (kHornAngTop-kHornAngBot)*progress),
        sinf(kHornAngBot + (kHornAngTop-kHornAngBot)*progress));
    glUniform3f(G.uTint, 0.21f, 0.82f, 0.23f);
    if (resultTiming == 3) glUniform3f(G.uLabelTint, 0.21f, 0.82f, 0.23f);
    else if (resultTiming == 2 || resultTiming == 4) glUniform3f(G.uLabelTint, 1.0f, 0.60f, 0.10f);
    else glUniform3f(G.uLabelTint, 0.90f, 0.15f, 0.15f);
    glUniform2f(G.uLabelSize, G.labelWidth, G.labelWidth > 0.0f ? 7.0f : 0.0f);

    if (G.es3) glBindVertexArray(G.vao);
    else {
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (G.es3) glBindVertexArray(oldVao);
    else {
        glBindBuffer(GL_ARRAY_BUFFER, a0Buffer);
        glVertexAttribPointer(0, a0Size, a0Type, a0Normalized, a0Stride, a0Pointer);
        if (!a0Enabled) glDisableVertexAttribArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, oldBuffer);
    glBindTexture(GL_TEXTURE_2D, oldTexture);
    glActiveTexture(oldActive);
    glUseProgram(oldProgram);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glBlendEquationSeparate(eqRgb, eqA);
    glBlendFuncSeparate(srcRgb, dstRgb, srcA, dstA);
    if (!wasBlend) glDisable(GL_BLEND);
    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (wasCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (wasStencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}


static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static size_t decode_base64(const char *src, uint8_t *dst, size_t cap) {
    uint32_t acc=0; int bits=-8; size_t out=0;
    for (const char *p=src; *p && *p!='='; ++p) {
        const int v=b64_value(*p); if (v<0) continue;
        acc=(acc<<6)|static_cast<uint32_t>(v); bits+=6;
        if (bits>=0) {
            if (out>=cap) return 0;
            dst[out++]=static_cast<uint8_t>((acc>>bits)&0xffu);
            bits-=8;
        }
    }
    return out;
}
static bool decode_green_giant_ima(int16_t *out, size_t expectedSamples) {
    static uint8_t packed[22400];
    const size_t bytes=decode_base64(kGreenGiantImaB64, packed, sizeof(packed));
    if (bytes < 16 || memcmp(packed, "GGI1", 4)!=0) return false;
    const uint32_t samples=static_cast<uint32_t>(packed[4]) |
        (static_cast<uint32_t>(packed[5])<<8) |
        (static_cast<uint32_t>(packed[6])<<16) |
        (static_cast<uint32_t>(packed[7])<<24);
    const uint32_t blockSize=static_cast<uint32_t>(packed[8]) |
        (static_cast<uint32_t>(packed[9])<<8) |
        (static_cast<uint32_t>(packed[10])<<16) |
        (static_cast<uint32_t>(packed[11])<<24);
    const uint32_t payloadBytes=static_cast<uint32_t>(packed[12]) |
        (static_cast<uint32_t>(packed[13])<<8) |
        (static_cast<uint32_t>(packed[14])<<16) |
        (static_cast<uint32_t>(packed[15])<<24);
    if (samples!=expectedSamples || blockSize!=256 || payloadBytes+16!=bytes) return false;
    static const int stepTable[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
    static const int indexTable[16]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
    size_t pos=16, written=0;
    while (written<samples) {
        const size_t blockSamples=std::min<size_t>(blockSize, samples-written);
        if (pos+4>bytes) return false;
        int predictor=static_cast<int16_t>(static_cast<uint16_t>(packed[pos]) |
            (static_cast<uint16_t>(packed[pos+1])<<8));
        int index=std::min(88, static_cast<int>(packed[pos+2]));
        const size_t nibbleBytes=(blockSamples-1+1)/2;
        if (pos+4+nibbleBytes>bytes) return false;
        out[written++]=static_cast<int16_t>(predictor);
        for (size_t i=0;i<blockSamples-1;++i) {
            const uint8_t byte=packed[pos+4+i/2];
            const int code=(i&1u) ? (byte>>4) : (byte&15);
            const int step=stepTable[index];
            int delta=step>>3;
            if (code&1) delta+=step>>2;
            if (code&2) delta+=step>>1;
            if (code&4) delta+=step;
            predictor=(code&8) ? predictor-delta : predictor+delta;
            predictor=std::max(-32768,std::min(32767,predictor));
            index=std::max(0,std::min(88,index+indexTable[code]));
            out[written++]=static_cast<int16_t>(predictor);
        }
        pos+=4+nibbleBytes;
    }
    return written==samples && pos==bytes;
}

static bool initialize_sound(void) {
    SLresult result = slCreateEngine(&g_sl_engine_object, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_engine_object)->Realize(g_sl_engine_object, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_engine_object)->GetInterface(g_sl_engine_object, SL_IID_ENGINE, &g_sl_engine);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_engine)->CreateOutputMix(g_sl_engine, &g_sl_output_mix, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_output_mix)->Realize(g_sl_output_mix, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }

    SLDataLocator_AndroidSimpleBufferQueue queue_locator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2
    };
    SLDataFormat_PCM pcm_format = {
        SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_44_1,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource source = {&queue_locator, &pcm_format};
    SLDataLocator_OutputMix output_locator = {
        SL_DATALOCATOR_OUTPUTMIX, g_sl_output_mix
    };
    SLDataSink sink = {&output_locator, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};
    result = (*g_sl_engine)->CreateAudioPlayer(
        g_sl_engine, &g_sl_player_object, &source, &sink, 1, ids, required);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_player_object)->Realize(g_sl_player_object, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_player_object)->GetInterface(g_sl_player_object, SL_IID_PLAY, &g_sl_play);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }
    result = (*g_sl_player_object)->GetInterface(
        g_sl_player_object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_sl_queue);
    if (result != SL_RESULT_SUCCESS) { g_sound_init_code.store(static_cast<int32_t>(result)); return false; }

    if (!decode_green_giant_ima(g_green_giant_pcm, kGreenGiantSamples)) {
        g_sound_init_code.store(-2001); return false;
    }
    g_sound_init_code.store(0);
    return true;
}

static void play_perfect_sound(void) {
    if (!g_sl_play || !g_sl_queue) return;
    const uint64_t now = monotonic_ns();
    pthread_mutex_lock(&g_sound_mutex);
    const uint64_t previous = g_last_sound_ns.load(std::memory_order_relaxed);
    if (previous == 0 || now < previous || now - previous >= 450000000ULL) {
        const SLresult clear_result = (*g_sl_queue)->Clear(g_sl_queue);
        const SLresult enqueue_result = clear_result == SL_RESULT_SUCCESS
            ? (*g_sl_queue)->Enqueue(g_sl_queue, g_green_giant_pcm, sizeof(g_green_giant_pcm))
            : clear_result;
        if (enqueue_result == SL_RESULT_SUCCESS &&
            (*g_sl_play)->SetPlayState(g_sl_play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS) {
            g_last_sound_ns.store(now, std::memory_order_relaxed);
            g_sound_play_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    pthread_mutex_unlock(&g_sound_mutex);
}

__attribute__((noinline))
static void release_event_proxy(void *actor, int32_t timing, float arg0, float arg1) {
    (void)arg0;
    (void)arg1;
    const ReleaseFn original = g_original_release_event;
    if (original) original(actor, timing, arg0, arg1);
    g_release_event_calls.fetch_add(1, std::memory_order_relaxed);
    g_last_release_timing.store(timing, std::memory_order_relaxed);
    /* The release event gets the HUR classifier's class directly; class 3 is
       the standard jumpshot's stock Excellent/special-feedback class. */
    if (!g_component || !actor || timing < 1 || timing > 5) return;
    const uintptr_t tracked = *reinterpret_cast<volatile uintptr_t *>(g_component + 0x08);
    if (tracked == 0 || reinterpret_cast<uintptr_t>(actor) != tracked) return;
    const float raw = *reinterpret_cast<volatile float *>(g_component + 0x10);
    const float finalProgress = std::isfinite(raw)
        ? std::max(0.0f, std::min(1.0f, raw * kProgressScale)) : 0.0f;
    store_float(g_result_raw_bits, timing == 3 ? 1.0f : finalProgress);
    g_result_timing.store(timing, std::memory_order_release);
    g_result_until_ns.store(monotonic_ns() + 1000000000ULL, std::memory_order_release);
    g_result_sequence.fetch_add(1, std::memory_order_acq_rel);
    if (timing == 3) {
        g_perfect_release_events.fetch_add(1, std::memory_order_relaxed);
        g_perfect_until_ns.store(monotonic_ns() + 650000000ULL, std::memory_order_release);
        play_perfect_sound();
    }
}

static void draw_overlay(EGLDisplay display, EGLSurface surface) {
    if (!g_component || g_anchor_valid.load(std::memory_order_acquire) == 0) return;
    const uintptr_t tracked = *reinterpret_cast<volatile uintptr_t *>(g_component + 0x08);
    const float raw = *reinterpret_cast<volatile float *>(g_component + 0x10);
    const int32_t state = *reinterpret_cast<volatile int32_t *>(g_component + 0x18);
    g_component_state.store(state, std::memory_order_relaxed);
    g_tracked_player.store(tracked ? 1U : 0U, std::memory_order_relaxed);
    store_float(g_meter_bits, raw);

    const uint64_t now = monotonic_ns();
    const bool resultActive = tracked != 0 &&
        now <= g_result_until_ns.load(std::memory_order_acquire);
    const int32_t resultTiming = resultActive ? g_result_timing.load(std::memory_order_acquire) : -1;
    const bool perfect = resultActive && resultTiming == 3 &&
        now <= g_perfect_until_ns.load(std::memory_order_acquire);
    const bool meterActive = tracked && state == 2 && std::isfinite(raw);
    if (!tracked || (!meterActive && !resultActive)) return;

    float progress = resultActive
        ? load_float(g_result_raw_bits)
        : (std::isfinite(raw) ? std::max(0.0f, std::min(1.0f, raw * kProgressScale)) : 0.0f);
    if (resultActive && resultTiming == 3) progress = 1.0f;
    store_float(g_marker_bits, progress);
    if (meterActive) g_active_meter_frames.fetch_add(1, std::memory_order_relaxed);
    if (perfect) g_perfect_flash_frames.fetch_add(1, std::memory_order_relaxed);

    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    EGLint sw=0, sh=0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &sw) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &sh) != EGL_TRUE || sw <= 0 || sh <= 0) return;
    GLint vp[4]={0,0,0,0}, fbo=-1;
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    g_surface_w.store(sw); g_surface_h.store(sh);
    g_viewport_w.store(vp[2]); g_viewport_h.store(vp[3]); g_framebuffer.store(fbo);
    if (fbo != 0 || vp[2] <= 0 || vp[3] <= 0) return;

    if (G.ctx != eglGetCurrentContext()) {
        G = {};
        G.ctx = eglGetCurrentContext();
        G.ok = init_horn_gl();
        g_shader_init_code.store(G.ok ? 0 : -1);
    }
    if (!G.ok) return;

    const float sx = static_cast<float>(vp[2]) / static_cast<float>(sw);
    const float sy = static_cast<float>(vp[3]) / static_cast<float>(sh);
    const float anchor_x = load_float(g_anchor_x_bits);
    const float anchor_y = load_float(g_anchor_y_bits);
    const GLint ax = vp[0] + static_cast<GLint>(std::lround(anchor_x * sx));
    const GLint top_y = static_cast<GLint>(std::lround(anchor_y * sy));
    const GLint ay = vp[1] + vp[3] - top_y;
    draw_horn(vp[2], vp[3], ax, ay, progress, perfect, resultTiming);
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
}

__attribute__((noinline))
static EGLBoolean swap_proxy(EGLDisplay display, EGLSurface surface) {
    g_swap_calls.fetch_add(1, std::memory_order_relaxed);
    draw_overlay(display, surface);
    return g_original_swap ? g_original_swap(display, surface) : EGL_FALSE;
}

static void write_diagnostic(void) {
    char path[256];
    const int n = snprintf(path, sizeof(path), "%s/%s", FILES_DIR, DIAG_NAME);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) return;
    const bool self_ok = g_self_code == 0 && g_self_base == 105 &&
        g_self_original == 105 && g_self_patched == 112;
    const char *status = "HOOK_REGISTRATION_FAILED";
    if (!self_ok) status = "DOBBY_SELF_TEST_FAILED";
    else if (!g_game || !g_component) status = "GAME_COMPONENT_NOT_FOUND";
    else if (g_do_draw_code != 0 || !g_original_do_draw)
        status = "PLAYER_ANCHOR_HOOK_FAILED";
    else if (g_swap_code != 0 || !g_original_swap)
        status = "EGL_HOOK_FAILED";
    else if (g_release_event_code != 0 || !g_original_release_event)
        status = "RELEASE_EVENT_HOOK_FAILED";
    else if (g_draw_calls.load()) status = "HORN_SHADER_DRAWN";
    else if (g_active_meter_frames.load()) status = "METER_ACTIVE_WAITING_FOR_RENDER";
    else if (g_anchor_valid.load()) status = "PLAYER_ANCHOR_READY_WAITING_FOR_SHOT";
    else if (g_swap_calls.load()) status = "WAITING_FOR_VALID_PLAYER_PROJECTION";
    else status = "HOOKS_READY_WAITING_FOR_FRAMES";

    const bool perfect_active = monotonic_ns() <= g_perfect_until_ns.load();
    char body[3072];
    (void)snprintf(body, sizeof(body),
        "agent=v49\npurpose=player_anchored_smooth_opengles_horn\nengine=Dobby\n"
        "engine_commit=5dfc8546954ce3b3198132ab13fddb89ee92cdd7\n"
        "target_library=%s\ncomponent_symbol=%s\ncomponent_address=%p\n"
        "do_draw_address=%p\ndo_draw_hook_code=%d\nhead_location_address=%p\n"
        "saved_view_address=%p\nproject_address=%p\negl_swap_buffers_address=%p\n"
        "egl_swap_buffers_hook_code=%d\nrelease_event_address=%p\nrelease_event_hook_code=%d\n"
        "self_test_status=%s\nself_test_hook_code=%d\nself_test_baseline=%d\n"
        "self_test_original=%d\nself_test_patched=%d\nattach_calls=%u\n"
        "duplicate_attach_calls=%u\ninitialization_state=%u\ndo_draw_callbacks=%u\n"
        "projection_calls=%u\nprojection_valid=%u\nlast_anchor_x=%.8g\n"
        "last_anchor_y=%.8g\nlast_anchor_z=%.8g\nlast_reciprocal_clip_w=%.8g\n"
        "anchor_valid=%u\negl_swap_callbacks=%u\nactive_meter_frames=%u\n"
        "overlay_draw_calls=%u\nlast_component_state=%d\nlast_tracked_player_present=%u\n"
        "last_meter_value=%.7g\\nlast_meter_position=%.7g\\nprogress_mapping_scale=4\\n"
        "shader_init_code=%d\\nresult_timing_enum=%d\\nresult_sequence=%u\\n"
        "release_event_callbacks=%u\\nlast_release_timing_class=%d\\nperfect_class3_release_events=%u\\n"
        "perfect_flash_active=%u\\nperfect_flash_frames=%u\\nsound_init_code=%d\\n"
        "perfect_sound_play_count=%u\\nsurface_width=%d\\nsurface_height=%d\\n"
        "viewport_width=%d\\nviewport_height=%d\\nframebuffer_binding=%d\\n"
        "saved_view_copy_only=1\\ngame_owned_view_bytes_written=0\\n"
        "game_object_fields_modified=0\\ncomponent_memory_modified=0\\n"
        "original_meter_resource_modified=0\\nshot_timing_modified=0\\n"
        "original_library_file_modified=0\\nstatus=%s\\n",
        g_game_name, kComponentSymbol, (const void *)g_component,
        g_do_draw_address, g_do_draw_code, g_head_address, (void *)g_saved_view,
        g_project_address, g_swap_address, g_swap_code, g_release_event_address,
        g_release_event_code, self_ok ? "PASS" : "FAIL", g_self_code, g_self_base,
        g_self_original, g_self_patched, g_attach_calls.load(),
        g_duplicate_attach_calls.load(), g_init_state.load(), g_do_draw_calls.load(),
        g_projection_calls.load(), g_projection_valid.load(),
        load_float(g_anchor_x_bits), load_float(g_anchor_y_bits),
        load_float(g_anchor_z_bits), load_float(g_clip_w_bits), g_anchor_valid.load(),
        g_swap_calls.load(), g_active_meter_frames.load(), g_draw_calls.load(),
        g_component_state.load(), g_tracked_player.load(), load_float(g_meter_bits),
        load_float(g_marker_bits), g_shader_init_code.load(), g_result_timing.load(),
        g_result_sequence.load(), g_release_event_calls.load(),
        g_last_release_timing.load(), g_perfect_release_events.load(),
        perfect_active ? 1U : 0U, g_perfect_flash_frames.load(),
        g_sound_init_code.load(), g_sound_play_count.load(),
        g_surface_w.load(), g_surface_h.load(), g_viewport_w.load(),
        g_viewport_h.load(), g_framebuffer.load(), status);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { LOGE("diagnostic open failed errno=%d", errno); return; }
    const size_t len = strlen(body);
    const ssize_t wrote = write(fd, body, len);
    if (wrote < 0 || static_cast<size_t>(wrote) != len)
        LOGE("diagnostic write incomplete %ld/%lu", (long)wrote, (unsigned long)len);
    close(fd);
}

static void *reporter(void *) {
    for (;;) { write_diagnostic(); sleep(1); }
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM *vm, char *, void *) {
    if (!vm) return JNI_ERR;
    g_attach_calls.fetch_add(1, std::memory_order_relaxed);
    uint32_t expected = 0;
    if (!g_init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        g_duplicate_attach_calls.fetch_add(1, std::memory_order_relaxed);
        return JNI_OK;
    }
    g_self_base = invoke_test(5);
    void *test_original = nullptr;
    g_self_code = DobbyHook((void *)&test_target, (void *)&test_proxy, &test_original);
    g_test_original = reinterpret_cast<TestFn>(test_original);
    g_self_original = g_test_original ? g_test_original(5) : -1;
    g_self_patched = invoke_test(5);
    const bool self_ok = g_self_code == 0 && g_self_base == 105 &&
        g_self_original == 105 && g_self_patched == 112;
    if (!self_ok) {
        g_init_state.store(3, std::memory_order_release);
        write_diagnostic();
        return JNI_OK;
    }

    g_game = open_game();
    if (g_game) {
        g_component = reinterpret_cast<volatile uint8_t *>(resolve(g_game, kComponentSymbol));
        g_do_draw_address = resolve(g_game, kDoDrawSymbol);
        g_head_address = resolve(g_game, kHeadSymbol);
        g_saved_getter_address = resolve(g_game, kSavedGetterSymbol);
        g_project_address = resolve(g_game, kProjectSymbol);
        g_swap_address = resolve(g_game, kSwapSymbol);
        g_release_event_address = resolve(g_game, kReleaseEventSymbol);
        g_get_head = reinterpret_cast<HeadFn>(g_head_address);
        g_project = reinterpret_cast<ProjectFn>(g_project_address);
        const bool view_ok = derive_saved_view();
        if (g_do_draw_address && g_get_head && g_project && view_ok) {
            void *original = nullptr;
            g_do_draw_code = DobbyHook(g_do_draw_address, (void *)&do_draw_proxy, &original);
            g_original_do_draw = reinterpret_cast<DoDrawFn>(original);
        } else g_do_draw_code = -1002;
        if (g_swap_address) {
            void *original = nullptr;
            g_swap_code = DobbyHook(g_swap_address, (void *)&swap_proxy, &original);
            g_original_swap = reinterpret_cast<SwapFn>(original);
        } else g_swap_code = -1003;
        if (g_release_event_address) {
            void *original_release = nullptr;
            g_release_event_code = DobbyHook(g_release_event_address,
                (void *)&release_event_proxy, &original_release);
            g_original_release_event = reinterpret_cast<ReleaseFn>(original_release);
        } else g_release_event_code = -1004;
    } else {
        g_do_draw_code = -1001;
        g_swap_code = -1001;
    }

    const bool sound_ready = initialize_sound();
    LOGI("v49 Green Giant audio init=%s code=%d", sound_ready ? "ready" : "unavailable", g_sound_init_code.load());
    g_init_state.store(2, std::memory_order_release);
    write_diagnostic();
    pthread_t thread;
    if (pthread_create(&thread, nullptr, reporter, nullptr) == 0) pthread_detach(thread);
    else LOGE("diagnostic reporter thread creation failed");
    LOGI("v49 smooth OpenGL ES horn meter attached; class-3 player release triggers green flash and optional tone");
    return JNI_OK;
}
extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *) {
    LOGI("v49 smooth horn meter unloading");
}
