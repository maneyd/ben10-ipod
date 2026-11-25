#pragma once
#include <SDL.h>

namespace resona::ui {

// ============================================================================
// Omnitrix Design System Tokens (v1.0.0)
// Matches omnitrix_design_system_specification.md
// ============================================================================

namespace OmnitrixTheme {
    // Brand Lime Accents
    constexpr SDL_Color kLimePrimary     = {166, 232, 41, 255}; // #A6E829
    constexpr SDL_Color kLimeHover       = {184, 245, 59, 255}; // #B8F53B
    constexpr SDL_Color kLimeMuted       = {74, 82, 72, 255};   // #4A5248 (Paused state)
    constexpr SDL_Color kLimeTrackBg     = {43, 51, 40, 255};   // #2B3328 (Scrubber background)

    // LCD Screen Materials
    constexpr SDL_Color kLcdBg           = {13, 17, 14, 255};   // #0D110E
    constexpr SDL_Color kLcdElevated     = {20, 26, 21, 255};   // #141A15
    constexpr SDL_Color kLcdInput        = {24, 32, 26, 255};   // #18201A
    constexpr SDL_Color kLcdBorder       = {34, 44, 36, 255};   // #222C24
    constexpr SDL_Color kLcdBorderFocus  = {166, 232, 41, 255}; // #A6E829
    constexpr SDL_Color kMiniBorder      = {56, 60, 64, 255};   // #383C40

    // Dial Materials
    constexpr SDL_Color kDialRing        = {38, 41, 43, 255};   // #26292B
    constexpr SDL_Color kDialGunmetal    = {92, 94, 96, 255};   // #5C5E60
    constexpr SDL_Color kDialInner       = {29, 31, 33, 255};   // #1D1F21

    // Text & Icon Materials
    constexpr SDL_Color kTextPrimary     = {242, 245, 243, 255}; // #F2F5F3
    constexpr SDL_Color kTextSecondary   = {142, 150, 144, 255}; // #8E9690
    constexpr SDL_Color kTextTertiary    = {95, 102, 97, 255};   // #5F6661
    constexpr SDL_Color kTextInverse     = {13, 17, 14, 255};    // #0D110E (on Lime)

    // Chassis Themes
    struct ChassisPalette {
        SDL_Color surface;
        SDL_Color border;
        SDL_Color accentPill;
        SDL_Color powerBtn;
        SDL_Color powerIcon;
    };

    constexpr ChassisPalette kDarkChassis = {
        {35, 37, 40, 255},   // #232528
        {82, 86, 92, 255},   // #52565C
        {166, 232, 41, 255}, // #A6E829
        {242, 245, 243, 255},// #F2F5F3 (white circular power button)
        {26, 28, 30, 255}    // #1A1C1E (dark power icon)
    };

    constexpr ChassisPalette kClassicWhiteChassis = {
        {255, 255, 255, 255}, // #FFFFFF
        {220, 224, 228, 255}, // #DCE0E4
        {166, 232, 41, 255},  // #A6E829
        {240, 242, 241, 255}, // #F0F2F1
        {38, 41, 43, 255}     // #26292B
    };

    // Spatial grid & radii (320 x 560 logical canvas)
    constexpr int kCanvasWidth = 320;
    constexpr int kCanvasHeight = 560;
    constexpr int kChassisRadius = 24;
    constexpr int kMiniRadius = 16;
    constexpr int kLcdRadius = 12;
    constexpr int kArtworkRadius = 8;
    constexpr int kWheelDiameter = 240;
    constexpr int kNodeDiameter = 28;
    constexpr int kNodeHitbox = 48;
    constexpr int kGridUnit = 4;

    // Type scale (Space Grotesk / JetBrains Mono)
    constexpr int kTypeHeaderLabel = 10;
    constexpr int kTypeHeaderScreen = 12;
    constexpr int kTypeRowPrimary = 14;
    constexpr int kTypeTitleHero = 16;
    constexpr int kTypeSubRegular = 12;
    constexpr int kTypeMonoTime = 11;
    constexpr int kTypeMicroHint = 10;

    // Dynamic feedback
    constexpr Uint8 kGlowAlpha = 89; // rgba(..., 0.35) * 255
}

} // namespace resona::ui
