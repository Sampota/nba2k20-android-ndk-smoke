/*
 * =====================================================================================
 *  green_flame.cpp : the green flame (overlay module)
 * =====================================================================================
 *
 *  WHAT IT IS
 *  ----------
 *  A wispy, translucent green flame that burns at the top tip of the shot meter (the green
 *  release zone). It ignites with a bright green flash after an EXCELLENT shot, burns, then
 *  fades out.
 *
 *  This file contains ONLY the flame. The horn meter (horn_meter.cpp) and the result banner
 *  (shot_feedback.cpp) are separate modules, and the hook and GL state handling are in
 *  overlay_core.cpp. Like them, this module registers itself with the core when the file is
 *  part of the build, so you can build with or without it.
 *
 *  WHAT IT LOOKS LIKE
 *  ------------------
 *   - Wispy smoke-like fire: soft, see-through green tongues made of thin flowing filaments
 *     that curl as they rise, wide at the bottom, breaking into a thin wispy tip.
 *   - Color: pale green hot core, bright green body, dark green edges, a touch of yellow-green
 *     at the base, and a soft green glow around it.
 *   - Animation like real fire: the filaments flow upward and keep curling, the whole flame
 *     sways and flickers, and a few sparks drift up.
 *   - Hidden until an EXCELLENT shot (kIdle = 0). On an excellent shot it ignites with a
 *     strobing green flash, grows taller and throws sparks, burns for kHotTime seconds, then
 *     fades away smoothly over kFadeOutTime seconds (shrinking and dimming as it goes).
 *   - Other results (early/late) do not change the flame.
 *   Set kIdle above 0 (for example 0.45) to keep a small flame burning all the time.
 *
 *  HOW IT IS DRAWN
 *  ---------------
 *  One fragment shader draws the flame inside a small viewport around itself:
 *     1. a soft envelope (wide at the base, narrowing to the tip) sets where flame can be;
 *     2. coordinates scroll upward over time, and are bent ("domain warped") by two noise
 *        lookups, which is what makes the tongues curl and flow;
 *     3. "ridged" noise (bright thin lines where the noise crosses its middle value) gives
 *        the fine filaments, a softer layer gives the volume, a bright column gives the core;
 *     4. a color ramp and a glow halo finish it.
 *  All noise comes from ONE small texture (256 x 256, 64 KB) that is generated on the CPU when
 *  the library loads. The shader only reads it, so no noise math runs per pixel.
 *
 *  PERFORMANCE / CPU NOTES (octa-core phones)
 *  ------------------------------------------
 *   - Load time: the noise texture is generated in parallel on up to 8 threads (one band of
 *     rows per core). That takes a few milliseconds instead of a long single-core stall. It
 *     happens once, when the first GL context is seen.
 *   - Every frame: nothing runs on the CPU except a few uniform uploads. OpenGL calls must stay
 *     on the game's render thread, so extra cores cannot help per-frame drawing; the point is
 *     that the per-frame work is almost nothing.
 *   - GPU: about 6 texture reads per pixel inside the flame, none outside it. Pixels outside
 *     the flame's envelope skip the noise reads completely. The flame box is small.
 *   - Zero cost when idle: after the fade out finishes, the flame is not drawn at all.
 *   - FLAME_QUALITY: 2 = full detail (default), 1 = one fewer noise layer (faster, a little
 *     less fine detail). Lower kFlameSizeFrac for an even cheaper flame.
 *
 *  USING IT FROM YOUR GAME
 *  -----------------------
 *      Shot_Release(progress);          // core API: an excellent result ignites the flame
 *      Shot_ReleaseResult(HORN_*);      // same, when your game already knows the result
 *      GreenFlame_SetEnabled(false);    // hide the flame
 *
 *  SETTINGS (below)
 *  ----------------
 *  kFlameSizeFrac     flame size: side of its square drawing box, relative to screen height
 *  kAnchorX/Y         where the flame base sits, in the meter's design box (247 x 319)
 *  kOffsetX/Y         extra offset, in flame heights (X right, Y up)
 *  kIdle              idle flame size 0..1 (0 = hidden until an excellent shot)
 *  kFlashTime         seconds of the bright flash
 *  kHotTime           seconds the flame burns at full size after an excellent shot
 *  kFadeOutTime       seconds of the fade out that follows
 */

