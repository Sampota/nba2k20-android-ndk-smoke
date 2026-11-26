/*
 * =====================================================================================
 *  sword_meter.cpp : the sword shot meter (overlay module)
 * =====================================================================================
 *
 *  WHAT IT IS
 *  ----------
 *  A shot meter shaped like the red demon sword (round cross-guard with bone ends, wrapped
 *  handle, pommel), drawn as an OUTLINE ONLY: no colors, no fill. It is an ALTERNATIVE to
 *  horn_meter.cpp: build one or the other (CMake option METER_STYLE). The result banner
 *  (shot_feedback.cpp) and the flame (green_flame.cpp) work with either one and follow it.
 *
 *  This file contains ONLY the sword meter. The hook and GL state handling are in
 *  overlay_core.cpp.
 *
 *  WHAT IT LOOKS LIKE
 *  ------------------
 *   - RESTING: a thin light outline of the sword on a see-through background, with a thin dark
 *     edge so it shows on any background. The perfect zone at the tip of the blade is marked in
 *     green (the blade-tip outline and a short tick across the blade).
 *   - METER: a thin bright LINE travels up the blade, from the guard toward the tip.
 *   - MISS (early / late): no animation. The line just stays where it stopped. (The banner and
 *     the other modules still show their feedback.)
 *   - PERFECT (excellent, the green zone): the WHOLE sword
 *         1. EXPANDS a little (about 10 %), with a small overshoot,
 *         2. GLOWS GREEN: the outline turns bright green and a soft green light spreads around
 *            and just inside it, pulsing gently; a shiny highlight sweeps along the sword and a
 *            few glints twinkle,
 *         3. SHRINKS back to its normal size while the glow fades out.
 *
 *  HOW IT IS DRAWN
 *  ---------------
 *  No meshes and no textures. One fragment shader draws everything from signed-distance math:
 *     - each part of the sword (blade, guard ring, arms, handle, pommel, cloth tail, X wrap) is
 *       a simple shape; only its outline is drawn, and the parts of an outline that are hidden
 *       behind a part in front of it are removed, like a clean line drawing;
 *     - the glow is a soft falloff from the distance to the whole silhouette;
 *     - the shine is a diagonal band that moves along the sword;
 *     - the expansion scales every pixel around the middle of the sword before drawing.
 *
 *  USING IT FROM YOUR GAME
 *  -----------------------
 *      SwordMeter_Set(0.0f .. 1.0f);   // every frame while the shot button is held
 *      Shot_Release(progress);         // on release (core API): a perfect shot plays the animation
 *      SwordMeter_Set(0.0f);           // reset to empty for the next shot
 *      SwordMeter_Set(-1.0f);          // back to the built-in demo loop
 *      SwordMeter_SetVisible(false);   // hide only the meter
 *  By default it plays a demo loop (very early, slight early, excellent, slight late, very late)
 *  until your code calls SwordMeter_Set(>= 0). Only the excellent one animates.
 *
 *  SETTINGS
 *  --------
 *  Position and size: kHeightFrac / kCenterX / kCenterY in horn_common.h (sword block).
 *  Zones: kTooEarly, kWinLo, ... in horn_common.h. If you change kWinLo, change WIN_LO in this
 *  file's shader to the same value.
 *  Animation: kExpandAmt, kExpandTime, kGlowHold, kShrinkTime, kShineTime below.
 *  Look: LINE_COLOR and GREEN in the shader; ZONE_MARK turns the green zone mark on or off.
 *
 *  PERFORMANCE / CPU NOTES (octa-core phones)
 *  ------------------------------------------
 *   - Nothing is baked or generated on the CPU: the sword has no textures at all, so there is
 *     nothing to load and no extra memory.
 *   - Every frame the CPU does only a handful of uniform uploads, and a little arithmetic for
 *     the animation. OpenGL calls must stay on the game's render thread, so extra cores cannot
 *     speed up drawing; the per-frame CPU work is simply almost nothing.
 *   - GPU: while the sword just sits there, only a tight box around it is drawn. A slightly
 *     larger box is used only during the ~1.2 second perfect-shot animation. Pixels far from the
 *     sword leave the shader immediately.
 */

#ifndef SWORD_METER
#error "sword_meter.cpp needs SWORD_METER defined (CMake: -DMETER_STYLE=sword). The banner and flame read the sword layout from horn_common.h."
#endif

#include <math.h>
#include "overlay_core.h"
#include "horn_common.h"

static const float kFillTime    = 1.3f;   // demo: seconds for the line to travel up the blade
static const float kHoldTime    = 2.0f;   // demo: seconds before the next shot

