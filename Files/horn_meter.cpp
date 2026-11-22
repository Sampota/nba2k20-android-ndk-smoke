/*
 * =====================================================================================
 *  HORN SHOT METER + RESULT BANNER  (horn_meter.cpp)
 * =====================================================================================
 *
 *  WHAT IT IS
 *  ----------
 *  A curved, horn-shaped shot meter (like the NBA 2K shot meter) plus a result banner
 *  ("Excellent", "Slight Early", ...) drawn as an overlay on top of a running Android
 *  game. It is injected with a Dobby hook on eglSwapBuffers, so it is drawn once per
 *  frame, just before the frame is shown.
 *
 *  FILES
 *  -----
 *   horn_meter.cpp        this file (hook, shaders, meter logic, drawing)
 *   horn_font_atlas.h     generated text atlas for the banner labels (keep next to this file)
 *   tools/gen_atlas.py    script that regenerates horn_font_atlas.h (only needed to change
 *                         the label text or font)
 *
 *  WHAT IT LOOKS LIKE
 *  ------------------
 *  METER (center of the screen by default)
 *   - A thin black outline (same thickness on both sides) around a dark-gray body.
 *   - A WHITE fill that travels ALONG THE CURVE from the bottom tip to the top tip.
 *   - A GREEN release zone at the top tip (the "perfect" window).
 *   - After the player releases, the whole horn, zone included, flashes the result color.
 *
 *  BANNER (top right of the meter, like NBA 2K)
 *   - A slanted dark panel with a darker border and a glowing label.
 *   - A neon outline of the same shape, offset down and to the right behind the panel.
 *   - The panel, neon and label all use the result color.
 *   - It pops in, stays for kBannerTime seconds, then fades out.
 *
 *        RESULT            WHEN (fill progress 0..1)          COLOR   BANNER TEXT
 *        ----------------  ---------------------------------  ------  --------------
 *        Very early        below kTooEarly       (< 0.800)    RED     Very Early
 *        Slight early      kTooEarly .. kWinLo   (0.80-0.90)  WHITE   Slight Early
 *        Excellent         kWinLo .. kWinHi      (0.90-0.975) GREEN   Excellent
 *        Slight late       kWinHi .. kSlightLate (0.975-0.995) YELLOW Slight Late
 *        Very late         above kSlightLate     (> 0.995)    RED     Very Late
 *
 *  HOW IT IS DRAWN
 *  ---------------
 *  There are no meshes. Every shape is computed per pixel in a fragment shader from
 *  "signed distance" math, so edges stay smooth at any resolution:
 *     - the horn is a big circle minus a smaller circle (a crescent), trimmed at the top;
 *     - the white fill front is a straight line through the arc's center, rotated to an
 *       angle that depends on the fill progress (that is what makes it follow the curve);
 *     - the green zone is the same kind of line, fixed at the angle of kWinLo;
 *     - the banner is a slanted rectangle (a parallelogram) drawn by a second shader;
 *     - the banner text comes from a small signed-distance-field texture (the atlas), which
 *       gives sharp text and a soft glow from one 40 KB texture.
 *
 *  FILE LAYOUT
 *  -----------
 *   1. Config           : constants you are expected to edit (position, size, timing, zones)
 *   2. Shaders          : vertex shader, horn fragment shader, banner fragment shader
 *   3. State            : GL objects (G) and meter/banner animation state (M)
 *   4. Public API       : functions your own code can call (HornMeter_*)
 *   5. GL helpers       : shader compile, program link, init (shaders, buffers, texture)
 *   6. Meter logic      : classify(), demo loop, banner timing
 *   7. Draw             : saves game GL state, draws horn + banner, restores state
 *   8. Dobby hook       : hook on eglSwapBuffers + library entry point
 *
 *  BUILD (Android NDK, CMake)
 *  --------------------------
 *      add_library(hornmeter SHARED horn_meter.cpp)
 *      target_link_libraries(hornmeter dobby EGL GLESv3 log)
 *  Load the .so into the game process. The constructor at the bottom installs the hook
 *  automatically when the library loads.
 *
 *  USING IT WITH YOUR OWN CODE
 *  ---------------------------
 *  By default it plays a demo loop (red, white, green, yellow, red). To drive it from
 *  the real game:
 *      HornMeter_Set(0.0f .. 1.0f);         // every frame while the shot button is held
 *      int r = HornMeter_Release(progress); // when the shot is released; returns HORN_*,
 *                                           // flashes the horn and shows the banner
 *      HornMeter_Set(-1.0f);                // after the effect, back to the demo loop
 *  Hide the meter entirely (zero cost) with HornMeter_SetVisible(false), and turn only the
 *  banner off with HornMeter_SetBanner(false).
 *
 *  PERFORMANCE NOTES
 *  -----------------
 *   - Everything runs on the game's render thread (GL contexts are thread-bound), so extra
 *     CPU cores do not help. Memory use is about 150 KB, so RAM is not a concern.
 *   - ES3 contexts use a private VAO, so vertex attribute state is not saved/restored.
 *   - Shaders exit early for pixels far from the shapes, the meter uses no atan/length for
 *     the fill, and both passes draw into tight viewports (the banner pass only runs while
 *     the banner is visible).
 *   - The GL state around the draw is saved and restored once per frame to avoid breaking
 *     the game's rendering. If you see frame spikes, the state queries in drawOverlay()
 *     are the first place to look. Set HORN_SHADOW to 0 for a slightly cheaper horn shader.
 *
 *  TROUBLESHOOTING
 *  ---------------
 *   - Nothing appears   : the game may not draw to the surface that eglSwapBuffers presents;
 *                         add a log line in hook_swap to check that the hook runs.
 *   - Flickering/black  : some game state is not being restored; extend the save/restore
 *                         list in drawOverlay() (for example the color mask).
 *   - Banner text wrong : the texture unit 0 binding or a sampler object was changed by the
 *                         game; the code already saves/restores both while the banner draws.
 *   - Wrong position    : change kCenterX / kCenterY / kHeightFrac for the meter, and
 *                         kBannerDX / kBannerDY / kBannerHeightFrac for the banner.
 *   - Compile errors    : on very old GPUs try replacing "highp" with "mediump" in the shaders.
 *
 *  Only use this on apps you own or have permission to modify.
 * =====================================================================================
 */

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "dobby.h"
#include "horn_font_atlas.h"   // kHornAtlas, kHornText, HORN_ATLAS_* (generated)

