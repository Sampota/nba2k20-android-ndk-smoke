/*
 * =====================================================================================
 *  crown_animation.cpp : the Crown Animation (overlay module)
 * =====================================================================================
 *
 *  WHAT IT IS
 *  ----------
 *  A green crown that pops up, spins, glows and shines when the player hits the green zone
 *  (a PERFECT / excellent shot) on the shot meter. It is the same kind of effect as the green
 *  flame: it starts on an excellent result, plays for about two seconds, then closes.
 *
 *  This file contains ONLY the crown. The shot meter (sword_meter.cpp or horn_meter.cpp), the
 *  banner (shot_feedback.cpp) and the flame (green_flame.cpp) are separate modules, and the hook
 *  and GL state handling are in overlay_core.cpp. Like them, this module registers itself with the
 *  core when the file is part of the build, so you can build with or without it. It does not call
 *  or include any of the other modules.
 *
 *  WHAT IT LOOKS LIKE
 *  ------------------
 *   - A crown with five pointed spikes topped with round balls, a jeweled band with a row of beads
 *     along its top and bottom edge, round studs, and a medallion under every spike (inspired by
 *     a classic gold crown, but all green).
 *   - SPINNING: it turns around its vertical axis like a turntable. Spikes, beads, studs and
 *     medallions move around it, spikes go behind it and come back to the front.
 *   - EXPANDING: it pops in small and grows past its normal size with a little overshoot, with a
 *     bright green flash.
 *   - GLOWING AND SHINING: a soft green glow pulses around it, a shiny highlight sweeps across it
 *     again and again, and the ball tips twinkle.
 *   - CLOSING: at the end it shrinks while it fades and the glow dies out.
 *
 *  TIMELINE (after an excellent shot, in seconds)
 *  ----------------------------------------------
 *      0 .. kAppearTime       expand in (small -> a bit bigger -> normal) with the flash, fade in
 *      .. + kHoldTime         spin, breathe, glow and shine
 *      .. + kShrinkTime       shrink and fade out
 *
 *  HOW IT IS DRAWN
 *  ---------------
 *  No meshes and no textures. One fragment shader draws the crown from simple 2D shapes that are
 *  placed as if they stood on a ring seen from slightly above:
 *     - the crown's rim is an ellipse; every spike, bead, stud and medallion sits at an angle
 *       around that ring, and the spin simply adds to every angle;
 *     - a point at angle t is drawn at x = R sin(t) and moves up or down by cos(t): spikes at the
 *       back are drawn first (seen from the inside, darker), then the inside of the crown, then the
 *       spikes at the front and the front wall of the band over them (painter's order);
 *     - lighting is a fixed light in front and to the left, so highlights stay in place while the
 *       crown turns under them.
 *
 *  USING IT FROM YOUR GAME
 *  -----------------------
 *      Shot_Release(progress);          // core API: an excellent result starts the crown
 *      Shot_ReleaseResult(HORN_*);      // same, when your game already knows the result
 *      CrownAnimation_SetEnabled(false);// turn the crown off
 *
 *  SETTINGS
 *  --------
 *  Position relative to the meter: kCrownDX / kCrownDY in horn_common.h.
 *  Size: kCrownWidthFrac below. Timing: kAppearTime, kHoldTime, kShrinkTime. Spin: kSpinSpeed.
 *  Look: the color constants at the top of the shader (DARK, MID, LIGHT, GREEN).
 *
 *  PERFORMANCE / CPU NOTES (octa-core phones)
 *  ------------------------------------------
 *   - Nothing is baked or generated on the CPU and there are no textures, so there is nothing to
 *     load and no extra memory.
 *   - It costs nothing while it is not playing: no drawing, no GL calls.
 *   - While it plays (about 2.2 s per perfect shot) the CPU only uploads a handful of uniforms per
 *     frame. OpenGL calls must stay on the game's render thread, so extra cores cannot speed up the
 *     drawing; the per-frame CPU work is simply almost nothing.
 *   - GPU: one small viewport around the crown, with an early exit for pixels far from it.
 */

#include <math.h>
#include "overlay_core.h"
#include "horn_common.h"