// --- perfect-shot animation ---
static const float kExpandAmt   = 0.10f;  // how much the sword grows (0.10 = 10 %)
static const float kExpandTime  = 0.16f;  // seconds to expand (glow fades in meanwhile)
static const float kGlowHold    = 0.70f;  // seconds it stays expanded and glowing
static const float kShrinkTime  = 0.35f;  // seconds to shrink back (glow fades out meanwhile)
static const float kShineStart  = 0.08f;  // when the shiny highlight starts sweeping, seconds
static const float kShineTime   = 0.55f;  // how long the shine takes to sweep along the sword

// =====================================================================================
//  SHADER
// =====================================================================================
// Design space: the sword is drawn in a 180 x 520 box, y pointing DOWN, tip at the top.
//   blade   y 10 .. 300      guard ring center (90, 340) radius 39      arms x 12 .. 168
//   handle  y 377 .. 469     pommel center (90, 478)                    spike y 495 .. 519
// The expansion scales around the middle of the box, (90, 260).

static const char* FS_SWORD = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define ZONE_MARK 1                    // 1 = mark the green perfect zone, 0 = plain outline only
uniform vec2  uOrigin;                 // pixel position of design point (0, 520), the box's bottom-left
uniform float uScale;                  // pixels per design unit
uniform float uZoom;                   // 1 = normal size, about 1.1 while expanded
uniform float uFill;                   // line position along the blade, 0 = guard .. 1 = tip
uniform float uGlow;                   // green glow strength 0..1 (whole sword)
uniform float uShine;                  // position of the shine band along the sword (-5 = off)
uniform float uTime;                   // seconds into the animation, for the twinkle
uniform float uAlpha;

const vec2  ZOOMC = vec2(90.0, 260.0); // the sword expands around its middle
const vec2  RC    = vec2(90.0, 340.0); // center of the guard ring
const float WIN_LO = 0.90;             // zone start, keep equal to kWinLo
const float ZONE_Y = 300.0 - 284.0 * WIN_LO;   // blade y where the green zone starts
const vec3  LINE_COLOR = vec3(0.96);   // outline color while resting
const vec3  GREEN = vec3(0.22, 1.00, 0.35);

float W;                                           // half anti-aliasing width, design units
float cov(float x) { return 1.0 - smoothstep(-W, W, x); }       // 1 inside (x<0), 0 outside

float sdBox(vec2 p, vec2 c, vec2 hs) {
    vec2 d = abs(p - c) - hs;
    return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0);
}
float sdSeg(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}
// half width of the blade at height y: a pointed tip, then a slight widening toward the guard
float bladeHW(float y) {
    return max(y < 46.0 ? 14.0 * (y - 10.0) / 36.0 : 14.0 + 3.0 * (y - 46.0) / 254.0, 0.0);
}
float sdBlade(vec2 p) {
    float d = max(abs(p.x - 90.0) - bladeHW(p.y), max(10.0 - p.y, p.y - 300.0));
    return d * 0.95;
}
// a soft four-point star used for the twinkles
float glint(vec2 q) {
    vec2 a = abs(q) / 20.0;
    return exp(-dot(a, a) * 10.0) + 0.7 * (exp(-a.x * 16.0 - a.y * 1.4) + exp(-a.y * 16.0 - a.x * 1.4));
}