// =====================================================================================
//  1. CONFIG
// =====================================================================================

#define HORN_SHADOW 1   // 1 = soft drop shadow under the horn, 0 = no shadow (cheaper)

// --- meter placement (fractions of the screen) ---
static const float kHeightFrac = 0.32f;  // icon height relative to screen height
static const float kCenterX    = 0.50f;  // icon center X, 0 = left edge, 1 = right edge
static const float kCenterY    = 0.38f;  // icon center Y, 0 = top edge,  1 = bottom edge

// --- banner placement and timing ---
static const float kBannerHeightFrac = 0.075f; // banner box height relative to screen height
static const float kBannerDX   = 0.75f;  // banner center, right of the meter center, in meter heights
static const float kBannerDY   = -0.35f; // banner center, below(+)/above(-) the meter center, in meter heights
static const float kBannerTime = 1.3f;   // seconds the banner stays (pop-in 0.2 s, fade-out last 0.25 s)

// --- meter timing ---
static const float kFillTime   = 1.3f;   // seconds for the meter to fill from 0 to 1 (demo)

// --- result zones on the fill progress (0 = empty, 1 = full) ---
static const float kTooEarly   = 0.800f; // below this          -> RED    "very early"
static const float kWinLo      = 0.900f; // [kTooEarly, kWinLo) -> WHITE  "slight early"
                                         // KEEP EQUAL to WIN_LO in the shader below
static const float kWinHi      = 0.975f; // [kWinLo, kWinHi]    -> GREEN  "excellent"
static const float kSlightLate = 0.995f; // (kWinHi, this]      -> YELLOW "slight late"
                                         // above this          -> RED    "very late"

// --- geometry of the arc (do not change unless you change the shape) ---
static const float kAngBot     = 1.1135f;   // radians: bottom tip as seen from the arc center
static const float kAngTop     = -0.9931f;  // radians: top tip

// --- result identifiers (returned by HornMeter_Release) ---
// The order also matches the label order in horn_font_atlas.h.
enum HornResult {
    HORN_TOO_EARLY    = 0,   // red
    HORN_SLIGHT_EARLY = 1,   // white
    HORN_EXCELLENT    = 2,   // green
    HORN_SLIGHT_LATE  = 3,   // yellow
    HORN_TOO_LATE     = 4    // red
};