// --- settings ---
static const float kCrownWidthFrac = 0.15f;  // crown width (about 210 design units) relative to screen height
static const float kAppearTime  = 0.30f;     // expand in
static const float kHoldTime    = 1.50f;     // spin, glow and shine
static const float kShrinkTime  = 0.40f;     // shrink and fade out
static const float kStartZoom   = 0.35f;     // size it starts from (1 = normal size)
static const float kShrinkTo    = 0.55f;     // size it shrinks down to at the end
static const float kSpinSpeed   = 5.0f;      // radians per second (5.0 = about 0.8 turns per second)
static const float kShineEvery  = 0.70f;     // seconds between shine sweeps

// =====================================================================================
//  SHADER
// =====================================================================================
// Design space: the crown is drawn around (0, 0), y pointing DOWN.
//   rim ellipse radius 95 x 24 centered at y = 18 (the top of the band), band height 34,
//   spikes 78 high with a ball of radius 8.5 on top.
// The drawing area (viewport) covers x -165 .. 165 and y -145 .. 120.

static const char* FS_CROWN = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec2  uOrigin;                 // pixel position of design point (0, 0)
uniform float uScale;                  // pixels per design unit
uniform float uZoom;                   // size of the crown: 1 = normal
uniform float uSpin;                   // spin angle in radians
uniform float uGlow;                   // green glow strength 0..1
uniform float uFlash;                  // bright flash 1 -> 0 at the start
uniform float uShine;                  // position of the shine band (-5 = off)
uniform float uTime;                   // seconds into the animation, for the twinkle
uniform float uAlpha;                  // overall fade

const float PI = 3.14159265;
const float R  = 95.0;                 // radius of the rim
const float RY = 24.0;                 // vertical radius of the rim ellipse (the tilt)
const float YT = 18.0;                 // y of the center of the top rim
const float HB = 34.0;                 // height of the band
const float HS = 78.0;                 // height of a spike
const float DELTA = 0.34;              // half angular width of a spike's base
const float BALLR = 8.5;               // radius of the ball on a spike
const vec2  LDIR = vec2(-0.45, 0.89);  // light direction: left and in front (x, z)
const vec2  LH   = vec2(-0.24, 0.97);  // half vector for the shiny highlights
const vec3  DARK  = vec3(0.01, 0.20, 0.08);
const vec3  MID   = vec3(0.10, 0.72, 0.28);
const vec3  LIGHT = vec3(0.72, 1.00, 0.80);
const vec3  EDGE  = vec3(0.00, 0.09, 0.04);   // outline color
const vec3  GREEN = vec3(0.22, 1.00, 0.35);

float W;                                           // half anti-aliasing width, design units
float cov(float x) { return 1.0 - smoothstep(-W, W, x); }       // 1 inside (x<0), 0 outside

