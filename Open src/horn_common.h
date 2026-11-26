/*
 * =====================================================================================
 *  horn_common.h : definitions shared by the horn meter and the shot feedback
 * =====================================================================================
 *
 *  This is the only thing the two modules have in common (besides overlay_core).
 *  It holds:
 *    - the five shot results (HornResult) and their colors,
 *    - the timing zones that decide which result a release gets,
 *    - where the meter sits on the screen (the banner is placed relative to it).
 *
 *  Edit the values here to change the feedback for BOTH modules at once.
 */
#pragma once

// --- result identifiers (returned by Shot_Release) ---
// The order also matches the label order in horn_font_atlas.h.
enum HornResult {
    HORN_VERY_EARLY   = 0,   // red
    HORN_SLIGHT_EARLY = 1,   // white
    HORN_EXCELLENT    = 2,   // green
    HORN_SLIGHT_LATE  = 3,   // yellow
    HORN_VERY_LATE    = 4    // red
};

// Color of each result (r, g, b), same order as HornResult.
// Used for the meter flash and for the banner (panel, neon outline and text).
static const float kTint[5][3] = {
    {0.90f, 0.15f, 0.15f},   // very early   : red
    {1.00f, 1.00f, 1.00f},   // slight early : white
    {0.21f, 0.82f, 0.23f},   // excellent    : green
    {1.00f, 0.85f, 0.10f},   // slight late  : yellow
    {0.90f, 0.15f, 0.15f}    // very late    : red
};

// --- result zones on the fill progress (0 = empty, 1 = full) ---
static const float kTooEarly   = 0.800f; // below this          -> RED    "very early"
static const float kWinLo      = 0.900f; // [kTooEarly, kWinLo) -> WHITE  "slight early"
                                         // KEEP EQUAL to WIN_LO in the meter shader
static const float kWinHi      = 0.975f; // [kWinLo, kWinHi]    -> GREEN  "excellent"
static const float kSlightLate = 0.995f; // (kWinHi, this]      -> YELLOW "slight late"
                                         // above this          -> RED    "very late"

// --- meter placement and layout ---
// Which meter is built decides these values: the build defines SWORD_METER when
// sword_meter.cpp is used (see CMakeLists.txt, METER_STYLE), otherwise horn_meter.cpp is meant.
// The banner and the flame are placed relative to the meter using this block.
//   kHeightFrac / kCenterX / kCenterY : meter size and center, as fractions of the screen
//                                       (X: 0 = left edge, Y: 0 = top edge)
//   kMeterBoxW / kMeterBoxH           : size of the meter's design box (the units its shader uses)
//   kZoneAnchorX / kZoneAnchorY       : where the perfect zone is in that box (the flame sits here)
//   kBannerDX / kBannerDY             : banner center relative to the meter center, in meter heights
//   kImpactDelay                      : seconds from release until the "hit" (flame ignites then)
#ifdef SWORD_METER
static const float kHeightFrac  = 0.52f;
static const float kCenterX     = 0.50f;
static const float kCenterY     = 0.50f;
static const float kMeterBoxW   = 180.0f;
static const float kMeterBoxH   = 520.0f;
static const float kZoneAnchorX = 90.0f;     // sword tip
static const float kZoneAnchorY = 22.0f;
static const float kBannerDX    = 0.46f;
static const float kBannerDY    = -0.34f;
static const float kImpactDelay = 0.0f;      // the glow starts right at the release (set above 0 to delay the flame)
#else
static const float kHeightFrac  = 0.32f;
static const float kCenterX     = 0.50f;
static const float kCenterY     = 0.38f;
static const float kMeterBoxW   = 247.0f;
static const float kMeterBoxH   = 319.0f;
static const float kZoneAnchorX = 118.0f;    // green zone at the top tip of the horn
static const float kZoneAnchorY = 40.0f;
static const float kBannerDX    = 0.75f;
static const float kBannerDY    = -0.35f;
static const float kImpactDelay = 0.0f;
#endif

// Decide which result a release at this fill progress gets.
static inline int classify(float progress) {
    if (progress < kTooEarly)    return HORN_VERY_EARLY;
    if (progress < kWinLo)       return HORN_SLIGHT_EARLY;
    if (progress <= kWinHi)      return HORN_EXCELLENT;
    if (progress <= kSlightLate) return HORN_SLIGHT_LATE;
    return HORN_VERY_LATE;
}