// flash / banner color for each result (r, g, b), same order as HornResult
static const float kTint[5][3] = {
    {0.90f, 0.15f, 0.15f},   // very early   : red
    {1.00f, 1.00f, 1.00f},   // slight early : white
    {0.21f, 0.82f, 0.23f},   // excellent    : green
    {1.00f, 0.85f, 0.10f},   // slight late  : yellow
    {0.90f, 0.15f, 0.15f}    // very late    : red
};

// =====================================================================================
//  2. SHADERS
// =====================================================================================
// Both shaders work in a "design space" (y pointing DOWN) and convert every pixel into it,
// so the shapes scale to any size:
//   horn   : 247 x 319 box
//   banner : 420 x 130 box

// Shared vertex shader: a plain quad. The viewport decides where it is drawn.
static const char* VS = R"(
attribute vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

// ---- horn fragment shader --------------------------------------------------------
static const char* FS_HORN = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec2  uOrigin;              // bottom-left pixel of the icon box
uniform vec2  uDirFill, uDirCap;    // unit vectors: fill front, green zone start
uniform vec3  uTint;                // result flash color (green / white / yellow / red)
uniform float uScale, uFill, uPulse, uFlash, uAlpha;

// circle that forms the outer (right) edge, and the circle cut out for the inner edge
const vec2  OUT_C = vec2(50.0, 143.0);  const float OUT_R = 136.0;
const vec2  IN_C  = vec2(-3.0, 170.0);  const float IN_R  = 147.0;
// slanted line that trims the top of the horn
const vec2  CUT_P = vec2(98.0, 67.0);
const vec2  CUT_N = vec2(-0.823, -0.57);

float W;                                         // half anti-aliasing width, design units
float cov(float x)  { return 1.0 - smoothstep(-W, W, x); }   // 1 inside (x<0), 0 outside
float dOuter(vec2 p){ return length(p - OUT_C) - OUT_R; }
float dInner(vec2 p){ return IN_R - length(p - IN_C); }
float dCut(vec2 p)  { return dot(p - CUT_P, CUT_N); }
float horn(vec2 p)  { return max(max(dOuter(p), dInner(p)), dCut(p)); }  // signed distance

void main() {
    W = 0.8 / uScale;
    vec2 p = (gl_FragCoord.xy - uOrigin) / uScale;   // pixel -> design space
    p.y = 319.0 - p.y;
    vec2 c = vec2(123.5, 159.5);
    p = c + (p - c) / (1.0 + 0.12 * uPulse);         // small "pop" on a perfect release

    float d = horn(p);
    if (d > 20.0) { gl_FragColor = vec4(0.0); return; }   // early exit: far from the horn

    float a    = cov(d);          // whole horn including outline
    float body = cov(d + 3.0);    // inside of the outline (outline is 3 units thick)
    float sh   = 0.0;
#if HORN_SHADOW
    sh = (1.0 - smoothstep(-4.0, 12.0, horn(p - vec2(4.0, 6.0)))) * 0.35;  // drop shadow
#endif

    vec3 ink = vec3(0.04);
    // empty meter: dark gray, tinted by the result color during the flash
    vec3 col = mix(ink, mix(vec3(0.20), uTint, uFlash), body);

    // white fill: signed distance to the fill front, a line through OUT_C (no atan needed)
    vec2  v  = p - OUT_C;
    float fm = cov(-(uDirFill.x * v.y - uDirFill.y * v.x));
    col = mix(col, mix(vec3(1.0), uTint, uFlash), fm * body * step(0.001, uFill));

    // release zone: the last part of the arc (starts at progress WIN_LO).
    // Green normally; during the result flash it takes the result color too, so a very
    // late shot turns the whole horn, zone included, red (white for slight early, yellow for slight late).
    float cap = cov(uDirCap.x * v.y - uDirCap.y * v.x) * body;
    col = mix(col, mix(vec3(0.25, 0.90, 0.20), uTint, uFlash), cap);

    float al = (a + sh * (1.0 - a)) * uAlpha;
    gl_FragColor = vec4(col * a * uAlpha, al);       // premultiplied alpha output
}
)";