void main() {
    W = 0.8 / (uScale * uZoom);
    vec2 pw = (gl_FragCoord.xy - uOrigin) / uScale;       // pixel -> design space
    pw.y = 520.0 - pw.y;
    vec2 p = ZOOMC + (pw - ZOOMC) / uZoom;                // undo the expansion
    if (abs(p.x - 90.0) > 130.0 || p.y < -45.0 || p.y > 570.0) { gl_FragColor = vec4(0.0); return; }

    // ---- distances to every part ----
    float dBlade  = sdBlade(p);
    float dRing   = length(p - RC) - 39.0;
    float dArm    = sdBox(p, RC, vec2(78.0, 6.5));
    float dKnob   = min(min(length(p - vec2(14.0, 333.0)) - 8.0, length(p - vec2(14.0, 347.0)) - 8.0),
                        min(length(p - vec2(166.0, 333.0)) - 8.0, length(p - vec2(166.0, 347.0)) - 8.0));
    float dArms   = min(dArm, dKnob);                      // bone-like ends
    float dHandle = sdBox(p, vec2(90.0, 423.0), vec2(12.0, 46.0));
    float dPom    = length(p - vec2(90.0, 478.0)) - 19.0;
    float dSpike  = sdBox(p, vec2(90.0, 507.0), vec2(5.0, 12.0));
    float dTail   = sdSeg(p, vec2(70.0, 350.0), vec2(62.0, 398.0)) - 4.5;   // cloth tail
    float dX      = min(sdSeg(p, vec2(80.0, 330.0), vec2(100.0, 350.0)),
                        sdSeg(p, vec2(100.0, 330.0), vec2(80.0, 350.0))) - 3.0;   // wrap on the ring
    float d = min(min(min(dBlade, dRing), min(dArms, dHandle)), min(min(dPom, dSpike), dTail));   // whole silhouette

    // ---- outline only: draw each part's edge, but not where a part in front hides it ----
    const float HW = 1.1;                                  // half of the line width
    float sSpike  = cov(abs(dSpike)  - HW) * (1.0 - cov(dPom));
    float sPom    = cov(abs(dPom)    - HW) * (1.0 - cov(dHandle));
    float sHandle = cov(abs(dHandle) - HW) * (1.0 - cov(dRing));
    float sBlade  = cov(abs(dBlade)  - HW) * (1.0 - cov(dRing));
    float sArms   = cov(abs(dArms)   - HW) * (1.0 - cov(dRing));
    float sRing   = cov(abs(dRing)   - HW) * (1.0 - cov(dTail));
    float sTail   = cov(abs(dTail)   - HW);
    float hidInner = (1.0 - cov(dX)) * (1.0 - cov(dTail)) * cov(dRing + HW);   // inside the ring, not under the X
    float rr = length(p - RC);
    float sInner  = cov(abs(rr - 30.5) - 0.8) * hidInner;                      // inner ring line
    float sCross  = cov(min(abs(p.x - 90.0), abs(p.y - 340.0)) - 0.8) * cov(rr - 30.0) * hidInner;   // crosshair
    float sX      = cov(abs(dX) - HW) * (1.0 - cov(dTail));
    float sDiv    = cov(abs(p.y - 340.0) - 0.7) * cov(dArm + HW) * (1.0 - cov(dRing));   // line between the two arm bars
    float ph      = fract((p.y + (p.x - 90.0) * 0.7) / 10.0);                  // leather wrap lines on the handle
    float sWrap   = cov(min(ph, 1.0 - ph) * 8.2 - 0.55) * cov(dHandle + 1.2) * 0.55 * (1.0 - cov(dRing));

    float sAll = max(max(max(sSpike, sPom), max(sHandle, sBlade)), max(max(sArms, sRing), max(sInner, sCross)));
    sAll = max(max(sAll, max(sX, sTail)), max(sDiv, sWrap));

    // green perfect zone mark: the blade-tip outline above the zone line and a tick across the blade
    float aGreen = 0.0;
#if ZONE_MARK
    float inZone = 1.0 - smoothstep(ZONE_Y - W, ZONE_Y + W, p.y);              // 1 above the zone line
    float tick   = cov(abs(p.y - ZONE_Y) - 0.9) * (1.0 - smoothstep(bladeHW(ZONE_Y) + 6.0, bladeHW(ZONE_Y) + 8.0, abs(p.x - 90.0)));
    aGreen = max(sBlade * inZone, tick);
    sAll   = max(sAll, tick);
#endif

    // ---- glow: whole sword ----
    float bq      = (((520.0 - p.y) / 520.0 + (p.x - 90.0) / 520.0 * 0.7) - uShine) / 0.07;   // distance to the shine band
    float band    = uShine > -2.0 ? exp(-bq * bq) : 0.0;                       // diagonal band moving along the sword
    float shine   = band * uGlow;
    float inside  = cov(d);
    float halo    = exp(-max(d, 0.0) / 14.0) * uGlow * 0.65;                   // soft light outside the outline
    float inner   = inside * (0.10 + 0.25 * exp(min(d, 0.0) / 8.0)) * uGlow;  // soft light just inside it
    float shineF  = shine * 0.35 * inside;                                     // the shiny sweep, also inside
    float tw      = 0.5 + 0.5 * sin(uTime * 16.0);
    float glints  = (glint(p - vec2(90.0, 26.0)) * tw
                   + glint(p - vec2(60.0, 318.0)) * (1.0 - tw)
                   + glint(p - vec2(166.0, 340.0)) * (0.5 + 0.5 * sin(uTime * 16.0 + 2.0))) * uGlow * 0.55;

    // outline color: light normally, green in the zone mark, green and shiny while glowing
    vec3 sc = mix(LINE_COLOR, GREEN, clamp(aGreen / max(sAll, 0.001), 0.0, 1.0));
    sc = mix(sc, GREEN, uGlow);
    sc = mix(sc, vec3(0.72, 1.00, 0.78), uGlow * 0.4);                         // hot neon core
    sc = mix(sc, vec3(0.93, 1.00, 0.95), shine * 0.9);                         // shiny highlight on the line
    float rim = cov(d - 2.4) * (1.0 - cov(d)) * 0.45;                          // dark edge just outside, for visibility
    float a   = sAll + (1.0 - sAll) * rim;
    vec3 rgb  = sc * sAll;

    // ---- the meter line: a thin bright line across the blade that moves up ----
    if (uFill > 0.001) {
        float yL  = 300.0 - 284.0 * uFill;
        float hw  = bladeHW(yL);
        float inB = 1.0 - smoothstep(hw + 9.0, hw + 11.0, abs(p.x - 90.0));   // a small tick past each edge
        vec3  lc  = mix(vec3(1.0), GREEN, uGlow);
        float lm  = cov(abs(p.y - yL) - 1.2) * inB;
        float lg  = exp(-abs(p.y - yL) / 3.5) * 0.45 * inB;
        rgb = rgb * (1.0 - lm) + lc * lm;
        a   = max(a, lm);
        rgb += lc * lg * (1.0 - a);
        a   = min(a + lg * (1.0 - a), 1.0);
    }

    // ---- glow on top (additive) ----
    vec3  fxc = GREEN * (halo + inner) + vec3(0.90, 1.00, 0.92) * shineF + vec3(0.60, 1.00, 0.65) * glints;
    float fxa = clamp(halo + inner + shineF * 0.7 + glints * 0.8, 0.0, 1.0);
    rgb += fxc * (1.0 - 0.5 * a);
    a    = min(a + fxa * (1.0 - a), 1.0);
    rgb  = min(rgb, vec3(1.0));

    gl_FragColor = vec4(rgb, a) * uAlpha;                                      // premultiplied alpha output
}
)";