#include <math.h>
#include <thread>
#include <vector>
#include "overlay_core.h"
#include "horn_common.h"

#define FLAME_QUALITY 2   // 2 = full detail, 1 = faster (one noise layer fewer)

// --- settings ---
static const float kFlameSizeFrac = 0.12f;     // side of the flame's square box, relative to screen height
static const float kAnchorX = 118.0f;          // flame base X in the meter design box
static const float kAnchorY = 40.0f;           // flame base Y in the meter design box (y down)
static const float kOffsetX = 0.0f;            // extra offset, in flame heights, right (+)
static const float kOffsetY = 0.0f;            // extra offset, in flame heights, up (+)
static const float kIdle        = 0.0f;        // idle size 0..1 (0 = hidden until excellent)
static const float kFlashTime   = 0.6f;        // bright flash duration, seconds
static const float kHotTime     = 1.5f;        // full-size burn after an excellent shot, seconds
static const float kFadeOutTime = 0.9f;        // fade out after that, seconds

// =====================================================================================
//  NOISE TEXTURE (generated on the CPU, in parallel)
// =====================================================================================
// A 256 x 256 tileable noise image. It tiles, so the shader can scroll through it forever
// (the texture wraps) without visible seams.

static const int N = 256;
static unsigned char g_noise[N * N];
static bool g_baked = false;

static unsigned hash2(unsigned x, unsigned y, unsigned seed) {
    unsigned h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Periodic Perlin (gradient) noise: x, y in [0,1), g = grid cells per tile. About -0.7 .. 0.7.
static float perlin(float x, float y, int g, unsigned seed) {
    float fx = x * g, fy = y * g;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float tx = fx - ix, ty = fy - iy;
    auto grad = [&](int cx, int cy, float dx, float dy) {
        unsigned h = hash2((unsigned)(cx % g), (unsigned)(cy % g), seed);   // wraps, so it tiles
        float a = (float)(h & 0xFFFF) * (6.2831853f / 65536.0f);
        return cosf(a) * dx + sinf(a) * dy;
    };
    float u = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    float v = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
    float n00 = grad(ix,     iy,     tx,        ty);
    float n10 = grad(ix + 1, iy,     tx - 1.0f, ty);
    float n01 = grad(ix,     iy + 1, tx,        ty - 1.0f);
    float n11 = grad(ix + 1, iy + 1, tx - 1.0f, ty - 1.0f);
    float a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
    return a + (b - a) * v;
}

// Fills rows [y0, y1) of the texture: four layers of noise, coarse to fine.
static void genRows(unsigned char* img, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < N; x++) {
            float fx = (float)x / N, fy = (float)y / N;
            float v = 0.50f * perlin(fx, fy, 4, 1) + 0.30f * perlin(fx, fy, 8, 2)
                    + 0.15f * perlin(fx, fy, 16, 3) + 0.08f * perlin(fx, fy, 32, 4);
            v = 0.5f + v * 1.1f;
            img[y * N + x] = (unsigned char)(v < 0.0f ? 0.0f : (v > 1.0f ? 255.0f : v * 255.0f));
        }
    }
}

// Bakes the whole texture, one band of rows per CPU core (up to 8 cores). If a thread cannot
// be started, the calling thread simply does that band itself.
static void bakeNoise() {
    if (g_baked) return;
    unsigned cores = std::thread::hardware_concurrency();
    if (cores < 1) cores = 1;
    if (cores > 8) cores = 8;
    int n = (int)cores;
    std::vector<std::thread> workers;
    int firstUnstarted = n;                      // bands from here on run on this thread
    for (int k = 1; k < n; k++) {
        try {
            workers.emplace_back(genRows, g_noise, k * N / n, (k + 1) * N / n);
        } catch (...) { firstUnstarted = k; break; }
    }
    genRows(g_noise, 0, N / n);                  // band 0 runs here while the others work
    for (int k = firstUnstarted; k < n; k++) genRows(g_noise, k * N / n, (k + 1) * N / n);
    for (auto& t : workers) t.join();
    g_baked = true;
}