// ---- banner fragment shader ------------------------------------------------------
// Layers, back to front: neon outline + glow, dark panel with border, text glow, text.
static const char* FS_BANNER = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec2  uOrigin;            // bottom-left pixel of the banner box
uniform float uScale;             // pixels per design unit
uniform float uAlpha;             // overall fade 0..1
uniform float uPop;               // pop-in scale (about 0.6 .. 1.04 .. 1.0)
uniform float uSdfK;              // converts atlas alpha to design-unit distance
uniform vec3  uTint;              // result color
uniform sampler2D uAtlas;         // signed-distance text atlas (alpha channel)
uniform vec2  uTextOrigin;        // design position of the text cell's top-left corner
uniform vec2  uCellSize;          // design size of the text cell
uniform vec2  uTexV;              // v start and v height of the text row in the atlas

const vec2  C    = vec2(196.0, 54.0);    // panel center
const vec2  HS   = vec2(150.0, 36.0);    // panel half size before slanting
const float SK   = 0.36;                 // slant: sideways shift per unit of height
const float INV  = 0.9409;               // 1 / sqrt(1 + SK*SK), makes slanted edges true distances
const vec2  NEON = vec2(14.0, 14.0);     // neon outline offset (down-right of the panel)

float W;
float cov(float x) { return 1.0 - smoothstep(-W, W, x); }
// signed distance to the slanted panel centered at c (negative inside)
float para(vec2 p, vec2 c) {
    vec2 q = p - c;
    q.x += SK * q.y;                      // un-slant
    return max((abs(q.x) - HS.x) * INV, abs(q.y) - HS.y);
}

void main() {
    W = 0.8 / uScale;
    vec2 p = (gl_FragCoord.xy - uOrigin) / uScale;
    p.y = 130.0 - p.y;
    p = C + (p - C) / uPop;               // pop-in scale around the panel center

    float dP = para(p, C);                // panel
    float dN = para(p, C + NEON);         // neon outline shape
    if (dP > 24.0 && dN > 24.0) { gl_FragColor = vec4(0.0); return; }   // early exit

    // 1) neon outline with glow (behind the panel)
    vec3  neonCol = mix(uTint, vec3(1.0), 0.18);
    float edge = abs(dN) - 1.7;
    float line = cov(edge);
    float glow = exp(-max(edge, 0.0) / 5.0) * 0.55;
    float na   = clamp(line + glow * (1.0 - line), 0.0, 1.0);
    vec4  res  = vec4(neonCol * na, na);

    // 2) dark translucent panel with a darker, more opaque border
    float pc     = cov(dP);
    float border = pc * (1.0 - cov(dP + 3.5));
    vec3  pcol   = mix(uTint * 0.16, uTint * 0.48, border);
    float pa     = pc * mix(0.88, 1.0, border);
    res *= (1.0 - pc);                    // the neon never shows through the panel
    res  = vec4(pcol * pa, pa) + res * (1.0 - pa);

    // 3) text from the signed-distance atlas: glow first, then the letters
    vec2  n = (p - uTextOrigin) / uCellSize;
    float inCell = step(0.0, n.x) * step(n.x, 1.0) * step(0.0, n.y) * step(n.y, 1.0);
    float a = texture2D(uAtlas, vec2(n.x, uTexV.x + n.y * uTexV.y)).a;
    float dist = mix(-100.0, (a - 0.50196) * uSdfK, inCell);   // + inside a letter, - outside
    float tc    = smoothstep(-W, W, dist);                    // letter coverage
    float glowT = exp(min(dist, 0.0) / 4.5) * 0.7 * (1.0 - tc) * pc;
    vec3  gcol  = mix(uTint, vec3(1.0), 0.15);
    vec3  tcol  = mix(uTint, vec3(1.0), 0.40);
    res = vec4(gcol * glowT, glowT) + res * (1.0 - glowT);
    res = vec4(tcol * tc, tc) + res * (1.0 - tc);

    gl_FragColor = res * uAlpha;          // premultiplied alpha output
}
)";

// =====================================================================================
//  3. STATE
// =====================================================================================