// dark -> mid -> light green color ramp, s in 0..1
vec3 ramp(float s) {
    vec3 c = mix(DARK, MID, smoothstep(0.0, 0.55, s));
    return mix(c, LIGHT, smoothstep(0.60, 1.0, s));
}
float sdSeg(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}
float sdBox(vec2 p, vec2 c, vec2 hs) {
    vec2 d = abs(p - c) - hs;
    return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0);
}
float sdTri(vec2 p, vec2 a, vec2 b, vec2 c) {
    vec2 e0 = b - a, e1 = c - b, e2 = a - c;
    vec2 v0 = p - a, v1 = p - b, v2 = p - c;
    vec2 q0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 q1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 q2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = e0.x * e2.y - e0.y * e2.x;
    vec2 d = min(min(vec2(dot(q0, q0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(q1, q1), s * (v1.x * e1.y - v1.y * e1.x))),
                     vec2(dot(q2, q2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}
// signed distance to the rim ellipse centered at height yc (approximate, but with a true unit gradient)
float ellD(vec2 p, float yc) {
    vec2 q = vec2(p.x, p.y - yc);
    float k0 = length(q / vec2(R, RY));
    float k1 = length(q / vec2(R * R, RY * RY));
    return k0 * (k0 - 1.0) / max(k1, 0.0001);
}
// a soft four-point star used for the twinkles
float glint(vec2 q) {
    vec2 a = abs(q) / 18.0;
    return exp(-dot(a, a) * 10.0) + 0.7 * (exp(-a.x * 16.0 - a.y * 1.4) + exp(-a.y * 16.0 - a.x * 1.4));
}
// paint one shape: dark outline of width ow, then the fill color inside
void layer(inout vec3 col, inout float a, float d, vec3 fill, float ow) {
    col = mix(col, EDGE, cov(d));
    col = mix(col, fill, cov(d + ow));
    a = max(a, cov(d));
}
// beads along an elliptical arc of the rim at height yc: side +1 = front arc, -1 = back arc.
// N beads around the whole ring, so the spin moves them. Returns the distance to the nearest bead.
float beads(vec2 p, float yc, float side, float N, float rb, out float hi, out float cs) {
    float sx = clamp(p.x / R, -0.999, 0.999);
    cs = sqrt(1.0 - sx * sx);                                   // cosine of the screen angle
    float ths = side > 0.0 ? asin(sx) : PI - asin(sx);          // angle around the ring, as seen on screen
    float ph  = fract((ths - uSpin) * N / (2.0 * PI));          // position between two beads (the spin moves them)
    float dx  = (ph - 0.5) * (2.0 * PI / N) * R * cs;           // sideways distance to the nearest bead center
    float dy  = p.y - (yc + side * RY * cs);
    hi = exp(-length(vec2(dx + 0.3 * rb, dy + 0.35 * rb)) / (0.55 * rb));
    return length(vec2(dx, dy)) - rb;
}

// one spike (plate + ball); wantFront selects which half of the ring this call draws
void spike(inout vec3 col, inout float a, inout float dAll, inout float glints,
           vec2 p, float th, float wantFront) {
    float c = cos(th), s = sin(th);
    if ((c >= 0.0) != (wantFront > 0.5)) return;
    vec2 bl  = vec2(R * sin(th - DELTA), YT + RY * cos(th - DELTA) + 4.0);   // base corners on the rim
    vec2 br  = vec2(R * sin(th + DELTA), YT + RY * cos(th + DELTA) + 4.0);
    vec2 tip = vec2(R * s, YT + RY * c - HS);
    float dT = sdTri(p, bl, br, tip);
    vec2  bc = tip + vec2(0.0, -3.0);                                        // ball center
    float dB = length(p - bc) - BALLR;

    // plate shading: the plate faces outward (seen from the inside when it is at the back)
    float sg   = c >= 0.0 ? 1.0 : -1.0;
    vec2  n    = vec2(s * sg, c * sg);
    float diff = max(dot(n, LDIR), 0.0);
    float spec = pow(max(dot(n, LH), 0.0), 18.0);
    float grad = clamp((YT + RY * c - p.y) / HS, 0.0, 1.0);                  // 0 at the base .. 1 at the tip
    vec3  fill = ramp(0.20 + 0.55 * diff + 0.18 * grad) + LIGHT * spec * 0.40;
    layer(col, a, dT, fill, 1.6);
    float ridge = cov(sdSeg(p, (bl + br) * 0.5, tip) - 1.3) * cov(dT + 1.6) * (0.25 + 0.45 * diff);
    col = mix(col, LIGHT, ridge);                                            // raised spine of the plate

    // ball: lit like a small sphere
    vec2  q  = (p - bc) / BALLR;
    float zq = sqrt(max(1.0 - dot(q, q), 0.0));
    float bd = max(dot(vec3(q.x, -q.y, zq), vec3(-0.45, 0.55, 0.70)), 0.0);
    layer(col, a, dB, ramp(0.25 + 0.75 * bd) + LIGHT * pow(bd, 24.0) * 0.7, 1.4);

    dAll = min(dAll, min(dT, dB));
    if (c >= 0.0) glints += glint(p - bc) * (0.5 + 0.5 * sin(uTime * 9.0 + th * 3.0));   // the front balls twinkle
}

void main() {
    W = 0.8 / (uScale * uZoom);
    vec2 q = (gl_FragCoord.xy - uOrigin) / uScale;          // pixel -> design space
    q.y = -q.y;
    vec2 p = q / uZoom;                                     // undo the crown's size
    if (abs(p.x) > 145.0 || p.y < -135.0 || p.y > 115.0) { gl_FragColor = vec4(0.0); return; }

    vec3  col = EDGE;
    float a = 0.0, dAll = 1000.0, glints = 0.0;

    // ---- 1. spikes at the back (seen from the inside) ----
    for (int i = 0; i < 5; i++) spike(col, a, dAll, glints, p, uSpin + float(i) * 1.2566371, 0.0);

    // ---- 2. the inside of the crown, with the beads on the back rim ----
    float dTop = ellD(p, YT);
    {
        float c  = sqrt(max(1.0 - p.x * p.x / (R * R), 0.0));
        float t  = clamp((p.y - (YT - RY * c)) / max(2.0 * RY * c, 0.001), 0.0, 1.0);   // 0 at the back rim .. 1 at the front
        layer(col, a, dTop, ramp(0.10 + 0.50 * t * t), 1.8);
        float hi, cs;
        float dBead = beads(p, YT, -1.0, 40.0, 2.6, hi, cs);
        layer(col, a, dBead, ramp(0.35 + 0.5 * hi), 0.8);
    }

    // ---- 3. spikes at the front ----
    for (int i = 0; i < 5; i++) spike(col, a, dAll, glints, p, uSpin + float(i) * 1.2566371, 1.0);

    // ---- 4. the front wall of the band, with its beads, studs and medallions ----
    float dEllBot = ellD(p, YT + HB);
    float dBand   = min(min(dTop, dEllBot), sdBox(p, vec2(0.0, YT + 0.5 * HB), vec2(R, 0.5 * HB)));   // whole band silhouette
    float dWall   = max(dBand, -dTop);                                                                // below the top rim = front wall
    dAll = min(dAll, dBand);
    {
        float sx = clamp(p.x / R, -1.0, 1.0);
        float c  = sqrt(max(1.0 - sx * sx, 0.0));
        vec2  nW = vec2(sx, c);                                    // the wall is a cylinder: its normal follows x
        float diff = max(dot(nW, LDIR), 0.0);
        float spec = pow(max(dot(nW, LH), 0.0), 26.0);
        float vg = clamp((p.y - (YT + RY * c)) / HB, 0.0, 1.0);    // 0 at the top edge .. 1 at the bottom edge
        layer(col, a, dWall, ramp(0.20 + 0.62 * diff - 0.14 * vg) + LIGHT * spec * 0.55, 1.8);

        float inWall = cov(dWall + 1.0) * step(abs(p.x), 0.985 * R);
        float hi, cs, dB1;
        dB1 = beads(p, YT, 1.0, 40.0, 3.0, hi, cs);                // beads along the top edge
        layer(col, a, dB1, ramp(0.40 + 0.55 * hi), 0.9);
        dB1 = beads(p, YT + HB, 1.0, 40.0, 3.0, hi, cs);           // beads along the bottom edge
        layer(col, a, dB1, ramp(0.40 + 0.55 * hi), 0.9);

        // round studs halfway down the band (10 around the ring)
        {
            float sx2 = clamp(p.x / R, -0.999, 0.999);
            float cs2 = sqrt(1.0 - sx2 * sx2);
            float ph  = fract((asin(sx2) - uSpin) * 10.0 / (2.0 * PI) + 0.25);       // offset so studs sit between the medallions
            float dx  = (ph - 0.5) * (2.0 * PI / 10.0) * R * cs2;
            float dy  = p.y - (YT + RY * cs2 + HB * 0.60);
            float dSt = length(vec2(dx / max(cs2, 0.3), dy)) - 4.6;
            float hiS = exp(-length(vec2(dx / max(cs2, 0.3) + 1.5, dy + 1.6)) / 2.6);
            layer(col, a, mix(1.0, dSt, inWall), ramp(0.55 + 0.45 * hiS), 0.9);
        }
        // a medallion under every spike: ring plus a gem
        {
            float sx2 = clamp(p.x / R, -0.999, 0.999);
            float cs2 = sqrt(1.0 - sx2 * sx2);
            float ph  = fract((asin(sx2) - uSpin) * 5.0 / (2.0 * PI) + 0.5);
            float dx  = (ph - 0.5) * (2.0 * PI / 5.0) * R * cs2;
            vec2  m   = vec2(dx / max(cs2, 0.3), p.y - (YT + RY * cs2 + 15.0));
            float dMed = length(m) - 12.0;
            float hiM  = exp(-length(m + vec2(3.0, 3.5)) / 6.0);
            layer(col, a, mix(1.0, dMed, inWall), ramp(0.32 + 0.30 * diff + 0.10 * hiM), 1.2);
            layer(col, a, mix(1.0, length(m) - 7.0, inWall), ramp(0.18 + 0.60 * hiM), 1.0);
        }
    }

    // ---- shine, flash, glow ----
    float win   = smoothstep(0.0, 30.0, min(min(165.0 - abs(q.x), q.y + 145.0), 120.0 - q.y));   // fades the glow before the drawing area's edge
    float bq    = ((p.x * 0.75 - p.y * 0.65) / 200.0 - uShine) / 0.09;
    float band  = uShine > -2.0 ? exp(-bq * bq) : 0.0;                          // diagonal shiny band sweeping across
    float shine = band * uGlow;
    vec3  cc    = mix(col, vec3(0.92, 1.00, 0.95), shine * 0.80);
    cc += GREEN * uFlash * 0.55;                                                // flash at the start
    float halo  = exp(-max(dAll, 0.0) / 16.0) * uGlow * 0.70 * win;             // soft green light around the crown
    float inner = a * 0.10 * uGlow;                                             // faint green sheen on the crown
    vec3  fxc   = GREEN * (halo + inner) + vec3(0.60, 1.00, 0.65) * glints * uGlow * 0.55;
    float fxa   = clamp(halo + inner + glints * uGlow * 0.45, 0.0, 1.0);

    vec3  rgb = cc * a;
    rgb += fxc * (1.0 - 0.5 * a);
    float al  = min(a + fxa * (1.0 - a), 1.0);
    rgb = min(rgb, vec3(1.0));
    gl_FragColor = vec4(rgb, al) * (uAlpha * smoothstep(0.0, 6.0, min(165.0 - abs(q.x), min(q.y + 145.0, 120.0 - q.y))));
}
)";

// =====================================================================================
//  STATE
// =====================================================================================

static struct {
    GLuint prog = 0;
    GLint uOrigin = -1, uScale = -1, uZoom = -1, uSpin = -1, uGlow = -1, uFlash = -1,
          uShine = -1, uTime = -1, uAlpha = -1;
} G;

static struct {
    float animT = -1.0f;        // seconds since a perfect shot; < 0 = not playing
    bool  enabled = true;
} C;

// =====================================================================================
//  PUBLIC API
// =====================================================================================

// Turn the crown on or off.
extern "C" void CrownAnimation_SetEnabled(bool v) { C.enabled = v; }

// =====================================================================================
//  ANIMATION
// =====================================================================================

static float easeOutBack(float u) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float t = u - 1.0f;
    return 1.0f + c3 * t * t * t + c1 * t * t;
}
static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float totalTime() { return kAppearTime + kHoldTime + kShrinkTime; }

// Fills in the zoom, fade, glow, flash and shine for time t (seconds since the shot).
static void animValues(float t, float* zoom, float* alpha, float* glow, float* flash, float* shine) {
    *shine = -5.0f;
    if (t < kAppearTime) {                                   // expand in, with a small overshoot
        float u = t / kAppearTime;
        *zoom  = kStartZoom + (1.0f - kStartZoom) * easeOutBack(u);
        *alpha = clamp01(u * 4.0f);
        *glow  = u;
        *flash = 1.0f - u;
    } else if (t < kAppearTime + kHoldTime) {                // spin, breathe, glow and shine
        float v = t - kAppearTime;
        *zoom  = 1.0f + 0.04f * sinf(v * 6.0f);
        *alpha = 1.0f;
        *glow  = 0.85f + 0.15f * sinf(v * 9.0f);
        *flash = 0.0f;
        float sp = fmodf(v, kShineEvery) / kShineEvery;      // a new shine sweep every kShineEvery seconds
        *shine = -0.8f + 1.6f * sp;
    } else {                                                 // shrink and fade out
        float u = clamp01((t - kAppearTime - kHoldTime) / kShrinkTime);
        float s = u * u * (3.0f - 2.0f * u);
        *zoom  = 1.0f - (1.0f - kShrinkTo) * s;
        *alpha = 1.0f - s;
        *glow  = 1.0f - s;
        *flash = 0.0f;
    }
}

// =====================================================================================
//  MODULE CALLBACKS
// =====================================================================================

static bool crownInit() {
    G.prog = Overlay_MakeProgram("", FS_CROWN);
    if (!G.prog) return false;
    G.uOrigin = glGetUniformLocation(G.prog, "uOrigin");
    G.uScale  = glGetUniformLocation(G.prog, "uScale");
    G.uZoom   = glGetUniformLocation(G.prog, "uZoom");
    G.uSpin   = glGetUniformLocation(G.prog, "uSpin");
    G.uGlow   = glGetUniformLocation(G.prog, "uGlow");
    G.uFlash  = glGetUniformLocation(G.prog, "uFlash");
    G.uShine  = glGetUniformLocation(G.prog, "uShine");
    G.uTime   = glGetUniformLocation(G.prog, "uTime");
    G.uAlpha  = glGetUniformLocation(G.prog, "uAlpha");
    return true;
}

// An excellent shot (hit in the green zone) starts the crown. Other results are ignored.
static void crownOnResult(int result) {
    if (result == HORN_EXCELLENT) C.animT = 0.0f;
}

static void crownStep(float dt) {
    if (C.animT >= 0.0f) {
        C.animT += dt;
        if (C.animT > totalTime()) C.animT = -1.0f;          // finished: nothing is drawn any more
    }
}

static void crownDraw(int W, int H) {
    if (!C.enabled || C.animT < 0.0f) return;                // zero cost while it is not playing

    float t = C.animT;
    float zoom, alpha, glow, flash, shine;
    animValues(t, &zoom, &alpha, &glow, &flash, &shine);
    float spin = kSpinSpeed * t;

    // placement: relative to the meter center, like the banner (offsets are in meter heights)
    float hPx = kHeightFrac * H;
    float s   = kCrownWidthFrac * H / 210.0f;                // pixels per design unit
    float cx  = kCenterX * W + kCrownDX * hPx;
    float cy  = (1.0f - kCenterY) * H - kCrownDY * hPx;      // GL y goes up
    float mx  = 165.0f * s, myT = 145.0f * s, myB = 120.0f * s;   // keep the drawing area on screen
    if (cx < mx) cx = mx;                    if (cx > W - mx) cx = W - mx;
    if (cy > H - myT) cy = H - myT;          if (cy < myB) cy = myB;

    glViewport((int)floorf(cx - 165.0f * s), (int)floorf(cy - 120.0f * s),
               (int)ceilf(330.0f * s) + 1, (int)ceilf(265.0f * s) + 1);
    glUseProgram(G.prog);
    glUniform2f(G.uOrigin, cx, cy);
    glUniform1f(G.uScale, s);
    glUniform1f(G.uZoom, zoom);
    glUniform1f(G.uSpin, spin);
    glUniform1f(G.uGlow, glow);
    glUniform1f(G.uFlash, flash);
    glUniform1f(G.uShine, shine);
    glUniform1f(G.uTime, t);
    glUniform1f(G.uAlpha, alpha);
    Overlay_BindQuad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Registers the crown with the core when this file is part of the build. It draws after the
// meter (order 0) and the flame (order 5), and before the banner (order 10).
static OverlayModule g_crown = { "crown_animation", crownInit, crownStep, crownDraw, crownOnResult, 7, false };
static OverlayRegistrar g_crownReg(&g_crown);