// =====================================================================================
//  STATE
// =====================================================================================

static struct {
    GLuint prog = 0;
    GLint uOrigin = -1, uScale = -1, uZoom = -1, uFill = -1, uGlow = -1, uShine = -1, uTime = -1, uAlpha = -1;
} G;

static struct {
    float fill = 0;                        // line position, 0..1
    float external = -1.0f;                // >= 0 means your code drives the line
    float animT = -1.0f;                   // seconds since a perfect shot; < 0 = no animation
    int   phase = 0, ti = 0;               // demo state machine
    float hold = 0;
    bool  visible = true;
} M;

// =====================================================================================
//  PUBLIC API
// =====================================================================================

// Drive the line yourself (0..1). Pass -1 to go back to the built-in demo loop.
extern "C" void SwordMeter_Set(float fill01)  { M.external = fill01; }

// Show or hide only the sword (the banner and flame are not affected).
extern "C" void SwordMeter_SetVisible(bool v) { M.visible = v; }

// =====================================================================================
//  PERFECT-SHOT ANIMATION
// =====================================================================================
// Timeline after an excellent release (t = seconds):
//   0 .. kExpandTime                       expand (with a small overshoot), glow fades in
//   .. + kGlowHold                         stay expanded, glow pulses, the shine sweeps by
//   .. + kShrinkTime                       shrink back to normal, glow fades out
// Returns the zoom factor and glow strength for time t.

static float easeOutBack(float u) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float t = u - 1.0f;
    return 1.0f + c3 * t * t * t + c1 * t * t;
}

static float animTotal() { return kExpandTime + kGlowHold + kShrinkTime; }

static void animValues(float t, float* zoom, float* glow) {
    if (t < kExpandTime) {                                   // expand
        float u = t / kExpandTime;
        *zoom = 1.0f + kExpandAmt * easeOutBack(u);
        *glow = u;
    } else if (t < kExpandTime + kGlowHold) {                // hold: breathe and pulse
        float v = t - kExpandTime;
        *zoom = 1.0f + kExpandAmt + 0.008f * sinf(v * 14.0f);
        *glow = 0.92f + 0.08f * sinf(v * 18.0f);
    } else {                                                 // shrink
        float u = (t - kExpandTime - kGlowHold) / kShrinkTime;
        if (u > 1.0f) u = 1.0f;
        float s = u * u * (3.0f - 2.0f * u);
        *zoom = 1.0f + kExpandAmt * (1.0f - s);
        *glow = 1.0f - s;
    }
}