// GL objects for the context we are drawing in
static struct {
    EGLContext ctx = EGL_NO_CONTEXT;      // context these objects belong to
    GLuint prog = 0, bprog = 0;           // horn program, banner program
    GLuint vbo = 0, vao = 0, tex = 0;     // quad buffer, private VAO (ES3), text atlas
    // horn uniforms
    GLint uOrigin = -1, uScale = -1, uFill = -1, uPulse = -1, uFlash = -1, uAlpha = -1,
          uDirFill = -1, uDirCap = -1, uTint = -1;
    // banner uniforms
    GLint bOrigin = -1, bScale = -1, bAlpha = -1, bPop = -1, bSdfK = -1, bTint = -1,
          bTextOrigin = -1, bCellSize = -1, bTexV = -1;
    bool ok = false, es3 = false;
} G;

// meter + banner animation state
static struct {
    float fill = 0;                       // current fill progress, 0..1
    float pulse = 0, flash = 0;           // effect strengths, decay to 0
    float tint[3] = {0.21f, 0.82f, 0.23f};// current flash / banner color
    float hold = 0;                       // demo: time left to show the result
    float external = -1.0f;               // >= 0 means your code drives the fill
    int   phase = 0, ti = 0;              // demo state machine
    int   result = -1;                    // last HornResult, shown by the banner
    float bannerT = -1.0f;                // seconds since release; < 0 = banner hidden
    bool  visible = true, showBanner = true;
    double last = 0;                      // last frame time
} M;

// =====================================================================================
//  4. PUBLIC API  (call these from your own code)
// =====================================================================================

// Decide which result a release at this progress gets.
static int classify(float progress) {
    if (progress < kTooEarly)    return HORN_TOO_EARLY;
    if (progress < kWinLo)       return HORN_SLIGHT_EARLY;
    if (progress <= kWinHi)      return HORN_EXCELLENT;
    if (progress <= kSlightLate) return HORN_SLIGHT_LATE;
    return HORN_TOO_LATE;
}

// Drive the fill yourself (0..1). Pass -1 to go back to the built-in demo loop.
extern "C" void HornMeter_Set(float fill01)  { M.external = fill01; }

// Show or hide the whole overlay. Hidden = the hook does nothing and costs nothing.
extern "C" void HornMeter_SetVisible(bool v) { M.visible = v; }

// Show or hide only the result banner (the meter flash still happens).
extern "C" void HornMeter_SetBanner(bool v)  { M.showBanner = v; }

// Trigger the release effect for a known result (HORN_*): flash color, a pulse for an
// excellent release, and the banner.
extern "C" void HornMeter_ReleaseResult(int result) {
    if (result < 0 || result > 4) return;
    M.tint[0] = kTint[result][0];
    M.tint[1] = kTint[result][1];
    M.tint[2] = kTint[result][2];
    M.flash = 1.0f;
    M.pulse = (result == HORN_EXCELLENT) ? 1.0f : 0.0f;
    M.result = result;
    M.bannerT = 0.0f;                     // starts the banner animation
}

// Release at a fill progress: classifies it, plays the effect, returns the HORN_* result.
extern "C" int HornMeter_Release(float progress) {
    int r = classify(progress);
    HornMeter_ReleaseResult(r);
    return r;
}

// =====================================================================================
//  5. GL HELPERS
// =====================================================================================

static GLuint compile(GLenum type, const char* const* src, int n) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, n, src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

// Builds a program from the shared vertex shader and one fragment shader.
static GLuint makeProgram(const char* fsDefines, const char* fsBody) {
    const char* vsrc[1] = { VS };
    const char* fsrc[2] = { fsDefines, fsBody };
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc, 1);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc, 2);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