// =====================================================================================
//  SHADER
// =====================================================================================
// The flame is drawn in a SQUARE box. Coordinates inside it, in "flame units" (1 unit = box side):
//   p.x : 0 on the flame axis, -0.5 .. 0.5 across the box
//   p.y : 0 at the bottom of the box .. 1 at the top, the flame base is at BASE (0.20)
// The flame and its sparks stay well inside the box, and the glow is faded to zero before it
// reaches any edge, so the edge of the box is never visible.
static const char* FS_FLAME = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uNoise;                       // tileable noise (alpha channel), wraps
uniform vec2  uOrigin;                          // pixels: x of the flame axis, y of the box bottom
uniform float uPx;                              // pixels per flame unit
uniform float uTime, uPower, uFlash, uAlpha;    // clock, size 0..1.35, flash 0..1, fade 0..1

float tx(vec2 p) { return texture2D(uNoise, p).a; }

// only used for the four sparks
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main() {
    const float BASE = 0.20;
    vec2  p = (gl_FragCoord.xy - uOrigin) / uPx;
    float HF = 0.40 + 0.16 * uPower;            // flame height (flame units): wide, not stretched
    float h  = (p.y - BASE) / HF;               // 0 at the base .. 1 at the tip
    float t  = uTime;
    float strobe = uFlash * (0.65 + 0.35 * sin(t * 40.0));   // flashing while the flash lasts
    float flick  = 0.94 + 0.06 * sin(t * 13.0) + 0.04 * sin(t * 23.0 + 1.3);
    float wmax   = 0.20 * (0.85 + 0.15 * uPower) * flick;   // half width at the base

    // soft green glow around the flame. It is faded to exactly zero toward the box edges
    // (win), so the box edge never shows, even during the bright flash.
    float edgeD = min(min(0.5 - abs(p.x), p.y), 1.0 - p.y);   // distance to the nearest box edge
    float win   = smoothstep(0.0, 0.25, edgeD);
    float g = exp(-length(vec2(p.x * 2.0, (p.y - BASE - 0.12) * 1.3)) * 4.2)
              * (0.16 + 0.28 * uPower + 0.9 * strobe) * win;
    vec4 res = vec4(vec3(0.10, 0.90, 0.25) * g, g);

    // the noise reads only happen inside the flame's envelope
    if (abs(p.x) < wmax * 1.9 && h > -0.15 && h < 1.1) {
        // slow sway of the whole flame, growing toward the tip
        float sway = sin(t * 1.7 + h * 3.0) * 0.04 * h + (tx(vec2(t * 0.05, h * 0.7)) - 0.5) * 0.10 * h;
        float x = p.x - sway;

        // envelope: wide at the base, narrowing to a thin wispy tip, soft edges
        float w  = wmax * pow(max(1.0 - h, 0.0), 0.9) * (0.55 + 0.45 * (1.0 - h));
        float m  = 1.0 - smoothstep(0.35, 1.0, abs(x) / max(w, 0.005));
        float vm = smoothstep(-0.08, 0.12, h) * (1.0 - smoothstep(0.7, 1.0, h));

        // coordinates scroll upward (faster during the flash) ...
        vec2 q = vec2(x * 2.6, h * 1.5 - t * 0.5 * (1.0 + 0.35 * uFlash));
        // ... and are bent by two noise lookups, which makes the tongues curl and flow
        vec2 wv = vec2(tx(q * 0.55 + vec2(0.0, -t * 0.18)),
                       tx(q * 0.55 + vec2(0.37, 0.61 - t * 0.21))) - 0.5;
        vec2 qq = q + wv * (0.55 + 1.1 * h);

        // ridged noise: thin bright lines where the noise crosses its middle value = filaments
        float n1 = tx(qq * 1.15);
        float r1 = pow(1.0 - abs(n1 * 2.0 - 1.0), 3.0);
#if FLAME_QUALITY >= 2
        float n2  = tx(qq * 2.3 + vec2(3.1, 1.7));
        float fil = r1 * 0.75 + pow(1.0 - abs(n2 * 2.0 - 1.0), 4.0) * 0.5;
#else
        float fil = r1 * 0.95;
#endif
        float body = tx(qq * 0.8 + vec2(1.3, 4.1));            // soft volume under the filaments
        float d = m * vm * (0.30 * body * (1.0 - 0.5 * h) + 0.95 * fil);

        // bright hot column in the lower middle
        float cx   = x / (wmax * 0.45);
        float core = exp(-cx * cx) * (1.0 - smoothstep(0.05, 0.7, h)) * (0.6 + 0.4 * body) * vm;

        float dens = clamp(d + core * 0.55, 0.0, 1.0);
        float a    = smoothstep(0.04, 0.55, dens);

        // color: dark green edge -> bright green body -> pale green hot parts
        vec3 col = mix(vec3(0.00, 0.42, 0.10), vec3(0.12, 0.88, 0.28), smoothstep(0.05, 0.45, dens));
        col = mix(col, vec3(0.78, 1.00, 0.80), smoothstep(0.45, 1.0, dens) * (0.7 + 0.3 * strobe));
        col = mix(col, vec3(0.60, 0.95, 0.12), 0.35 * (1.0 - smoothstep(0.0, 0.22, h)));   // yellow-green at the base
        col *= 1.0 + 0.8 * strobe;                              // brighter during the flash
        res = vec4(col * a, a) + res * (1.0 - a);
    }

    // four small sparks rising from the flame (only when the flame is big or flashing)
    float spk = clamp(uPower - 0.45, 0.0, 1.0) + uFlash;
    for (int i = 0; i < 4; i++) {
        float fi   = float(i);
        float seed = hash(vec2(fi, 3.7));
        float life = fract(t * (0.45 + 0.25 * seed) + seed * 5.0);   // 0 at birth .. 1 at death
        vec2  sp   = vec2((seed - 0.5) * 0.30 + sin(t * 3.0 + fi * 2.0) * 0.03 * life,
                          BASE + 0.08 + life * (0.30 + 0.25 * uPower));
        float s = (1.0 - smoothstep(0.0, 0.02, length(p - sp))) * (1.0 - life) * spk;
        res += vec4(vec3(0.60, 1.00, 0.60) * s, s);
    }

    res *= smoothstep(0.0, 0.05, edgeD);                    // safety: nothing is ever cut by the box edge
    res.a   = min(res.a, 1.0);
    res.rgb = min(res.rgb, vec3(1.0));
    gl_FragColor = res * uAlpha;                            // premultiplied alpha output
}
)";