// =====================================================================================
//  MODULE CALLBACKS
// =====================================================================================

static bool swordInit() {
    G.prog = Overlay_MakeProgram("", FS_SWORD);
    if (!G.prog) return false;
    G.uOrigin = glGetUniformLocation(G.prog, "uOrigin");
    G.uScale  = glGetUniformLocation(G.prog, "uScale");
    G.uZoom   = glGetUniformLocation(G.prog, "uZoom");
    G.uFill   = glGetUniformLocation(G.prog, "uFill");
    G.uGlow   = glGetUniformLocation(G.prog, "uGlow");
    G.uShine  = glGetUniformLocation(G.prog, "uShine");
    G.uTime   = glGetUniformLocation(G.prog, "uTime");
    G.uAlpha  = glGetUniformLocation(G.prog, "uAlpha");
    glUseProgram(G.prog);                  // the core restores the game's program afterwards
    glUniform1f(G.uAlpha, 1.0f);
    return true;
}

// A shot was released: only a PERFECT (excellent) shot plays the animation. Misses do nothing here.
static void swordOnResult(int result) {
    if (result == HORN_EXCELLENT) M.animT = 0.0f;
}

static void swordStep(float dt) {
    if (M.animT >= 0.0f) {
        M.animT += dt;
        if (M.animT > animTotal()) M.animT = -1.0f;
    }

    if (M.external >= 0.0f) { M.fill = M.external; return; }   // your code drives the line

    // Demo loop: releases at these progress values in order ->
    // very early, slight early, EXCELLENT (the only one that animates), slight late, very late.
    static const float targets[5] = {0.74f, 0.85f, 0.93f, 0.985f, 1.0f};
    if (M.phase == 0) {                          // phase 0: the line travels up the blade
        M.fill += dt / kFillTime;
        float tgt = targets[M.ti];
        if (M.fill >= tgt) {
            M.fill = tgt;
            Shot_Release(tgt);                   // notifies every module (this one, banner, flame)
            M.phase = 1; M.hold = kHoldTime;
        }
    } else {                                     // phase 1: showing the result
        M.hold -= dt;
        if (M.hold <= 0) { M.fill = 0; M.phase = 0; M.ti = (M.ti + 1) % 5; }
    }
}

static void swordDraw(int W, int H) {
    if (!M.visible) return;

    // ---- animation values for this frame ----
    float zoom = 1.0f, glow = 0.0f, shine = -5.0f, t = 0.0f;
    bool active = (M.animT >= 0.0f);
    if (active) {
        t = M.animT;
        animValues(t, &zoom, &glow);
        float sp = (t - kShineStart) / kShineTime;           // shine sweeps from the pommel to the tip
        if (sp >= 0.0f && sp <= 1.0f) shine = -0.2f + 1.4f * sp;
    }

    // ---- placement: the 180 x 520 design box is centered on the meter center ----
    float hPx = kHeightFrac * H, s = hPx / kMeterBoxH;
    float cx  = kCenterX * W, cy = (1.0f - kCenterY) * H;
    float ox  = cx - 0.5f * kMeterBoxW * s;                  // GL pixel of design point (0, 520)
    float oy  = cy - 0.5f * kMeterBoxH * s;

    // Tight viewport while the sword just sits there; a slightly larger one only during the
    // animation (expansion and glow). Design x -25 .. 205 / y -10 .. 535 at rest,
    // x -70 .. 250 / y -70 .. 600 while animating.
    float dx0 = active ? -70.0f : -25.0f, dx1 = active ? 250.0f : 205.0f;
    float dyT = active ? -70.0f : -10.0f, dyB = active ? 600.0f : 535.0f;
    glViewport((int)floorf(ox + dx0 * s), (int)floorf(oy + (520.0f - dyB) * s),
               (int)ceilf((dx1 - dx0) * s) + 1, (int)ceilf((dyB - dyT) * s) + 1);

    glUseProgram(G.prog);
    glUniform2f(G.uOrigin, ox, oy);
    glUniform1f(G.uScale, s);
    glUniform1f(G.uZoom, zoom);
    glUniform1f(G.uFill, M.fill);
    glUniform1f(G.uGlow, glow);
    glUniform1f(G.uShine, shine);
    glUniform1f(G.uTime, t);
    Overlay_BindQuad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Registers the sword meter with the core when this file is part of the build.
static OverlayModule g_sword = { "sword_meter", swordInit, swordStep, swordDraw, swordOnResult, 0, false };
static OverlayRegistrar g_swordReg(&g_sword);