// Creates programs, buffers and the text atlas once per GL context. Restores whatever the
// game had bound, so it is safe to call in the middle of a frame.
static bool initGL() {
    const char* ver = (const char*)glGetString(GL_VERSION);
    G.es3 = ver && strstr(ver, "OpenGL ES 3");

    G.prog  = makeProgram(HORN_SHADOW ? "#define HORN_SHADOW 1\n" : "#define HORN_SHADOW 0\n", FS_HORN);
    G.bprog = makeProgram("", FS_BANNER);
    if (!G.prog || !G.bprog) return false;

    GLint oldProg; glGetIntegerv(GL_CURRENT_PROGRAM, &oldProg);

    // horn uniforms
    G.uOrigin  = glGetUniformLocation(G.prog, "uOrigin");
    G.uScale   = glGetUniformLocation(G.prog, "uScale");
    G.uFill    = glGetUniformLocation(G.prog, "uFill");
    G.uPulse   = glGetUniformLocation(G.prog, "uPulse");
    G.uFlash   = glGetUniformLocation(G.prog, "uFlash");
    G.uAlpha   = glGetUniformLocation(G.prog, "uAlpha");
    G.uDirFill = glGetUniformLocation(G.prog, "uDirFill");
    G.uDirCap  = glGetUniformLocation(G.prog, "uDirCap");
    G.uTint    = glGetUniformLocation(G.prog, "uTint");
    // uniforms that never change are uploaded once
    glUseProgram(G.prog);
    float capAng = kAngBot + (kAngTop - kAngBot) * kWinLo;
    glUniform2f(G.uDirCap, cosf(capAng), sinf(capAng));
    glUniform1f(G.uAlpha, 1.0f);

    // banner uniforms
    G.bOrigin     = glGetUniformLocation(G.bprog, "uOrigin");
    G.bScale      = glGetUniformLocation(G.bprog, "uScale");
    G.bAlpha      = glGetUniformLocation(G.bprog, "uAlpha");
    G.bPop        = glGetUniformLocation(G.bprog, "uPop");
    G.bSdfK       = glGetUniformLocation(G.bprog, "uSdfK");
    G.bTint       = glGetUniformLocation(G.bprog, "uTint");
    G.bTextOrigin = glGetUniformLocation(G.bprog, "uTextOrigin");
    G.bCellSize   = glGetUniformLocation(G.bprog, "uCellSize");
    G.bTexV       = glGetUniformLocation(G.bprog, "uTexV");
    glUseProgram(G.bprog);
    glUniform1i(glGetUniformLocation(G.bprog, "uAtlas"), 0);   // atlas lives on texture unit 0

    // full-screen-quad vertex buffer (the viewport limits where it actually draws)
    GLint oldBuf, oldVao = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuf);
    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &G.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    if (G.es3) {   // private VAO: attribute setup is captured once and never saved/restored
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
        glGenVertexArrays(1, &G.vao);
        glBindVertexArray(G.vao);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(oldVao);
    }
    glBindBuffer(GL_ARRAY_BUFFER, oldBuf);

    // text atlas: one 8-bit alpha texture holding the five labels as distance fields
    GLint oldActive, oldTex, oldAlign;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign);
    glGenTextures(1, &G.tex);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, HORN_ATLAS_W, HORN_ATLAS_H, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, kHornAtlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);
    glBindTexture(GL_TEXTURE_2D, oldTex);
    glActiveTexture(oldActive);

    glUseProgram(oldProg);
    return true;
}

// =====================================================================================
//  6. METER LOGIC
// =====================================================================================

static double nowSec() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Ease-out with a small overshoot, used for the banner pop-in (u: 0..1 -> 0..1).
static float easeOutBack(float u) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float t = u - 1.0f;
    return 1.0f + c3 * t * t * t + c1 * t * t;
}

// Advances effects, banner timing and the demo loop by the time since the last frame.
static void stepMeter() {
    double t = nowSec();
    float dt = M.last ? (float)(t - M.last) : 0.016f;
    M.last = t;
    if (dt > 0.05f) dt = 0.05f;                 // ignore long pauses (alt-tab, loading)

    M.pulse -= dt / 0.35f; if (M.pulse < 0) M.pulse = 0;
    if (M.bannerT >= 0.0f) {                    // banner clock
        M.bannerT += dt;
        if (M.bannerT > kBannerTime) M.bannerT = -1.0f;
    }
    if (M.external >= 0.0f) {                   // your code drives the fill
        M.fill = M.external;
        M.flash -= dt; if (M.flash < 0) M.flash = 0;
        return;
    }

    // Demo loop: releases at these progress values in order ->
    // very early (red), slight early (white), excellent (green),
    // slight late (yellow), very late (red).
    static const float targets[5] = {0.74f, 0.85f, 0.93f, 0.985f, 1.0f};
    if (M.phase == 0) {                         // phase 0: filling
        M.fill += dt / kFillTime;
        float tgt = targets[M.ti];
        if (M.fill >= tgt) {
            M.fill = tgt;
            HornMeter_Release(tgt);             // classify + flash + banner
            M.phase = 1; M.hold = kBannerTime;
        }
    } else {                                    // phase 1: showing the result
        M.hold -= dt;
        M.flash -= dt; if (M.flash < 0) M.flash = 0;
        if (M.hold <= 0) { M.fill = 0; M.flash = 0; M.phase = 0; M.ti = (M.ti + 1) % 5; }
    }
}