// =====================================================================================
//  STATE
// =====================================================================================

static struct {
    GLuint prog = 0, tex = 0;
    GLint uOrigin = -1, uPx = -1, uTime = -1, uPower = -1, uFlash = -1, uAlpha = -1;
} G;

static struct {
    float t = 0;          // flame clock (seconds, wrapped)
    float burst = 0;      // bright flash 1 -> 0 over kFlashTime
    float age = 1e9f;     // seconds since the last excellent shot (huge = never / long ago)
    bool  enabled = true;
} F;

// =====================================================================================
//  PUBLIC API
// =====================================================================================

// Show or hide the flame.
extern "C" void GreenFlame_SetEnabled(bool v) { F.enabled = v; }

// =====================================================================================
//  MODULE CALLBACKS
// =====================================================================================

// Once per GL context: build the shader and upload the noise texture.
static bool flameInit() {
    G.prog = Overlay_MakeProgram(FLAME_QUALITY >= 2 ? "#define FLAME_QUALITY 2\n" : "#define FLAME_QUALITY 1\n", FS_FLAME);
    if (!G.prog) return false;
    G.uOrigin = glGetUniformLocation(G.prog, "uOrigin");
    G.uPx     = glGetUniformLocation(G.prog, "uPx");
    G.uTime   = glGetUniformLocation(G.prog, "uTime");
    G.uPower  = glGetUniformLocation(G.prog, "uPower");
    G.uFlash  = glGetUniformLocation(G.prog, "uFlash");
    G.uAlpha  = glGetUniformLocation(G.prog, "uAlpha");
    glUseProgram(G.prog);                 // the core restores the game's program afterwards
    glUniform1i(glGetUniformLocation(G.prog, "uNoise"), 0);   // noise lives on texture unit 0

    bakeNoise();                          // parallel CPU bake, only the first time

    GLint oldActive, oldTex, oldAlign;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign);
    glGenTextures(1, &G.tex);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, N, N, 0, GL_ALPHA, GL_UNSIGNED_BYTE, g_noise);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);   // the shader scrolls through it
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);
    glBindTexture(GL_TEXTURE_2D, oldTex);
    glActiveTexture(oldActive);
    return true;
}