// =====================================================================================
//  7. DRAW
// =====================================================================================

// Binds the quad (private VAO on ES3, plain VBO + attribute on ES2).
static void bindQuad() {
    if (G.es3) {
        glBindVertexArray(G.vao);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
}

// Pass 1: the horn meter.
static void drawHornPass(int W, int H) {
    // icon placement in pixels (design box is 247 x 319)
    float hPx = kHeightFrac * H, s = hPx / 319.0f, wPx = 247.0f * s;
    float bx = kCenterX * W - wPx * 0.5f;
    float by = (1.0f - kCenterY) * H - hPx * 0.5f;   // GL origin is the bottom-left corner

    // Tight viewport around the horn + shadow + pulse (design x 60..220, y 0..300).
    int vx = (int)floorf(bx + 60.0f * s);
    int vy = (int)floorf(by + (319.0f - 300.0f) * s);
    int vw = (int)ceilf(160.0f * s) + 1;
    int vh = (int)ceilf(300.0f * s) + 1;

    // The fill front is a line at this angle around the arc center.
    float th = kAngBot + (kAngTop - kAngBot) * M.fill;

    glViewport(vx, vy, vw, vh);
    glUseProgram(G.prog);
    glUniform2f(G.uOrigin, bx, by);
    glUniform1f(G.uScale, s);
    glUniform1f(G.uFill, M.fill);
    glUniform1f(G.uPulse, M.pulse);
    glUniform1f(G.uFlash, M.flash);
    glUniform3f(G.uTint, M.tint[0], M.tint[1], M.tint[2]);
    glUniform2f(G.uDirFill, cosf(th), sinf(th));
    bindQuad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Pass 2: the result banner (only while it is visible).
static void drawBannerPass(int W, int H) {
    // layout: the banner box is 420 x 130 design units, centered up and to the right of the meter
    float hPx  = kHeightFrac * H;
    float sB   = kBannerHeightFrac * H / 130.0f;           // pixels per design unit
    float bw   = 420.0f * sB, bh = 130.0f * sB;
    float cx   = kCenterX * W + kBannerDX * hPx;
    float cy   = (1.0f - kCenterY) * H - kBannerDY * hPx;  // GL y goes up
    float bx0  = cx - bw * 0.5f, by0 = cy - bh * 0.5f;
    if (bx0 + bw > W) bx0 = W - bw;                        // keep it on screen
    if (by0 + bh > H) by0 = H - bh;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;

    // animation: quick pop-in, hold, fade out over the last 0.25 s
    float bT   = M.bannerT;
    float fade = clamp01(bT / 0.10f) * (1.0f - clamp01((bT - (kBannerTime - 0.25f)) / 0.25f));
    float pop  = 0.6f + 0.4f * easeOutBack(clamp01(bT / 0.20f));

    // text placement: scale so the label fills the panel but never overflows it
    const HornTextRect& tr = kHornText[M.result];
    float sc   = fminf(2.1f, 270.0f / tr.tw);              // design units per atlas pixel
    float cellW = HORN_ATLAS_W * sc, cellH = tr.h * sc;
    float textOx = 196.0f - (HORN_ATLAS_PAD + tr.tw * 0.5f) * sc;   // center text in the panel
    float textOy = 54.0f  - (HORN_ATLAS_PAD + tr.th * 0.5f) * sc;
    float sdfK = (255.0f / 127.0f) * HORN_ATLAS_SPREAD * sc;

    // texture state: save, bind the atlas on unit 0 (and no sampler object), restore afterwards
    GLint oldActive, oldTex, oldSampler = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    if (G.es3) { glGetIntegerv(GL_SAMPLER_BINDING, &oldSampler); glBindSampler(0, 0); }

    glViewport((int)floorf(bx0), (int)floorf(by0), (int)ceilf(bw) + 1, (int)ceilf(bh) + 1);
    glUseProgram(G.bprog);
    glUniform2f(G.bOrigin, bx0, by0);
    glUniform1f(G.bScale, sB);
    glUniform1f(G.bAlpha, fade);
    glUniform1f(G.bPop, pop);
    glUniform1f(G.bSdfK, sdfK);
    glUniform3f(G.bTint, kTint[M.result][0], kTint[M.result][1], kTint[M.result][2]);
    glUniform2f(G.bTextOrigin, textOx, textOy);
    glUniform2f(G.bCellSize, cellW, cellH);
    glUniform2f(G.bTexV, (float)tr.y / HORN_ATLAS_H, (float)tr.h / HORN_ATLAS_H);
    bindQuad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (G.es3) glBindSampler(0, oldSampler);
    glBindTexture(GL_TEXTURE_2D, oldTex);
    glActiveTexture(oldActive);
}

// Saves the GL state we touch once, draws the horn (and the banner if visible), then
// restores everything. This is what keeps the game from flickering or rendering wrongly
// after our draw calls.
static void drawOverlay(int W, int H) {
    bool bannerOn = M.showBanner && M.bannerT >= 0.0f && M.result >= 0;

    // ---- save ----
    GLint prog, vp[4], abuf, vao = 0, bsrc, bdst, bsrcA, bdstA, beq;
    GLint a0en = 0, a0size = 4, a0type = GL_FLOAT, a0norm = 0, a0stride = 0, a0buf = 0;
    void* a0ptr = nullptr;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc);    glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrcA); glGetIntegerv(GL_BLEND_DST_ALPHA, &bdstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq);
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
              scis = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE),
              sten = glIsEnabled(GL_STENCIL_TEST);
    if (G.es3) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    } else {       // ES2 has no VAOs, so attribute 0 has to be saved by hand
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a0en);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0size);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0type);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0norm);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0stride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0buf);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0ptr);
    }

    // ---- common state for both passes ----
    if (depth) glDisable(GL_DEPTH_TEST);
    if (scis)  glDisable(GL_SCISSOR_TEST);
    if (cull)  glDisable(GL_CULL_FACE);
    if (sten)  glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied

    // ---- draw ----
    drawHornPass(W, H);
    if (bannerOn) drawBannerPass(W, H);

    // ---- restore ----
    if (G.es3) {
        glBindVertexArray(vao);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, a0buf);
        glVertexAttribPointer(0, a0size, a0type, a0norm, a0stride, a0ptr);
        if (!a0en) glDisableVertexAttribArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, abuf);
    glUseProgram(prog);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBlendEquation(beq);
    glBlendFuncSeparate(bsrc, bdst, bsrcA, bdstA);
    if (!blend) glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    if (scis)  glEnable(GL_SCISSOR_TEST);
    if (cull)  glEnable(GL_CULL_FACE);
    if (sten)  glEnable(GL_STENCIL_TEST);
}

// =====================================================================================
//  8. DOBBY HOOK
// =====================================================================================

static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface) = nullptr;   // the real eglSwapBuffers

// Runs instead of eglSwapBuffers: draw the overlay, then call the real function.
static EGLBoolean hook_swap(EGLDisplay dpy, EGLSurface surf) {
    if (M.visible) {
        EGLContext ctx = eglGetCurrentContext();
        // only draw on the main window surface of the current context
        if (ctx != EGL_NO_CONTEXT && surf == eglGetCurrentSurface(EGL_DRAW)) {
            if (ctx != G.ctx) { G.ctx = ctx; G.ok = initGL(); }   // new context -> re-init
            if (G.ok) {
                // the surface size rarely changes, so query it every 120 frames
                static int cw = 0, ch = 0, tick = 0;
                if (cw == 0 || (++tick % 120) == 0) {
                    EGLint w = 0, h = 0;
                    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
                    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
                    cw = w; ch = h;
                }
                if (cw > 0 && ch > 0) { stepMeter(); drawOverlay(cw, ch); }
            }
        }
    }
    return orig_swap(dpy, surf);
}

// Library entry point: runs automatically when the .so is loaded and installs the hook.
__attribute__((constructor)) static void horn_init() {
    void* sym = DobbySymbolResolver("libEGL.so", "eglSwapBuffers");
    if (!sym) sym = (void*)eglSwapBuffers;
    DobbyHook(sym, (dobby_dummy_func_t)hook_swap, (dobby_dummy_func_t*)&orig_swap);
}