// An excellent shot ignites the flash and restarts the burn. Other results are ignored.
static void flameOnResult(int result) {
    if (result == HORN_EXCELLENT) { F.burst = 1.0f; F.age = 0.0f; }
}

static void flameStep(float dt) {
    F.t += dt;
    if (F.t > 1000.0f) F.t -= 1000.0f;
    F.burst -= dt / kFlashTime; if (F.burst < 0) F.burst = 0;
    if (F.age < 1e8f) F.age += dt;                  // time since the last excellent shot
}

static void flameDraw(int W, int H) {
    if (!F.enabled) return;

    // timeline after an excellent shot:  [ full size for kHotTime ] [ fade out over kFadeOutTime ]
    float hotLevel = 1.0f;                          // 1 = full size, 0 = faded out
    if (F.age > kHotTime) {
        float u = (F.age - kHotTime) / kFadeOutTime;
        if (u > 1.0f) u = 1.0f;
        hotLevel = 1.0f - u * u * (3.0f - 2.0f * u);   // smooth ease from 1 down to 0
    }
    if (F.age > 1e8f) hotLevel = 0.0f;              // no excellent shot yet

    // size: idle level, up to full while burning, a little more during the flash
    float power = kIdle + hotLevel * (1.0f - kIdle) + F.burst * 0.35f;
    // opacity: quick fade in at ignition; fades out with the flame unless an idle flame stays
    float fadeIn = F.age / 0.08f; if (fadeIn > 1.0f) fadeIn = 1.0f;
    float alpha  = (kIdle > 0.0f) ? 1.0f : hotLevel * fadeIn;
    if (power <= 0.001f || alpha <= 0.002f) return; // fully faded out: skip drawing, zero cost

    // anchor point on the meter (same layout math as horn_meter.cpp), in GL pixels
    float hPx = kHeightFrac * H, s = hPx / 319.0f;
    float bx  = kCenterX * W - 123.5f * s;
    float by  = (1.0f - kCenterY) * H - hPx * 0.5f;
    float fh  = kFlameSizeFrac * H;                 // box side in pixels = pixels per flame unit
    float ax  = bx + kAnchorX * s + kOffsetX * fh;
    float ay  = by + (319.0f - kAnchorY) * s + kOffsetY * fh;

    // viewport: the square box around the flame; the base sits 0.20 above the bottom edge
    float x0 = ax - 0.5f * fh, y0 = ay - 0.20f * fh;

    // texture state: save, bind the noise on unit 0 (and no sampler object), restore afterwards
    GLint oldActive, oldTex, oldSampler = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    if (Overlay_IsES3()) { glGetIntegerv(GL_SAMPLER_BINDING, &oldSampler); glBindSampler(0, 0); }

    glViewport((int)floorf(x0), (int)floorf(y0), (int)ceilf(fh) + 1, (int)ceilf(fh) + 1);
    glUseProgram(G.prog);
    glUniform2f(G.uOrigin, ax, y0);
    glUniform1f(G.uPx, fh);
    glUniform1f(G.uTime, F.t);
    glUniform1f(G.uPower, power);
    glUniform1f(G.uFlash, F.burst);
    glUniform1f(G.uAlpha, alpha);
    Overlay_BindQuad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (Overlay_IsES3()) glBindSampler(0, oldSampler);
    glBindTexture(GL_TEXTURE_2D, oldTex);
    glActiveTexture(oldActive);
}

// Registers the flame with the core when this file is part of the build. It draws after the
// meter (order 0) and before the banner (order 10), so the banner stays on top.
static OverlayModule g_flame = { "green_flame", flameInit, flameStep, flameDraw, flameOnResult, 5, false };
static OverlayRegistrar g_flameReg(&g_flame);
