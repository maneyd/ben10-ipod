#include <SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "domain/models.hpp"
#include "player/player_controller.hpp"
#include "search/search_service.hpp"
#include "storage/paths.hpp"
#include "ui/ipod_model.hpp"
#include "ui/ipod_theme.hpp"
#ifdef OMNITRIX_HAVE_DBUS
#include "tray/status_notifier.hpp"
#endif

namespace {

// ============================================================================
// Palette & Theme Integration
// ============================================================================

namespace Palette {
    using namespace resona::ui::OmnitrixTheme;

    const SDL_Color kDeskBg        = {13, 17, 14, 255};
    const SDL_Color kDeskGlow      = {20, 26, 21, 255};
    const SDL_Color kPlayerBody    = kDarkChassis.surface;
    const SDL_Color kPlayerBodyBot = {26, 28, 30, 255};
    const SDL_Color kPlayerBevel   = kDarkChassis.border;
    const SDL_Color kPlayerShadow  = {0, 0, 0, 220};
    const SDL_Color kPlayerWhite   = kClassicWhiteChassis.surface;
    const SDL_Color kStripeBlack   = {16, 18, 20, 255};

    const SDL_Color kPlumberTeal   = kLimePrimary;
    const SDL_Color kPlumberBright = kLimeHover;
    const SDL_Color kPlumberAmber  = {245, 158, 11, 255};

    const SDL_Color kDialBlack     = kDialRing;
    const SDL_Color kDialGrey      = kDialGunmetal;
    const SDL_Color kDialMidGrey   = kDialInner;

    const SDL_Color kUiBg          = kLcdBg;
    const SDL_Color kHeaderTop     = kLcdElevated;
    const SDL_Color kHeaderBot     = kLcdBg;
    const SDL_Color kHeaderLine    = kLcdBorder;
    const SDL_Color kLineGrey      = kLcdBorder;
    const SDL_Color kTrackGrey     = kLimeTrackBg;
    const SDL_Color kStatusBg      = kLcdBg;

    const SDL_Color kTextMint      = kTextPrimary;
    const SDL_Color kTextDim       = kTextSecondary;
    const SDL_Color kTextDark      = kTextTertiary;
    const SDL_Color kTextSel       = kTextInverse;
    const SDL_Color kChevron       = kTextSecondary;

    const SDL_Color kGreenYellow   = kLimePrimary;
    const SDL_Color kGreenBright   = kLimeHover;
    const SDL_Color kTextBlack     = kTextInverse;
    const SDL_Color kTextGrey      = kTextSecondary;
    const SDL_Color kTextDarkGrey  = kTextTertiary;

    const SDL_Color kAccents[8] = {
        kLimePrimary,
        kLimeHover,
        {245, 158, 11, 255},
        {249, 115, 22, 255},
        {20, 184, 166, 255},
        {59, 130, 246, 255},
        {168, 85, 247, 255},
        {236, 72, 153, 255},
    };
}

SDL_Color accentColor(int index) {
    return Palette::kAccents[((index % 8) + 8) % 8];
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

// ============================================================================
// Font Rendering Engine (stb_truetype + SDL2)
// ============================================================================

struct Font {
    static constexpr int kAtlasW = 512;
    static constexpr int kAtlasH = 512;
    static constexpr int kFirstChar = 32;
    static constexpr int kCharCount = 96;

    stbtt_bakedchar glyphs[kCharCount];
    SDL_Texture* texture = nullptr;
    int size = 16;
    float ascent = 14.0f;

    bool load(SDL_Renderer* renderer, const std::string& path, int fontSize) {
        size = fontSize;
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) return false;
        std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (ttf.empty()) return false;

        stbtt_fontinfo info;
        if (stbtt_InitFont(&info, ttf.data(), 0)) {
            int a, d, lg;
            stbtt_GetFontVMetrics(&info, &a, &d, &lg);
            float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(fontSize));
            ascent = static_cast<float>(a) * scale;
        } else {
            ascent = fontSize * 0.82f;
        }

        std::vector<unsigned char> mono(kAtlasW * kAtlasH, 0);
        if (stbtt_BakeFontBitmap(ttf.data(), 0, static_cast<float>(fontSize), mono.data(), kAtlasW, kAtlasH,
                                 kFirstChar, kCharCount, glyphs) <= 0) {
            return false;
        }

        std::vector<Uint32> pixels(kAtlasW * kAtlasH);
        for (size_t i = 0; i < mono.size(); ++i) {
            Uint8 a = mono[i];
            pixels[i] = (255u) | (255u << 8) | (255u << 16) | (static_cast<Uint32>(a) << 24);
        }

        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kAtlasW, kAtlasH);
        if (!texture) return false;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(texture, nullptr, pixels.data(), kAtlasW * sizeof(Uint32));
        return true;
    }

    void destroy() {
        if (texture) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
    }

    int width(const std::string& text) const {
        float x = 0;
        for (char c : text) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc < kFirstChar || uc >= kFirstChar + kCharCount) {
                x += size * 0.5f;
                continue;
            }
            x += glyphs[uc - kFirstChar].xadvance;
        }
        return static_cast<int>(std::ceil(x));
    }

    std::string fit(const std::string& text, int maxW) const {
        if (width(text) <= maxW) return text;
        std::string dot = "...";
        int dw = width(dot);
        if (dw >= maxW) return "";
        std::string res;
        for (char c : text) {
            std::string next = res + c;
            if (width(next) + dw > maxW) break;
            res += c;
        }
        return res + dot;
    }

    void draw(SDL_Renderer* renderer, const std::string& text, int x, int y, SDL_Color color) const {
        if (!texture || text.empty()) return;
        float curX = static_cast<float>(x);
        float baseline = static_cast<float>(y) + ascent;
        std::vector<SDL_Vertex> verts;
        verts.reserve(text.size() * 6);

        for (char c : text) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc < kFirstChar || uc >= kFirstChar + kCharCount) {
                curX += size * 0.5f;
                continue;
            }
            stbtt_aligned_quad q;
            stbtt_GetBakedQuad(glyphs, kAtlasW, kAtlasH, uc - kFirstChar, &curX, &baseline, &q, 1);

            SDL_Vertex v0{SDL_FPoint{q.x0, q.y0}, color, SDL_FPoint{q.s0, q.t0}};
            SDL_Vertex v1{SDL_FPoint{q.x1, q.y0}, color, SDL_FPoint{q.s1, q.t0}};
            SDL_Vertex v2{SDL_FPoint{q.x1, q.y1}, color, SDL_FPoint{q.s1, q.t1}};
            SDL_Vertex v3{SDL_FPoint{q.x0, q.y1}, color, SDL_FPoint{q.s0, q.t1}};
            verts.push_back(v0);
            verts.push_back(v1);
            verts.push_back(v2);
            verts.push_back(v0);
            verts.push_back(v2);
            verts.push_back(v3);
        }
        if (!verts.empty()) {
            SDL_RenderGeometry(renderer, texture, verts.data(), static_cast<int>(verts.size()), nullptr, 0);
        }
    }
};

struct FontSet {
    Font regular; // 13px
    Font bold;    // 14px
    Font large;   // 16px
    Font small;   // 11px
    Font mono;    // 11px monospace

    bool load(SDL_Renderer* renderer) {
        std::vector<std::string> sansRegPaths = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "DejaVuSans.ttf"
        };
        std::vector<std::string> sansBoldPaths = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
            "DejaVuSans-Bold.ttf"
        };
        std::vector<std::string> monoPaths = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "DejaVuSansMono.ttf"
        };

        std::string regPath, boldPath, monoPath;
        for (const auto& p : sansRegPaths) {
            std::ifstream test(p);
            if (test.good()) { regPath = p; break; }
        }
        for (const auto& p : sansBoldPaths) {
            std::ifstream test(p);
            if (test.good()) { boldPath = p; break; }
        }
        for (const auto& p : monoPaths) {
            std::ifstream test(p);
            if (test.good()) { monoPath = p; break; }
        }

        if (regPath.empty()) regPath = boldPath;
        if (boldPath.empty()) boldPath = regPath;
        if (monoPath.empty()) monoPath = regPath;
        if (regPath.empty()) return false;

        bool ok = true;
        ok &= regular.load(renderer, regPath, 13);
        ok &= bold.load(renderer, boldPath, 14);
        ok &= large.load(renderer, boldPath, 16);
        ok &= small.load(renderer, regPath, 11);
        ok &= mono.load(renderer, monoPath, 11);
        return ok;
    }

    void destroy() {
        regular.destroy();
        bold.destroy();
        large.destroy();
        small.destroy();
        mono.destroy();
    }
};

// ============================================================================
// Primitives & Vector Helpers
// ============================================================================

void drawGradientV(SDL_Renderer* renderer, SDL_Rect rect, SDL_Color top, SDL_Color bot) {
    if (rect.w <= 0 || rect.h <= 0) return;
    SDL_Vertex verts[4];
    verts[0] = {SDL_FPoint{static_cast<float>(rect.x), static_cast<float>(rect.y)}, top, SDL_FPoint{0, 0}};
    verts[1] = {SDL_FPoint{static_cast<float>(rect.x + rect.w), static_cast<float>(rect.y)}, top, SDL_FPoint{0, 0}};
    verts[2] = {SDL_FPoint{static_cast<float>(rect.x + rect.w), static_cast<float>(rect.y + rect.h)}, bot, SDL_FPoint{0, 0}};
    verts[3] = {SDL_FPoint{static_cast<float>(rect.x), static_cast<float>(rect.y + rect.h)}, bot, SDL_FPoint{0, 0}};
    int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, verts, 4, indices, 6);
}

void fillCircle(SDL_Renderer* renderer, int cx, int cy, int radius, SDL_Color color) {
    if (radius <= 0) return;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int w = 0; w <= radius * 2; ++w) {
        for (int h = 0; h <= radius * 2; ++h) {
            int dx = radius - w;
            int dy = radius - h;
            if ((dx * dx + dy * dy) <= (radius * radius)) {
                SDL_RenderDrawPoint(renderer, cx + dx, cy + dy);
            }
        }
    }
}

void fillCircleAA(SDL_Renderer* renderer, float cx, float cy, float radius, SDL_Color color) {
    if (radius <= 0.0f) return;
    constexpr int kSegments = 48;
    constexpr float kTwoPi = 6.28318530717958647692f;
    std::vector<SDL_Vertex> verts;
    verts.reserve(kSegments + 2);
    verts.push_back({SDL_FPoint{cx, cy}, color, SDL_FPoint{0, 0}});

    for (int i = 0; i <= kSegments; ++i) {
        float angle = i * kTwoPi / kSegments;
        verts.push_back({SDL_FPoint{cx + radius * std::cos(angle), cy + radius * std::sin(angle)}, color, SDL_FPoint{0, 0}});
    }

    std::vector<int> indices;
    indices.reserve(kSegments * 3);
    for (int i = 0; i < kSegments; ++i) {
        indices.push_back(0);
        indices.push_back(i + 1);
        indices.push_back(i + 2);
    }
    SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()), indices.data(), static_cast<int>(indices.size()));
}

void fillCircleGradientAA(SDL_Renderer* renderer, float cx, float cy, float radius, SDL_Color center, SDL_Color edge) {
    if (radius <= 0.0f) return;
    constexpr int kSegments = 48;
    constexpr float kTwoPi = 6.28318530717958647692f;
    std::vector<SDL_Vertex> verts;
    verts.reserve(kSegments + 2);
    verts.push_back({SDL_FPoint{cx, cy}, center, SDL_FPoint{0, 0}});

    for (int i = 0; i <= kSegments; ++i) {
        float angle = i * kTwoPi / kSegments;
        verts.push_back({SDL_FPoint{cx + radius * std::cos(angle), cy + radius * std::sin(angle)}, edge, SDL_FPoint{0, 0}});
    }

    std::vector<int> indices;
    indices.reserve(kSegments * 3);
    for (int i = 0; i < kSegments; ++i) {
        indices.push_back(0);
        indices.push_back(i + 1);
        indices.push_back(i + 2);
    }
    SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()), indices.data(), static_cast<int>(indices.size()));
}

void drawRingAA(SDL_Renderer* renderer, float cx, float cy, float rInner, float rOuter, SDL_Color color) {
    if (rOuter <= rInner || rInner < 0.0f) return;
    constexpr int kSegments = 48;
    constexpr float kTwoPi = 6.28318530717958647692f;
    std::vector<SDL_Vertex> verts;
    verts.reserve((kSegments + 1) * 2);
    std::vector<int> indices;
    indices.reserve(kSegments * 6);

    for (int i = 0; i <= kSegments; ++i) {
        float angle = i * kTwoPi / kSegments;
        float cosA = std::cos(angle);
        float sinA = std::sin(angle);
        verts.push_back({SDL_FPoint{cx + rInner * cosA, cy + rInner * sinA}, color, SDL_FPoint{0, 0}});
        verts.push_back({SDL_FPoint{cx + rOuter * cosA, cy + rOuter * sinA}, color, SDL_FPoint{0, 0}});
    }

    for (int i = 0; i < kSegments; ++i) {
        int i0 = i * 2;
        int o0 = i * 2 + 1;
        int i1 = (i + 1) * 2;
        int o1 = (i + 1) * 2 + 1;

        indices.push_back(i0);
        indices.push_back(o0);
        indices.push_back(i1);

        indices.push_back(i1);
        indices.push_back(o0);
        indices.push_back(o1);
    }

    SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

void fillArcWedgeAA(SDL_Renderer* renderer, float cx, float cy, float rInner, float rOuter,
                    float startAngleRad, float endAngleRad, SDL_Color color) {
    if (rOuter <= rInner) return;
    constexpr int kSteps = 24;
    std::vector<SDL_Vertex> verts;
    verts.reserve((kSteps + 1) * 2);
    std::vector<int> indices;
    indices.reserve(kSteps * 6);

    for (int i = 0; i <= kSteps; ++i) {
        float t = static_cast<float>(i) / kSteps;
        float angle = startAngleRad + t * (endAngleRad - startAngleRad);
        float cosA = std::cos(angle);
        float sinA = std::sin(angle);
        verts.push_back({SDL_FPoint{cx + rInner * cosA, cy + rInner * sinA}, color, SDL_FPoint{0, 0}});
        verts.push_back({SDL_FPoint{cx + rOuter * cosA, cy + rOuter * sinA}, color, SDL_FPoint{0, 0}});
    }

    for (int i = 0; i < kSteps; ++i) {
        int i0 = i * 2;
        int o0 = i * 2 + 1;
        int i1 = (i + 1) * 2;
        int o1 = (i + 1) * 2 + 1;

        indices.push_back(i0);
        indices.push_back(o0);
        indices.push_back(i1);

        indices.push_back(i1);
        indices.push_back(o0);
        indices.push_back(o1);
    }

    SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

void fillRoundRect(SDL_Renderer* renderer, SDL_Rect rect, int radius, SDL_Color color) {
    if (rect.w <= 0 || rect.h <= 0) return;
    if (radius <= 0) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &rect);
        return;
    }
    const float r = static_cast<float>(std::min({radius, rect.w / 2, rect.h / 2}));
    const float x0 = static_cast<float>(rect.x);
    const float y0 = static_cast<float>(rect.y);
    const float x1 = static_cast<float>(rect.x + rect.w);
    const float y1 = static_cast<float>(rect.y + rect.h);
    const float fcx = (x0 + x1) * 0.5f;
    const float fcy = (y0 + y1) * 0.5f;

    constexpr int kArcSteps = 10;
    constexpr float kHalfPi = 1.57079632679489661923f;
    const float cornerCX[4] = {x1 - r, x0 + r, x0 + r, x1 - r};
    const float cornerCY[4] = {y1 - r, y1 - r, y0 + r, y0 + r};
    const float cornerStart[4] = {0.0f, kHalfPi, 2.0f * kHalfPi, 3.0f * kHalfPi};

    std::vector<SDL_Vertex> verts;
    verts.reserve(4 * (kArcSteps + 1) + 2);
    verts.push_back({SDL_FPoint{fcx, fcy}, color, SDL_FPoint{0, 0}});
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= kArcSteps; ++i) {
            const float a = cornerStart[c] + (kHalfPi * i) / kArcSteps;
            verts.push_back({SDL_FPoint{cornerCX[c] + r * std::cos(a), cornerCY[c] + r * std::sin(a)},
                             color, SDL_FPoint{0, 0}});
        }
    }

    const int ringCount = static_cast<int>(verts.size()) - 1;
    std::vector<int> indices;
    indices.reserve(ringCount * 3);
    for (int i = 0; i < ringCount; ++i) {
        indices.push_back(0);
        indices.push_back(1 + i);
        indices.push_back(1 + ((i + 1) % ringCount));
    }
    SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

void fillRoundRectWithBorder(SDL_Renderer* renderer, SDL_Rect rect, int radius,
                             SDL_Color fillColor, SDL_Color borderColor, int borderW = 1) {
    fillRoundRect(renderer, rect, radius, borderColor);
    SDL_Rect inner{rect.x + borderW, rect.y + borderW,
                   std::max(0, rect.w - 2 * borderW), std::max(0, rect.h - 2 * borderW)};
    fillRoundRect(renderer, inner, std::max(0, radius - borderW), fillColor);
}

void drawPlayIcon(SDL_Renderer* renderer, int x, int y, int size, SDL_Color color) {
    SDL_Vertex verts[3];
    verts[0] = {SDL_FPoint{static_cast<float>(x), static_cast<float>(y)}, color, SDL_FPoint{0, 0}};
    verts[1] = {SDL_FPoint{static_cast<float>(x + size), static_cast<float>(y + size / 2.0f)}, color, SDL_FPoint{0, 0}};
    verts[2] = {SDL_FPoint{static_cast<float>(x), static_cast<float>(y + size)}, color, SDL_FPoint{0, 0}};
    SDL_RenderGeometry(renderer, nullptr, verts, 3, nullptr, 0);
}

void drawPauseIcon(SDL_Renderer* renderer, int x, int y, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int barW = std::max(2, h / 3);
    SDL_Rect r1{x, y, barW, h};
    SDL_Rect r2{x + barW + 3, y, barW, h};
    SDL_RenderFillRect(renderer, &r1);
    SDL_RenderFillRect(renderer, &r2);
}

// Vector rendering of the iconic Omnitrix Dial Hourglass (matches reference images)
void drawOmnitrixDial(SDL_Renderer* renderer, int cx, int cy, int radius, bool isPlaying) {
    (void)isPlaying;
    if (radius <= 4) return;
    float r = static_cast<float>(radius);
    float fcx = static_cast<float>(cx);
    float fcy = static_cast<float>(cy);

    // Deep LCD core (#0D110E)
    fillCircleAA(renderer, fcx, fcy, r, Palette::kUiBg);

    SDL_Color coreColor = resona::ui::OmnitrixTheme::kLimePrimary;
    float embR = r * 0.92f;
    float waistW = embR * 0.12f;
    float waistH = embR * 0.06f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int kSlices = 28;

    // Soft radial lime glow behind the hourglass
    fillCircleGradientAA(renderer, fcx, fcy, embR * 0.98f,
                         SDL_Color{coreColor.r, coreColor.g, coreColor.b, 55},
                         SDL_Color{coreColor.r, coreColor.g, coreColor.b, 0});

    // Draw top and bottom hourglass halves with a clean flat-sided waist bridge
    auto drawHalfHourglass = [&](float aStart, float aEnd, float leftX, float rightX, float waistY) {
        std::vector<SDL_Vertex> verts;
        verts.reserve(kSlices + 4);
        verts.push_back({SDL_FPoint{fcx, waistY}, coreColor, SDL_FPoint{0, 0}});
        for (int i = 0; i <= kSlices; ++i) {
            float a = aStart + i * (aEnd - aStart) / kSlices;
            verts.push_back({SDL_FPoint{fcx + embR * std::cos(a), fcy + embR * std::sin(a)},
                             coreColor, SDL_FPoint{0, 0}});
        }
        int idxWL = static_cast<int>(verts.size());
        verts.push_back({SDL_FPoint{leftX, waistY}, coreColor, SDL_FPoint{0, 0}});
        int idxWR = static_cast<int>(verts.size());
        verts.push_back({SDL_FPoint{rightX, waistY}, coreColor, SDL_FPoint{0, 0}});

        std::vector<int> indices;
        indices.reserve(kSlices * 3 + 6);
        for (int i = 0; i < kSlices; ++i) {
            indices.push_back(0);
            indices.push_back(i + 1);
            indices.push_back(i + 2);
        }
        indices.push_back(idxWL);
        indices.push_back(1);
        indices.push_back(0);

        indices.push_back(idxWR);
        indices.push_back(0);
        indices.push_back(kSlices + 1);

        SDL_RenderGeometry(renderer, nullptr, verts.data(), static_cast<int>(verts.size()),
                           indices.data(), static_cast<int>(indices.size()));
    };

    // Center waist bridge rectangle
    SDL_Vertex waistVerts[4] = {
        {SDL_FPoint{fcx - waistW, fcy - waistH}, coreColor, SDL_FPoint{0, 0}},
        {SDL_FPoint{fcx + waistW, fcy - waistH}, coreColor, SDL_FPoint{0, 0}},
        {SDL_FPoint{fcx + waistW, fcy + waistH}, coreColor, SDL_FPoint{0, 0}},
        {SDL_FPoint{fcx - waistW, fcy + waistH}, coreColor, SDL_FPoint{0, 0}},
    };
    int waistIdx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, waistVerts, 4, waistIdx, 6);

    // Top half: -130 deg (top-left) to -50 deg (top-right)
    drawHalfHourglass(-kPi * 130.0f / 180.0f, -kPi * 50.0f / 180.0f, fcx - waistW, fcx + waistW, fcy - waistH);
    // Bottom half: +50 deg (bottom-right) to +130 deg (bottom-left)
    drawHalfHourglass(kPi * 50.0f / 180.0f, kPi * 130.0f / 180.0f, fcx + waistW, fcx - waistW, fcy + waistH);
}

// Flat-top/flat-bottom solid hourglass for Mini-Player artwork (matches reference)
void drawMiniHourglass(SDL_Renderer* renderer, int cx, int cy, int halfW, int halfH, bool isPlaying) {
    SDL_Color col = isPlaying ? resona::ui::OmnitrixTheme::kLimePrimary
                              : resona::ui::OmnitrixTheme::kLimeMuted;
    float fcx = static_cast<float>(cx);
    float fcy = static_cast<float>(cy);
    float hw = static_cast<float>(halfW);
    float hh = static_cast<float>(halfH);
    float ww = hw * 0.28f;

    SDL_Vertex verts[6] = {
        {SDL_FPoint{fcx - hw, fcy - hh}, col, SDL_FPoint{0, 0}}, // 0: top-left
        {SDL_FPoint{fcx + hw, fcy - hh}, col, SDL_FPoint{0, 0}}, // 1: top-right
        {SDL_FPoint{fcx - ww, fcy},      col, SDL_FPoint{0, 0}}, // 2: waist-left
        {SDL_FPoint{fcx + ww, fcy},      col, SDL_FPoint{0, 0}}, // 3: waist-right
        {SDL_FPoint{fcx - hw, fcy + hh}, col, SDL_FPoint{0, 0}}, // 4: bot-left
        {SDL_FPoint{fcx + hw, fcy + hh}, col, SDL_FPoint{0, 0}}, // 5: bot-right
    };
    int indices[12] = {
        0, 1, 2,  1, 3, 2, // top trapezoid
        2, 3, 4,  3, 5, 4  // bottom trapezoid
    };
    SDL_RenderGeometry(renderer, nullptr, verts, 6, indices, 12);
}

// Outlined circular Omnitrix emblem for Now Playing artwork (matches reference)
void drawOutlinedHourglass(SDL_Renderer* renderer, int cx, int cy, int radius, SDL_Color col) {
    float fcx = static_cast<float>(cx);
    float fcy = static_cast<float>(cy);
    float r = static_cast<float>(radius);
    drawRingAA(renderer, fcx, fcy, r - 2.0f, r, col);

    float hw = r * 0.56f;
    float hh = r * 0.72f;
    float ww = r * 0.16f;
    SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, col.a);
    // Top horizontal bar
    SDL_RenderDrawLine(renderer, static_cast<int>(fcx - hw), static_cast<int>(fcy - hh),
                       static_cast<int>(fcx + hw), static_cast<int>(fcy - hh));
    // Bottom horizontal bar
    SDL_RenderDrawLine(renderer, static_cast<int>(fcx - hw), static_cast<int>(fcy + hh),
                       static_cast<int>(fcx + hw), static_cast<int>(fcy + hh));
    // Left hourglass contour
    SDL_RenderDrawLine(renderer, static_cast<int>(fcx - hw), static_cast<int>(fcy - hh),
                       static_cast<int>(fcx - ww), static_cast<int>(fcy));
    SDL_RenderDrawLine(renderer, static_cast<int>(fcx - ww), static_cast<int>(fcy),
                       static_cast<int>(fcx - hw), static_cast<int>(fcy + hh));
    // Right hourglass contour
    SDL_RenderDrawLine(renderer, static_cast<int>(fcx + hw), static_cast<int>(fcy - hh),
                       static_cast<int>(fcx + ww), static_cast<int>(fcy));
    SDL_RenderDrawLine(renderer, static_cast<int>(fcx + ww), static_cast<int>(fcy),
                       static_cast<int>(fcx + hw), static_cast<int>(fcy + hh));
}

// ============================================================================
// Layout and UI State Definitions
// ============================================================================

enum class Screen {
    Boot,
    Menu,
    Home,
    Playlists,
    NowPlaying,
    Tracks,
    Search
};

enum class TrackListType {
    Local,
    Playlist,
    Queue,
    Liked,
    Downloads,
    SearchResults
};

enum class WheelRegion {
    None,
    Center,
    Menu,
    Next,
    PlayPause,
    Previous,
    InnerUp,
    InnerDown,
    InnerEnter,
    InnerPrev,
};

enum class ChassisTheme {
    Dark,
    ClassicWhite
};

struct DeviceLayout {
    int winW = 540;
    int winH = 860;

    SDL_Rect body;
    SDL_Rect screen;
    SDL_Rect titleBar;
    SDL_Rect contentArea;
    SDL_Rect statusArea;

    SDL_Rect accentPill;
    SDL_Rect powerButton;
    SDL_Rect titleSearchButton;
    SDL_Rect titlePlayButton;
    SDL_Rect minimizeButton;

    int wheelCX = 0;
    int wheelCY = 0;
    int outerRadius = 120;
    int innerRadius = 78;
    int centerRadius = 50;
};

DeviceLayout computeLayout(int width, int height) {
    (void)width;
    (void)height;
    using namespace resona::ui::OmnitrixTheme;

    DeviceLayout layout;
    layout.winW = kCanvasWidth;
    layout.winH = kCanvasHeight;
    layout.body = {0, 0, kCanvasWidth, kCanvasHeight};

    layout.accentPill = {16, 14, 48, 4};
    layout.powerButton = {kCanvasWidth - 40, 8, 24, 24};

    layout.screen = {16, 36, 288, 208};
    layout.titleBar = {layout.screen.x, layout.screen.y, layout.screen.w, 28};
    layout.minimizeButton = {layout.titleBar.x + layout.titleBar.w - 24, layout.titleBar.y, 24, 24};
    layout.titlePlayButton = {layout.titleBar.x + layout.titleBar.w - 46, layout.titleBar.y, 22, 24};
    layout.titleSearchButton = {layout.titleBar.x + layout.titleBar.w - 68, layout.titleBar.y, 22, 24};

    layout.statusArea = {layout.screen.x, layout.screen.y + layout.screen.h - 1, layout.screen.w, 1};
    layout.contentArea = {layout.screen.x, layout.titleBar.y + layout.titleBar.h, layout.screen.w,
                          layout.screen.h - layout.titleBar.h};

    const int remainingH = layout.body.y + layout.body.h - (layout.screen.y + layout.screen.h);
    layout.outerRadius = 116;
    layout.innerRadius = 76;
    layout.centerRadius = 62;
    layout.wheelCX = layout.body.x + layout.body.w / 2;
    layout.wheelCY = layout.screen.y + layout.screen.h + remainingH / 2;
    return layout;
}

constexpr int kHomeRowCount = 4;
constexpr int kSearchVisibleRows = 3;

SDL_Rect homeRowRect(const DeviceLayout& layout, int index) {
    constexpr int heights[kHomeRowCount] = {34, 28, 28, 28};
    constexpr int offsets[kHomeRowCount] = {6, 48, 82, 116};
    const int indexClamped = std::clamp(index, 0, kHomeRowCount - 1);
    return {layout.contentArea.x + 8, layout.contentArea.y + offsets[indexClamped],
            layout.contentArea.w - 16, heights[indexClamped]};
}

int homeRowAt(int x, int y, const DeviceLayout& layout) {
    for (int i = 0; i < kHomeRowCount; ++i) {
        const SDL_Rect row = homeRowRect(layout, i);
        if (x >= row.x && x <= row.x + row.w && y >= row.y && y <= row.y + row.h) return i;
    }
    return -1;
}

SDL_Rect nowPlayingActionRect(const DeviceLayout& layout, int index) {
    constexpr int kSize = 22;
    constexpr int kStride = 34;
    return {layout.contentArea.x + 88 + index * kStride, layout.contentArea.y + 86, kSize, kSize};
}

struct UiState {
    Screen screen = Screen::Home;
    ChassisTheme chassisTheme = ChassisTheme::Dark;
    bool isMiniPlayer = false;
    bool isTrayMode = false;
    bool demoMode = false;
    bool popupOpen = false;
    bool nativeTrayAvailable = false;
    int popupX = 0;
    int popupY = 0;
    resona::search::SearchService* searchService = nullptr;

    TrackListType listType = TrackListType::Local;
    std::string listTitle = "LIBRARY";
    int menuFirstVisible = 0;
    Uint32 bootUntil = 0;

    int menuSelected = 0;
    int homeSelected = 0;
    int trackSelected = 1; // "Cosmic drift" selected by default
    int firstVisibleTrack = 0;

    std::string searchQuery;
    std::vector<resona::Track> searchResults;
    std::vector<resona::Track> localTracks;
    std::vector<resona::Track> likedTracks;
    std::vector<resona::Track> downloads;
    std::vector<resona::Playlist> playlists;
    std::vector<resona::Track> playlistTracks;
    std::string activePlaylistId;

    resona::PlayerSnapshot status;
    float volume = 1.0f;
    std::string statusMessage = "Omnitrix Active | Core Online";
    Uint32 statusMessageExpiry = 0;

    Uint32 trackChangeUntil = 0;
    char trackChangeDir = 0;
    std::string trackChangeName;
    Uint32 dnaScanUntil = 0;
    std::string dnaScanTitle;

    WheelRegion pressedRegion = WheelRegion::None;
    bool isDraggingWheel = false;
    double lastWheelAngle = 0.0;
    double accumulatedWheelAngle = 0.0;
    double sweepAngle = 0.0;
    Uint32 sweepUntil = 0;
    Uint32 rippleUntil = 0;
    WheelRegion rippleZone = WheelRegion::None;
    double scrollAccumulator = 0.0;
    bool wheelDidRotate = false;

    Uint32 lastBlinkTime = 0;
    bool cursorVisible = true;
};

void setStatus(UiState& state, const std::string& message) {
    state.statusMessage = message;
    state.statusMessageExpiry = SDL_GetTicks() + 3500;
}

const std::vector<resona::Track>& getTracksForType(const UiState& state);

void updateSearchScroll(UiState& state, bool allowLoadMore) {
    if (state.screen != Screen::Search) return;
    const int count = static_cast<int>(state.searchResults.size());
    if (count <= 0) {
        state.firstVisibleTrack = 0;
        state.trackSelected = 0;
        return;
    }
    state.firstVisibleTrack = std::clamp(state.firstVisibleTrack, 0,
                                         std::max(0, count - kSearchVisibleRows));
    state.trackSelected = std::clamp(state.trackSelected, state.firstVisibleTrack,
                                     std::min(count - 1, state.firstVisibleTrack + kSearchVisibleRows - 1));
    if (allowLoadMore && state.searchService &&
        state.firstVisibleTrack >= count - kSearchVisibleRows &&
        !state.searchService->state().continuationToken.empty()) {
        state.searchService->loadMore();
    }
}

// ============================================================================
// Screen Rendering Implementations
// ============================================================================

void drawDeviceChassis(SDL_Renderer* renderer, const DeviceLayout& layout,
                       WheelRegion pressedRegion, bool isDraggingWheel, double wheelAngle,
                       double sweepAngle, Uint32 sweepUntil, WheelRegion rippleZone,
                       SDL_Texture* omniDialTexture, UiState& state) {
    (void)wheelAngle;
    (void)omniDialTexture;

    // 1. Ambient glow vignette behind chassis
    float centerDeskX = layout.body.x + layout.body.w * 0.5f;
    float centerDeskY = layout.body.y + layout.body.h * 0.5f;
    fillCircleGradientAA(renderer, centerDeskX, centerDeskY, layout.body.w * 0.95f,
                         Palette::kDeskGlow, Palette::kDeskBg);

    // 2. Chassis drop shadow
    fillRoundRect(renderer, SDL_Rect{layout.body.x + 6, layout.body.y + 12, layout.body.w, layout.body.h},
                  24, Palette::kPlayerShadow);

    // 3. Chassis outer surface based on theme
    const auto& chassisPal = (state.chassisTheme == ChassisTheme::Dark)
                                 ? resona::ui::OmnitrixTheme::kDarkChassis
                                 : resona::ui::OmnitrixTheme::kClassicWhiteChassis;

    fillRoundRect(renderer, layout.body, 24, chassisPal.border);
    SDL_Rect innerBody{layout.body.x + 1, layout.body.y + 1, layout.body.w - 2, layout.body.h - 2};
    fillRoundRect(renderer, innerBody, 23, chassisPal.surface);

    // 4. Top-left signature accent pill
    fillRoundRect(renderer, layout.accentPill, 2, chassisPal.accentPill);

    // 5. Top-right circular power button
    fillCircleAA(renderer, layout.powerButton.x + layout.powerButton.w / 2.0f,
                 layout.powerButton.y + layout.powerButton.h / 2.0f,
                 layout.powerButton.w / 2.0f, chassisPal.powerBtn);
    drawRingAA(renderer, layout.powerButton.x + layout.powerButton.w / 2.0f,
               layout.powerButton.y + layout.powerButton.h / 2.0f,
               layout.powerButton.w / 2.0f - 1.0f, layout.powerButton.w / 2.0f,
               chassisPal.border);

    // Power icon inside button
    float pCX = layout.powerButton.x + layout.powerButton.w / 2.0f;
    float pCY = layout.powerButton.y + layout.powerButton.h / 2.0f;
    float pR = layout.powerButton.w * 0.28f;
    drawRingAA(renderer, pCX, pCY, pR - 1.0f, pR, chassisPal.powerIcon);
    SDL_SetRenderDrawColor(renderer, chassisPal.powerIcon.r, chassisPal.powerIcon.g, chassisPal.powerIcon.b, 255);
    SDL_RenderDrawLine(renderer, static_cast<int>(pCX), static_cast<int>(pCY - pR - 1.0f),
                       static_cast<int>(pCX), static_cast<int>(pCY));

    // 6. Polished LCD Bezel & Screen Viewport (288 x 228)
    SDL_Rect bezelRect{layout.screen.x - 1, layout.screen.y - 1, layout.screen.w + 2, layout.screen.h + 2};
    fillRoundRectWithBorder(renderer, bezelRect, 12,
                            resona::ui::OmnitrixTheme::kLcdBg,
                            resona::ui::OmnitrixTheme::kLcdBorder, 1);

    // 7. Click Wheel System
    int cx = layout.wheelCX;
    int cy = layout.wheelCY;
    int rOuter = layout.outerRadius;
    int rInner = layout.innerRadius;
    int rCenter = layout.centerRadius;

    // Dark slate rotary track (#26292B) with subtle outer rim
    fillCircleAA(renderer, cx, cy, static_cast<float>(rOuter), resona::ui::OmnitrixTheme::kDialRing);
    drawRingAA(renderer, cx, cy, static_cast<float>(rOuter) - 1.0f, static_cast<float>(rOuter),
               SDL_Color{58, 62, 66, 180});

    // Segmented Ring between rCenter and rInner:
    // Top & bottom arcs are deep black (#0D110E), left & right arcs are gunmetal (#5C5E60)
    float pi = 3.14159265358979323846f;
    fillArcWedgeAA(renderer, cx, cy, static_cast<float>(rCenter), static_cast<float>(rInner),
                   -pi * 0.75f, -pi * 0.25f, resona::ui::OmnitrixTheme::kLcdBg);
    fillArcWedgeAA(renderer, cx, cy, static_cast<float>(rCenter), static_cast<float>(rInner),
                   pi * 0.25f, pi * 0.75f, resona::ui::OmnitrixTheme::kLcdBg);
    fillArcWedgeAA(renderer, cx, cy, static_cast<float>(rCenter), static_cast<float>(rInner),
                   -pi * 0.25f, pi * 0.25f, resona::ui::OmnitrixTheme::kDialGunmetal);
    fillArcWedgeAA(renderer, cx, cy, static_cast<float>(rCenter), static_cast<float>(rInner),
                   pi * 0.75f, pi * 1.25f, resona::ui::OmnitrixTheme::kDialGunmetal);

    // Drag sweep indicator (subtle dark)
    if (isDraggingWheel) {
        float touchX = cx + static_cast<float>((rOuter - 20) * std::cos(sweepAngle));
        float touchY = cy + static_cast<float>((rOuter - 20) * std::sin(sweepAngle));
        fillCircleAA(renderer, touchX, touchY, 18.0f, SDL_Color{40, 44, 46, 50});
        fillCircleAA(renderer, touchX, touchY, 5.0f, SDL_Color{80, 88, 90, 180});
    }

    // Zone press illumination (subtle dark ripple, never green)
    Uint32 now = SDL_GetTicks();
    if (pressedRegion != WheelRegion::None && pressedRegion != WheelRegion::Center) {
        float glowDist = (rOuter + rInner) * 0.5f;
        float gx = static_cast<float>(cx), gy = static_cast<float>(cy);
        if (pressedRegion == WheelRegion::Menu || pressedRegion == WheelRegion::InnerUp) {
            gy = cy - glowDist;
        } else if (pressedRegion == WheelRegion::PlayPause || pressedRegion == WheelRegion::InnerDown) {
            gy = cy + glowDist;
        } else if (pressedRegion == WheelRegion::Previous || pressedRegion == WheelRegion::InnerEnter) {
            gx = cx - glowDist;
        } else if (pressedRegion == WheelRegion::Next || pressedRegion == WheelRegion::InnerPrev) {
            gx = cx + glowDist;
        }
        Uint8 glowA = (now < sweepUntil && pressedRegion == rippleZone) ? 55 : 28;
        fillCircleAA(renderer, gx, gy, 24.0f, SDL_Color{15, 18, 16, glowA});
    }

    // Four Cardinal Nodes (28px diameter lime circles in 320px scale)
    float nodeDist = (rOuter + rInner) * 0.5f;
    float nodeRadius = std::clamp(layout.body.w * 14.0f / 320.0f, 11.0f, 20.0f);

    float topY = cy - nodeDist;
    float botY = cy + nodeDist;
    float leftX = cx - nodeDist;
    float rightX = cx + nodeDist;

    // Top node (Menu / Back ^)
    fillCircleAA(renderer, static_cast<float>(cx), topY, nodeRadius, Palette::kPlumberTeal);
    SDL_SetRenderDrawColor(renderer, 13, 17, 14, 255);
    SDL_RenderDrawLine(renderer, cx - 5, static_cast<int>(topY) + 2, cx, static_cast<int>(topY) - 3);
    SDL_RenderDrawLine(renderer, cx, static_cast<int>(topY) - 3, cx + 5, static_cast<int>(topY) + 2);
    SDL_RenderDrawLine(renderer, cx - 5, static_cast<int>(topY) + 3, cx, static_cast<int>(topY) - 2);
    SDL_RenderDrawLine(renderer, cx, static_cast<int>(topY) - 2, cx + 5, static_cast<int>(topY) + 3);

    // Bottom node (Play / Pause)
    fillCircleAA(renderer, static_cast<float>(cx), botY, nodeRadius, Palette::kPlumberTeal);
    if (state.status.playing) {
        drawPauseIcon(renderer, cx - 4, static_cast<int>(botY) - 5, 10, Palette::kTextBlack);
    } else {
        drawPlayIcon(renderer, cx - 3, static_cast<int>(botY) - 5, 10, Palette::kTextBlack);
    }

    // Left node (Previous |<)
    float fcy = static_cast<float>(cy);
    fillCircleAA(renderer, leftX, fcy, nodeRadius, Palette::kPlumberTeal);
    SDL_SetRenderDrawColor(renderer, 13, 17, 14, 255);
    SDL_RenderDrawLine(renderer, static_cast<int>(leftX) - 4, cy - 5, static_cast<int>(leftX) - 4, cy + 5);
    SDL_RenderDrawLine(renderer, static_cast<int>(leftX) - 5, cy - 5, static_cast<int>(leftX) - 5, cy + 5);
    SDL_Vertex prevV[3] = {
        {SDL_FPoint{leftX + 5.0f, fcy - 5.0f}, Palette::kTextBlack, SDL_FPoint{0, 0}},
        {SDL_FPoint{leftX - 3.0f, fcy},        Palette::kTextBlack, SDL_FPoint{0, 0}},
        {SDL_FPoint{leftX + 5.0f, fcy + 5.0f}, Palette::kTextBlack, SDL_FPoint{0, 0}},
    };
    SDL_RenderGeometry(renderer, nullptr, prevV, 3, nullptr, 0);

    // Right node (Next >|)
    fillCircleAA(renderer, rightX, fcy, nodeRadius, Palette::kPlumberTeal);
    SDL_SetRenderDrawColor(renderer, 13, 17, 14, 255);
    SDL_RenderDrawLine(renderer, static_cast<int>(rightX) + 4, cy - 5, static_cast<int>(rightX) + 4, cy + 5);
    SDL_RenderDrawLine(renderer, static_cast<int>(rightX) + 5, cy - 5, static_cast<int>(rightX) + 5, cy + 5);
    SDL_Vertex nextV[3] = {
        {SDL_FPoint{rightX - 5.0f, fcy - 5.0f}, Palette::kTextBlack, SDL_FPoint{0, 0}},
        {SDL_FPoint{rightX + 3.0f, fcy},        Palette::kTextBlack, SDL_FPoint{0, 0}},
        {SDL_FPoint{rightX - 5.0f, fcy + 5.0f}, Palette::kTextBlack, SDL_FPoint{0, 0}},
    };
    SDL_RenderGeometry(renderer, nullptr, nextV, 3, nullptr, 0);

    // Center Core Hourglass (Reflects Playing vs Paused state)
    drawOmnitrixDial(renderer, cx, cy, rCenter, state.status.playing);

    if (pressedRegion == WheelRegion::Center) {
        fillCircleAA(renderer, static_cast<float>(cx), fcy, static_cast<float>(rCenter),
                     SDL_Color{20, 24, 22, 60});
    }
}

void drawWheelLabels(SDL_Renderer* renderer, const DeviceLayout& layout, const FontSet& fonts) {
    (void)renderer;
    (void)layout;
    (void)fonts;
}

void drawTitleBar(SDL_Renderer* renderer, const DeviceLayout& layout, const UiState& state,
                  const FontSet& fonts) {
    const SDL_Rect& bar = layout.titleBar;
    SDL_SetRenderDrawColor(renderer, Palette::kUiBg.r, Palette::kUiBg.g, Palette::kUiBg.b, 255);
    SDL_RenderFillRect(renderer, &bar);
    SDL_SetRenderDrawColor(renderer, Palette::kLineGrey.r, Palette::kLineGrey.g, Palette::kLineGrey.b, 255);
    SDL_RenderDrawLine(renderer, bar.x, bar.y + bar.h - 1, bar.x + bar.w, bar.y + bar.h - 1);

    // Screen Title
    std::string title = "HOME";
    if (state.screen == Screen::NowPlaying) title = "NOW PLAYING";
    else if (state.screen == Screen::Search) title = "SEARCH";
    else if (state.screen == Screen::Playlists) title = "PLAYLISTS";
    else if (state.screen == Screen::Tracks && state.listType == TrackListType::Liked) title = "FAVORITES";
    else if (state.screen == Screen::Tracks && state.listType == TrackListType::Playlist) title = "PLAYLIST";
    else if (state.screen == Screen::Tracks && state.listType == TrackListType::SearchResults) title = "RESULTS";
    else if (state.screen == Screen::Tracks && state.listType == TrackListType::Local) title = "LIBRARY";
    else if (state.listType == TrackListType::Liked) title = "FAVORITES";
    else if (state.listType == TrackListType::Downloads) title = "DOWNLOADS";
    else if (state.listType == TrackListType::Queue) title = "QUEUE";

    fonts.bold.draw(renderer, fonts.bold.fit(title, bar.w - 84), bar.x + 12, bar.y + 6, Palette::kTextMint);

    const SDL_Rect& searchBtn = layout.titleSearchButton;
    const float searchCX = searchBtn.x + searchBtn.w / 2.0f - 1.0f;
    const float searchCY = searchBtn.y + searchBtn.h / 2.0f - 1.0f;
    drawRingAA(renderer, searchCX, searchCY, 3.5f, 4.5f, Palette::kPlumberTeal);
    SDL_SetRenderDrawColor(renderer, Palette::kPlumberTeal.r, Palette::kPlumberTeal.g,
                           Palette::kPlumberTeal.b, 255);
    SDL_RenderDrawLine(renderer, static_cast<int>(searchCX + 4), static_cast<int>(searchCY + 4),
                       static_cast<int>(searchCX + 7), static_cast<int>(searchCY + 7));

    const SDL_Rect& playBtn = layout.titlePlayButton;
    if (state.status.playing) {
        drawPauseIcon(renderer, playBtn.x + 6, playBtn.y + 7, 10, Palette::kPlumberTeal);
    } else {
        drawPlayIcon(renderer, playBtn.x + 7, playBtn.y + 7, 9, Palette::kPlumberTeal);
    }

    const SDL_Rect& minBtn = layout.minimizeButton;
    SDL_SetRenderDrawColor(renderer, Palette::kTextDim.r, Palette::kTextDim.g, Palette::kTextDim.b, 255);
    SDL_Rect minRect{minBtn.x + 7, minBtn.y + minBtn.h / 2, 10, 2};
    SDL_RenderFillRect(renderer, &minRect);
}

void drawStatusBar(SDL_Renderer* renderer, const DeviceLayout& layout, const UiState& state,
                   const FontSet& fonts) {
    (void)renderer;
    (void)layout;
    (void)state;
    (void)fonts;
}

void drawBootScreen(SDL_Renderer* renderer, const DeviceLayout& layout, const UiState& state,
                    const FontSet& fonts) {
    const int cx = layout.screen.x + layout.screen.w / 2;
    const int centerY = layout.contentArea.y + layout.contentArea.h / 2 - 10;
    drawOmnitrixDial(renderer, cx, centerY - 20, 28, true);

    const std::string title = "OMNITRIX OS";
    fonts.large.draw(renderer, title, cx - fonts.large.width(title) / 2, centerY + 20, Palette::kGreenYellow);
    const std::string sub = "CALIBRATING DNA CORE...";
    fonts.small.draw(renderer, sub, cx - fonts.small.width(sub) / 2, centerY + 38, Palette::kTextDarkGrey);

    constexpr Uint32 kBootDuration = 1800;
    const Uint32 remaining = state.bootUntil > SDL_GetTicks() ? state.bootUntil - SDL_GetTicks() : 0;
    const float progress = std::clamp(1.0f - static_cast<float>(remaining) / kBootDuration, 0.0f, 1.0f);
    const int barW = layout.contentArea.w - 40;
    const int barX = layout.contentArea.x + 20;
    const int barY = centerY + 56;
    fillRoundRect(renderer, SDL_Rect{barX, barY, barW, 4}, 2, Palette::kTrackGrey);
    fillRoundRect(renderer, SDL_Rect{barX, barY, static_cast<int>(barW * progress), 4}, 2, Palette::kGreenYellow);
}

void drawMainMenu(SDL_Renderer* renderer, const DeviceLayout& layout, UiState& state,
                  const FontSet& fonts) {
    state.screen = Screen::Tracks; // Seamlessly route into Library
}

void drawPlaylistsScreen(SDL_Renderer* renderer, const DeviceLayout& layout, UiState& state,
                         const FontSet& fonts) {
    const int count = static_cast<int>(state.playlists.size());
    if (count == 0) {
        const std::string msg = "NO PLAYLISTS YET\n—\nScan archives to begin";
        int y = layout.contentArea.y + layout.contentArea.h / 2 - 18;
        for (const auto& line : splitLines(msg)) {
            const int tw = fonts.regular.width(line);
            fonts.regular.draw(renderer, line, layout.contentArea.x + (layout.contentArea.w - tw) / 2, y,
                               Palette::kTextDarkGrey);
            y += 14;
        }
        return;
    }

    state.trackSelected = std::clamp(state.trackSelected, 0, count - 1);
    const int rowH = 36;
    const int maxVisible = 5;
    int firstRow = 0;
    if (state.trackSelected >= maxVisible) firstRow = state.trackSelected - maxVisible + 1;

    for (int i = 0; i < maxVisible && firstRow + i < count; ++i) {
        const int idx = firstRow + i;
        SDL_Rect rowRect{layout.contentArea.x + 8, layout.contentArea.y + 6 + i * (rowH + 2), layout.contentArea.w - 16, rowH};
        const bool isSel = idx == state.trackSelected;
        if (isSel) {
            fillRoundRect(renderer, rowRect, 6, Palette::kPlumberTeal);
        }
        const SDL_Color textCol = isSel ? Palette::kTextSel : Palette::kTextMint;
        fonts.bold.draw(renderer, fonts.bold.fit(state.playlists[idx].name, rowRect.w - 80),
                        rowRect.x + 10, rowRect.y + 10, textCol);
        const std::string countText = std::to_string(state.playlists[idx].trackIds.size()) + " tracks";
        fonts.small.draw(renderer, countText, rowRect.x + rowRect.w - fonts.small.width(countText) - 10,
                         rowRect.y + 11, textCol);
    }
}

void refreshPlaylistTracks(UiState& state, resona::PlayerController& controller) {
    state.playlists = controller.playlists();
    state.playlistTracks.clear();
    if (state.activePlaylistId.empty() && !state.playlists.empty()) {
        state.activePlaylistId = state.playlists.front().id;
    }
    for (const auto& playlist : state.playlists) {
        if (playlist.id != state.activePlaylistId) continue;
        for (const auto& id : playlist.trackIds) {
            if (auto track = controller.track(id)) state.playlistTracks.push_back(*track);
        }
        break;
    }
}

const std::vector<resona::Track>& getTracksForType(const UiState& state) {
    switch (state.listType) {
        case TrackListType::Local: return state.localTracks;
        case TrackListType::Playlist: return state.playlistTracks;
        case TrackListType::Queue: return state.status.queue;
        case TrackListType::Liked: return state.likedTracks;
        case TrackListType::Downloads: return state.downloads;
        case TrackListType::SearchResults: return state.searchResults;
    }
    return state.localTracks;
}

void drawTrackListScreen(SDL_Renderer* renderer, const DeviceLayout& layout, UiState& state,
                         const FontSet& fonts) {
    const auto& tracks = getTracksForType(state);
    const int count = static_cast<int>(tracks.size());

    if (count == 0) {
        std::string emptyMsg = "NO TRACKS IN DATABASE\n—\nTap search to find specimens";
        int y = layout.contentArea.y + layout.contentArea.h / 2 - 20;
        for (const auto& line : splitLines(emptyMsg)) {
            const int tw = fonts.regular.width(line);
            fonts.regular.draw(renderer, line, layout.contentArea.x + (layout.contentArea.w - tw) / 2, y,
                               Palette::kTextDarkGrey);
            y += 14;
        }
        return;
    }

    state.trackSelected = std::clamp(state.trackSelected, 0, count - 1);
    const int rowH = 34;
    const int rowGap = 2;
    const int maxVisible = 5;
    int firstRow = 0;
    if (state.trackSelected >= maxVisible) firstRow = state.trackSelected - maxVisible + 1;
    state.firstVisibleTrack = firstRow;
    const int startY = layout.contentArea.y + 4;

    for (int i = 0; i < maxVisible && firstRow + i < count; ++i) {
        const int idx = firstRow + i;
        const auto& t = tracks[idx];
        SDL_Rect rowRect{layout.contentArea.x, startY + i * (rowH + rowGap), layout.contentArea.w, rowH};
        const bool isSel = idx == state.trackSelected;

        if (isSel) {
            fillRoundRect(renderer, rowRect, 0, Palette::kPlumberTeal);
        }

        const SDL_Color titleCol = isSel ? Palette::kTextSel : Palette::kTextMint;
        const std::string title = t.title.empty() ? t.videoId : t.title;
        fonts.regular.draw(renderer, fonts.regular.fit(title, rowRect.w - 36), rowRect.x + 14, rowRect.y + 8, titleCol);

        if (isSel) {
            fonts.bold.draw(renderer, ">", rowRect.x + rowRect.w - 20, rowRect.y + 8, Palette::kTextSel);
        }
    }
}

void drawHomeScreen(SDL_Renderer* renderer, const DeviceLayout& layout, UiState& state,
                    const FontSet& fonts) {
    state.homeSelected = std::clamp(state.homeSelected, 0, kHomeRowCount - 1);

    for (int i = 0; i < kHomeRowCount; ++i) {
        const SDL_Rect row = homeRowRect(layout, i);
        const bool selected = i == state.homeSelected;
        if (selected) fillRoundRect(renderer, row, 6, Palette::kPlumberTeal);
        const SDL_Color color = selected ? Palette::kTextSel : Palette::kTextMint;
        const int textY = row.y + (i == 0 ? 8 : 5);

        std::string text;
        if (i == 0) {
            text = state.status.current.title.empty() ? "NOW PLAYING" : state.status.current.title;
        } else if (i == 1) {
            text = "SEARCH";
        } else if (i == 2) {
            text = "FAVORITES (" + std::to_string(state.likedTracks.size()) + ")";
        } else {
            text = "LIBRARY (" + std::to_string(state.localTracks.size()) + ")";
        }

        if (i == 0 && !selected) {
            fonts.bold.draw(renderer, "NOW PLAYING", row.x + 10, textY, color);
            fonts.small.draw(renderer, fonts.small.fit(text, row.w - 130), row.x + 116, textY + 2,
                             Palette::kTextDim);
        } else {
            fonts.regular.draw(renderer, fonts.regular.fit(text, row.w - 30), row.x + 10, textY, color);
        }
        if (selected) fonts.bold.draw(renderer, ">", row.x + row.w - 18, textY, color);
    }
}

void drawNowPlayingScreen(SDL_Renderer* renderer, const DeviceLayout& layout, UiState& state,
                          const FontSet& fonts, resona::PlayerController& controller) {
    const auto& status = state.status;
    const std::string title = status.current.title.empty() ? "Cosmic drift" : status.current.title;
    const std::string artist = status.current.artist.empty() ? "Omnitrix OST" : status.current.artist;

    // 1. Artwork slot (circular Omnitrix badge inside rounded square)
    int artSize = 64;
    SDL_Rect artRect{layout.contentArea.x + 12, layout.contentArea.y + 12, artSize, artSize};
    fillRoundRectWithBorder(renderer, artRect, 8,
                            resona::ui::OmnitrixTheme::kLcdElevated,
                            resona::ui::OmnitrixTheme::kLcdBorder, 1);

    // Outlined Omnitrix vector emblem inside artwork (matches reference)
    int emblemCX = artRect.x + artSize / 2;
    int emblemCY = artRect.y + artSize / 2;
    drawOutlinedHourglass(renderer, emblemCX, emblemCY, 20, Palette::kPlumberTeal);

    // 2. Track metadata stack
    int metaX = artRect.x + artSize + 12;
    int metaW = layout.contentArea.x + layout.contentArea.w - metaX - 12;
    fonts.bold.draw(renderer, fonts.bold.fit(title, metaW), metaX, artRect.y + 4, Palette::kTextMint);
    fonts.regular.draw(renderer, fonts.regular.fit(artist, metaW), metaX, artRect.y + 24, Palette::kTextDim);
    std::string albumLine = status.current.album;
    for (size_t i = 0; i < status.queue.size(); ++i) {
        if (status.queue[i].id != status.current.id) continue;
        if (!albumLine.empty()) albumLine += " • ";
        albumLine += std::to_string(i + 1) + "/" + std::to_string(status.queue.size());
        break;
    }
    if (albumLine.empty()) albumLine = "Omnitrix OST";
    fonts.small.draw(renderer, fonts.small.fit(albumLine, metaW), metaX, artRect.y + 42, Palette::kTextDark);

    // 3. Action icons (+, heart, download)
    const SDL_Rect addBtn = nowPlayingActionRect(layout, 0);
    const SDL_Rect heartBtn = nowPlayingActionRect(layout, 1);
    const SDL_Rect dlBtn = nowPlayingActionRect(layout, 2);
    const int btnSize = addBtn.w;
    const int bHalf = btnSize / 2;

    // Button '+' (Add to playlist - outlined circle with +)
    drawRingAA(renderer, addBtn.x + bHalf, addBtn.y + bHalf, bHalf - 1.5f, static_cast<float>(bHalf), Palette::kTextMint);
    SDL_SetRenderDrawColor(renderer, Palette::kTextMint.r, Palette::kTextMint.g, Palette::kTextMint.b, 255);
    SDL_Rect hBar{addBtn.x + 6, addBtn.y + bHalf - 1, btnSize - 12, 2};
    SDL_RenderFillRect(renderer, &hBar);
    SDL_Rect vBar{addBtn.x + bHalf - 1, addBtn.y + 6, 2, btnSize - 12};
    SDL_RenderFillRect(renderer, &vBar);

    // Button heart
    bool isLiked = controller.isLiked(status.current.id);
    {
        SDL_Color hCol = isLiked ? Palette::kPlumberTeal : Palette::kTextMint;
        float hcx = heartBtn.x + 11.0f;
        float hcy = heartBtn.y + 10.0f;
        if (isLiked) {
            fillCircleAA(renderer, hcx - 3.6f, hcy - 2.0f, 4.0f, hCol);
            fillCircleAA(renderer, hcx + 3.6f, hcy - 2.0f, 4.0f, hCol);
            SDL_Vertex hTri[3] = {
                {SDL_FPoint{hcx - 7.3f, hcy - 0.5f}, hCol, SDL_FPoint{0, 0}},
                {SDL_FPoint{hcx + 7.3f, hcy - 0.5f}, hCol, SDL_FPoint{0, 0}},
                {SDL_FPoint{hcx, hcy + 7.2f}, hCol, SDL_FPoint{0, 0}},
            };
            SDL_RenderGeometry(renderer, nullptr, hTri, 3, nullptr, 0);
        } else {
            // Smooth outlined heart using two top arcs and bottom V
            constexpr int kArcN = 10;
            constexpr float kPi = 3.14159265358979323846f;
            SDL_SetRenderDrawColor(renderer, hCol.r, hCol.g, hCol.b, 255);
            auto drawLobe = [&](float lx, float ly, float a0, float a1) {
                for (int i = 0; i < kArcN; ++i) {
                    float t0 = a0 + (a1 - a0) * i / kArcN;
                    float t1 = a0 + (a1 - a0) * (i + 1) / kArcN;
                    SDL_RenderDrawLine(renderer,
                                       static_cast<int>(std::round(lx + 3.6f * std::cos(t0))),
                                       static_cast<int>(std::round(ly + 3.6f * std::sin(t0))),
                                       static_cast<int>(std::round(lx + 3.6f * std::cos(t1))),
                                       static_cast<int>(std::round(ly + 3.6f * std::sin(t1))));
                }
            };
            drawLobe(hcx - 3.6f, hcy - 2.0f, kPi * 0.9f, kPi * 2.0f);
            drawLobe(hcx + 3.6f, hcy - 2.0f, kPi * 1.0f, kPi * 2.1f);
            SDL_RenderDrawLine(renderer, static_cast<int>(std::round(hcx - 7.0f)), static_cast<int>(std::round(hcy - 0.8f)),
                               static_cast<int>(std::round(hcx)), static_cast<int>(std::round(hcy + 7.0f)));
            SDL_RenderDrawLine(renderer, static_cast<int>(std::round(hcx + 7.0f)), static_cast<int>(std::round(hcy - 0.8f)),
                               static_cast<int>(std::round(hcx)), static_cast<int>(std::round(hcy + 7.0f)));
        }
    }

    // Button download (down arrow into tray)
    {
        SDL_SetRenderDrawColor(renderer, Palette::kTextMint.r, Palette::kTextMint.g, Palette::kTextMint.b, 255);
        int dx = dlBtn.x, dy = dlBtn.y;
        SDL_Rect stem{dx + bHalf - 1, dy + 4, 2, 9};
        SDL_RenderFillRect(renderer, &stem);
        SDL_RenderDrawLine(renderer, dx + 7,  dy + 9,  dx + 11, dy + 13);
        SDL_RenderDrawLine(renderer, dx + 15, dy + 9,  dx + 11, dy + 13);
        SDL_RenderDrawLine(renderer, dx + 5,  dy + 13, dx + 5,  dy + 17);
        SDL_RenderDrawLine(renderer, dx + 17, dy + 13, dx + 17, dy + 17);
        SDL_Rect base{dx + 5, dy + 16, 13, 2};
        SDL_RenderFillRect(renderer, &base);
    }

    // 4. Progress Scrubber Bar
    int barX = layout.contentArea.x + 12;
    int barW = layout.contentArea.w - 24;
    int barY = layout.contentArea.y + 132;
    fillRoundRect(renderer, SDL_Rect{barX, barY, barW, 4}, 2, Palette::kTrackGrey);

    int progressPct = resona::ui::progressPercent(status.positionMs, status.durationMs);
    if (progressPct == 0 && status.durationMs == 0 && state.demoMode) progressPct = 38;
    int fillW = std::clamp(barW * progressPct / 100, 0, barW);
    fillRoundRect(renderer, SDL_Rect{barX, barY, fillW, 4}, 2, Palette::kPlumberTeal);

    // Timestamps
    int timeY = barY + 8;
    std::string elapsed = resona::prettyDuration(status.positionMs > 0 ? status.positionMs
                                                                        : (state.demoMode ? 84000 : 0));
    std::string total = resona::prettyDuration(status.durationMs > 0 ? status.durationMs
                                                                      : (state.demoMode ? 222000 : 0));
    fonts.mono.draw(renderer, elapsed, barX, timeY, Palette::kTextDim);
    fonts.mono.draw(renderer, total, barX + barW - fonts.mono.width(total), timeY, Palette::kTextDim);
}

void drawTrackChangeOverlay(SDL_Renderer* renderer, const DeviceLayout& layout, const UiState& state,
                            const FontSet& fonts) {
    (void)renderer;
    (void)layout;
    (void)state;
    (void)fonts;
}

void drawDNAScan(SDL_Renderer* renderer, const DeviceLayout& layout, const UiState& state,
                 const FontSet& fonts) {
    (void)renderer;
    (void)layout;
    (void)state;
    (void)fonts;
}

void drawSearchScreen(SDL_Renderer* renderer, const DeviceLayout& layout, UiState& state,
                      const FontSet& fonts, resona::PlayerController& controller) {
    (void)controller;
    const auto& search = state.searchService ? state.searchService->state()
                                             : resona::search::SearchState{};
    state.searchResults = search.songs;
    state.listType = TrackListType::SearchResults;
    updateSearchScroll(state, true);

    SDL_Rect inputBar{layout.contentArea.x + 12, layout.contentArea.y + 8, layout.contentArea.w - 24, 34};
    fillRoundRectWithBorder(renderer, inputBar, 8, resona::ui::OmnitrixTheme::kLcdInput,
                            SDL_Color{44, 58, 44, 255}, 1);

    drawRingAA(renderer, inputBar.x + 14.0f, inputBar.y + 16.0f, 4.0f, 5.2f, Palette::kPlumberTeal);
    SDL_SetRenderDrawColor(renderer, Palette::kPlumberTeal.r, Palette::kPlumberTeal.g, Palette::kPlumberTeal.b, 255);
    SDL_RenderDrawLine(renderer, inputBar.x + 17, inputBar.y + 19, inputBar.x + 21, inputBar.y + 23);

    if (state.searchQuery.empty()) {
        fonts.regular.draw(renderer, "Search songs...", inputBar.x + 28, inputBar.y + 9, Palette::kTextDark);
    } else {
        std::string queryText = state.searchQuery + (state.cursorVisible ? "|" : "");
        fonts.regular.draw(renderer, fonts.regular.fit(queryText, inputBar.w - 54),
                           inputBar.x + 28, inputBar.y + 9, Palette::kTextMint);
    }

    const float clrCX = static_cast<float>(inputBar.x + inputBar.w - 16);
    const float clrCY = static_cast<float>(inputBar.y + 17);
    fillCircleAA(renderer, clrCX, clrCY, 8.0f, SDL_Color{38, 46, 40, 255});
    SDL_SetRenderDrawColor(renderer, Palette::kTextMint.r, Palette::kTextMint.g, Palette::kTextMint.b, 255);
    SDL_RenderDrawLine(renderer, static_cast<int>(clrCX) - 3, static_cast<int>(clrCY) - 3,
                       static_cast<int>(clrCX) + 3, static_cast<int>(clrCY) + 3);
    SDL_RenderDrawLine(renderer, static_cast<int>(clrCX) + 3, static_cast<int>(clrCY) - 3,
                       static_cast<int>(clrCX) - 3, static_cast<int>(clrCY) + 3);

    auto drawRow = [&](int index, int rowY, const std::string& title, const std::string& subtitle) {
        SDL_Rect row{layout.contentArea.x + 8, rowY, layout.contentArea.w - 16, 34};
        const bool selected = index == state.trackSelected;
        if (selected) fillRoundRect(renderer, row, 6, Palette::kPlumberTeal);
        const SDL_Color col = selected ? Palette::kTextSel : Palette::kTextMint;
        fonts.regular.draw(renderer, fonts.regular.fit(title, row.w - 24), row.x + 10, row.y + 4, col);
        if (!subtitle.empty()) {
            fonts.small.draw(renderer, fonts.small.fit(subtitle, row.w - 24), row.x + 10, row.y + 19,
                             selected ? Palette::kTextSel : Palette::kTextDim);
        }
    };

    if (state.searchQuery.empty()) {
        const int recentY = layout.contentArea.y + 52;
        fonts.bold.draw(renderer, "RECENT SEARCHES", inputBar.x, recentY, Palette::kTextDark);
        fonts.bold.draw(renderer, "Clear", inputBar.x + inputBar.w - fonts.bold.width("Clear"),
                        recentY, Palette::kPlumberTeal);

        const size_t count = std::min<size_t>(3, search.history.size());
        for (size_t i = 0; i < count; ++i) {
            int rowY = recentY + 18 + static_cast<int>(i) * 26;
            drawRingAA(renderer, inputBar.x + 6.0f, rowY + 7.0f, 4.0f, 5.0f, Palette::kTextDim);
            SDL_SetRenderDrawColor(renderer, Palette::kTextDim.r, Palette::kTextDim.g, Palette::kTextDim.b, 255);
            SDL_RenderDrawLine(renderer, inputBar.x + 6, rowY + 4, inputBar.x + 6, rowY + 7);
            SDL_RenderDrawLine(renderer, inputBar.x + 6, rowY + 7, inputBar.x + 9, rowY + 7);
            fonts.regular.draw(renderer, fonts.regular.fit(search.history[i], inputBar.w - 34),
                               inputBar.x + 18, rowY + 1, Palette::kTextMint);
            const int ax = inputBar.x + inputBar.w - 10;
            const int ay = rowY + 3;
            SDL_RenderDrawLine(renderer, ax - 5, ay + 5, ax, ay);
            SDL_RenderDrawLine(renderer, ax - 4, ay, ax, ay);
            SDL_RenderDrawLine(renderer, ax, ay, ax, ay + 4);
        }

        fonts.small.draw(renderer, "Find any song in your library",
                         inputBar.x, layout.contentArea.y + layout.contentArea.h - 16, Palette::kTextDark);
        return;
    }

    switch (search.status) {
        case resona::search::SearchStatus::Idle:
            break;
        case resona::search::SearchStatus::Loading: {
            const std::string text = "SEARCHING...";
            fonts.bold.draw(renderer, text, inputBar.x + (inputBar.w - fonts.bold.width(text)) / 2,
                            layout.contentArea.y + 100, Palette::kPlumberTeal);
            break;
        }
        case resona::search::SearchStatus::Error: {
            fonts.regular.draw(renderer, fonts.regular.fit(search.error, inputBar.w), inputBar.x,
                               layout.contentArea.y + 80, Palette::kTextDim);
            fonts.bold.draw(renderer, "RETRY", inputBar.x, layout.contentArea.y + 110, Palette::kPlumberTeal);
            break;
        }
        case resona::search::SearchStatus::Empty: {
            const std::string text = "NO RESULTS FOUND";
            fonts.bold.draw(renderer, text, inputBar.x + (inputBar.w - fonts.bold.width(text)) / 2,
                            layout.contentArea.y + 100, Palette::kTextDim);
            break;
        }
        case resona::search::SearchStatus::Success: {
            const int firstRow = inputBar.y + inputBar.h + 8;
            const int firstVisible = state.firstVisibleTrack;
            const int available = std::max(0, static_cast<int>(search.songs.size()) - firstVisible);
            const int visible = std::min(kSearchVisibleRows, available);
            for (int i = 0; i < visible; ++i) {
                const int index = firstVisible + i;
                drawRow(index, firstRow + i * 38, search.songs[index].title, search.songs[index].artist);
            }
            break;
        }
    }
}

// ============================================================================
// Mini-Player Floating Card Rendering (312 x 128 px)
// ============================================================================

void drawMiniPlayerCard(SDL_Renderer* renderer, int winW, int winH, UiState& state,
                        const FontSet& fonts, resona::PlayerController& controller) {
    (void)controller;
    int cardW = 312;
    int cardH = 128;
    int cardX = (winW - cardW) / 2;
    int cardY = (winH - cardH) / 2;

    // Drop shadow
    fillRoundRect(renderer, SDL_Rect{cardX - 4, cardY - 2, cardW + 8, cardH + 10}, 18, Palette::kPlayerShadow);

    // Card Chassis (Matte Charcoal #1A1C1E with 16px rounded border)
    fillRoundRectWithBorder(renderer, SDL_Rect{cardX, cardY, cardW, cardH}, 16,
                            resona::ui::OmnitrixTheme::kDarkChassis.surface,
                            resona::ui::OmnitrixTheme::kDarkChassis.border, 1);

    // Top Header Strip
    bool isPlaying = state.status.playing;
    SDL_Color statusDot = isPlaying ? resona::ui::OmnitrixTheme::kLimePrimary : resona::ui::OmnitrixTheme::kLimeMuted;
    fillCircleAA(renderer, cardX + 16.0f, cardY + 14.0f, 3.5f, statusDot);
    fonts.bold.draw(renderer, "OMNITRIX MINI", cardX + 26, cardY + 8, Palette::kTextDim);

    // Expand button (diagonal arrows -> restore full player)
    int expX = cardX + cardW - 44;
    int expY = cardY + 7;
    SDL_SetRenderDrawColor(renderer, Palette::kTextDim.r, Palette::kTextDim.g, Palette::kTextDim.b, 255);
    SDL_RenderDrawLine(renderer, expX, expY + 9, expX + 9, expY);
    SDL_RenderDrawLine(renderer, expX + 5, expY, expX + 9, expY);
    SDL_RenderDrawLine(renderer, expX + 9, expY, expX + 9, expY + 4);
    SDL_RenderDrawLine(renderer, expX, expY + 9, expX + 4, expY + 9);
    SDL_RenderDrawLine(renderer, expX, expY + 5, expX, expY + 9);

    // Close button (×)
    int clsX = cardX + cardW - 20;
    int clsY = cardY + 7;
    SDL_SetRenderDrawColor(renderer, Palette::kTextDim.r, Palette::kTextDim.g, Palette::kTextDim.b, 255);
    SDL_RenderDrawLine(renderer, clsX,     clsY,     clsX + 9, clsY + 9);
    SDL_RenderDrawLine(renderer, clsX + 9, clsY,     clsX,     clsY + 9);

    // Middle Content Row
    // Mini Artwork slot (52 x 52)
    int artX = cardX + 14;
    int artY = cardY + 34;
    SDL_Rect miniArt{artX, artY, 52, 52};
    fillRoundRectWithBorder(renderer, miniArt, 10,
                            resona::ui::OmnitrixTheme::kLcdBg,
                            resona::ui::OmnitrixTheme::kLcdBorder, 1);

    // Solid Hourglass inside mini artwork (matches reference)
    drawMiniHourglass(renderer, artX + 26, artY + 26, 11, 13, isPlaying);

    // Song metadata stack
    const auto& status = state.status;
    std::string title = status.current.title.empty() ? "Cosmic drift" : status.current.title;
    std::string subtitle = status.current.artist.empty() ? "Omnitrix Radio" : status.current.artist;
    fonts.bold.draw(renderer, fonts.bold.fit(title, 110), cardX + 76, cardY + 40, Palette::kTextMint);
    fonts.regular.draw(renderer, subtitle, cardX + 76, cardY + 60, Palette::kTextDim);

    // Controls Cluster
    // Skip Previous |<
    int prevX = cardX + cardW - 104;
    int ctrlY = cardY + 48;
    SDL_SetRenderDrawColor(renderer, Palette::kTextDim.r, Palette::kTextDim.g, Palette::kTextDim.b, 255);
    SDL_RenderDrawLine(renderer, prevX, ctrlY, prevX, ctrlY + 12);
    SDL_Vertex pV[3] = {
        {SDL_FPoint{static_cast<float>(prevX + 8), static_cast<float>(ctrlY)}, Palette::kTextDim, SDL_FPoint{0, 0}},
        {SDL_FPoint{static_cast<float>(prevX), static_cast<float>(ctrlY + 6)}, Palette::kTextDim, SDL_FPoint{0, 0}},
        {SDL_FPoint{static_cast<float>(prevX + 8), static_cast<float>(ctrlY + 12)}, Palette::kTextDim, SDL_FPoint{0, 0}},
    };
    SDL_RenderGeometry(renderer, nullptr, pV, 3, nullptr, 0);

    // Hero Play/Pause Button (36 x 36)
    int playCX = cardX + cardW - 66;
    int playCY = cardY + 54;
    float heroR = 17.0f;
    if (isPlaying) {
        fillCircleAA(renderer, playCX, playCY, heroR, Palette::kPlumberTeal);
        drawRingAA(renderer, playCX, playCY, heroR, heroR + 3.0f,
                   SDL_Color{Palette::kPlumberTeal.r, Palette::kPlumberTeal.g, Palette::kPlumberTeal.b, 60});
        drawPauseIcon(renderer, playCX - 4, playCY - 5, 10, Palette::kTextBlack);
    } else {
        fillCircleAA(renderer, playCX, playCY, heroR, resona::ui::OmnitrixTheme::kDialRing);
        drawRingAA(renderer, playCX, playCY, heroR - 1.5f, heroR, Palette::kPlumberTeal);
        drawPlayIcon(renderer, playCX - 3, playCY - 5, 10, Palette::kPlumberTeal);
    }

    // Skip Next >|
    int nextX = cardX + cardW - 28;
    SDL_SetRenderDrawColor(renderer, Palette::kTextDim.r, Palette::kTextDim.g, Palette::kTextDim.b, 255);
    SDL_RenderDrawLine(renderer, nextX + 8, ctrlY, nextX + 8, ctrlY + 12);
    SDL_Vertex nV[3] = {
        {SDL_FPoint{static_cast<float>(nextX), static_cast<float>(ctrlY)}, Palette::kTextDim, SDL_FPoint{0, 0}},
        {SDL_FPoint{static_cast<float>(nextX + 8), static_cast<float>(ctrlY + 6)}, Palette::kTextDim, SDL_FPoint{0, 0}},
        {SDL_FPoint{static_cast<float>(nextX), static_cast<float>(ctrlY + 12)}, Palette::kTextDim, SDL_FPoint{0, 0}},
    };
    SDL_RenderGeometry(renderer, nullptr, nV, 3, nullptr, 0);

    // Bottom Progress Bar
    int barX = cardX + 14;
    int barW = cardW - 28;
    int barY = cardY + 104;
    fillRoundRect(renderer, SDL_Rect{barX, barY, barW, 4}, 2, Palette::kTrackGrey);
    int pct = resona::ui::progressPercent(status.positionMs, status.durationMs);
    if (pct == 0 && status.durationMs == 0 && state.demoMode) pct = 42;
    int fillW = std::clamp(barW * pct / 100, 0, barW);
    SDL_Color barFill = isPlaying ? Palette::kPlumberTeal : resona::ui::OmnitrixTheme::kLimeMuted;
    fillRoundRect(renderer, SDL_Rect{barX, barY, fillW, 4}, 2, barFill);
}

// ============================================================================
// Dock Tray Icon Rendering (24 x 24 px / 32 x 32 px badge)
// ============================================================================

void drawTrayBadge(SDL_Renderer* renderer, int winW, int winH, UiState& state) {
    const float fcx = winW * 0.5f;
    const float fcy = winH * 0.5f;
    const float radius = 16.0f;

    // Dark circular badge (#1A1D1F) matching Image 5
    fillCircleAA(renderer, fcx, fcy, radius, SDL_Color{24, 27, 29, 255});
    drawMiniHourglass(renderer, static_cast<int>(fcx), static_cast<int>(fcy), 7, 8, state.status.playing);
}

// ============================================================================
// Actions & Navigation Logic
// ============================================================================

WheelRegion getWheelRegionAt(int x, int y, const DeviceLayout& layout) {
    double dx = static_cast<double>(x - layout.wheelCX);
    double dy = static_cast<double>(y - layout.wheelCY);
    double dist = std::sqrt(dx * dx + dy * dy);

    if (dist <= layout.centerRadius) return WheelRegion::Center;
    if (dist > layout.outerRadius) return WheelRegion::None;

    double angleDeg = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;

    if (dist <= layout.innerRadius) {
        if (angleDeg >= -135.0 && angleDeg < -45.0) return WheelRegion::InnerUp;
        if (angleDeg >= 45.0 && angleDeg < 135.0) return WheelRegion::InnerDown;
        if (angleDeg >= -45.0 && angleDeg < 45.0) return WheelRegion::InnerPrev;
        return WheelRegion::InnerEnter;
    }

    if (angleDeg >= -135.0 && angleDeg < -45.0) return WheelRegion::Menu;
    if (angleDeg >= -45.0 && angleDeg < 45.0) return WheelRegion::Next;
    if (angleDeg >= 45.0 && angleDeg < 135.0) return WheelRegion::PlayPause;
    return WheelRegion::Previous;
}

double getWheelAngleAt(int x, int y, const DeviceLayout& layout) {
    return std::atan2(static_cast<double>(y - layout.wheelCY), static_cast<double>(x - layout.wheelCX));
}

void triggerSearch(resona::PlayerController& controller, UiState& state) {
    (void)controller;
    if (state.searchQuery.empty() || !state.searchService) return;
    state.searchService->submit(state.searchQuery);
    state.trackSelected = 0;
    state.firstVisibleTrack = 0;
}

void playSelectedTrack(resona::PlayerController& controller, UiState& state) {
    const auto& tracks = getTracksForType(state);
    if (tracks.empty()) return;
    state.trackSelected = std::clamp(state.trackSelected, 0, static_cast<int>(tracks.size()) - 1);
    const auto& t = tracks[state.trackSelected];
    controller.play(t);
    state.status = controller.status();
    if (state.demoMode) {
        state.status.hasCurrent = true;
        state.status.current = t;
        state.status.durationMs = t.durationMs;
        state.status.playing = true;
    }
    state.screen = Screen::NowPlaying;
}

void leaveSearch(UiState& state) {
    SDL_StopTextInput();
    if (state.listType == TrackListType::SearchResults) state.listType = TrackListType::Local;
}

void openSearch(UiState& state) {
    state.screen = Screen::Search;
    state.trackSelected = 0;
    state.firstVisibleTrack = 0;
    SDL_StartTextInput();
}

void openFavorites(UiState& state) {
    state.listType = TrackListType::Liked;
    state.screen = Screen::Tracks;
    state.trackSelected = 0;
}

void activateHomeSelection(UiState& state, resona::PlayerController& controller) {
    (void)controller;
    switch (state.homeSelected) {
        case 0:
            state.screen = Screen::NowPlaying;
            break;
        case 1:
            openSearch(state);
            break;
        case 2:
            openFavorites(state);
            break;
        case 3:
            state.listType = TrackListType::Local;
            state.screen = Screen::Tracks;
            state.trackSelected = 0;
            break;
    }
}

void stepSelection(UiState& state, int delta, resona::PlayerController& controller) {
    (void)controller;
    if (state.screen == Screen::Home) {
        state.homeSelected = resona::ui::stepSelection(state.homeSelected, delta, kHomeRowCount);
        return;
    }
    if (state.screen == Screen::Search) {
        const int count = static_cast<int>(state.searchResults.size());
        if (count <= 0) return;
        state.trackSelected = resona::ui::stepSelection(state.trackSelected, delta, count);
        const int lastVisible = state.firstVisibleTrack + kSearchVisibleRows - 1;
        if (state.trackSelected < state.firstVisibleTrack) {
            state.firstVisibleTrack = state.trackSelected;
        } else if (state.trackSelected > lastVisible) {
            state.firstVisibleTrack = state.trackSelected - kSearchVisibleRows + 1;
        }
        updateSearchScroll(state, true);
        return;
    }
    const int count = static_cast<int>(getTracksForType(state).size());
    if (count > 0) {
        state.trackSelected = resona::ui::stepSelection(state.trackSelected, delta, count);
    }
}

void applyScrollDelta(UiState& state, double delta, resona::PlayerController& controller) {
    constexpr double kScrollStep = 1.0;
    state.scrollAccumulator += delta;
    while (state.scrollAccumulator >= kScrollStep) {
        if (state.screen == Screen::NowPlaying) {
            const int64_t target = std::max<int64_t>(0, state.status.positionMs + 5000);
            controller.seek(target);
            state.status.positionMs = target;
        } else {
            stepSelection(state, 1, controller);
        }
        state.scrollAccumulator -= kScrollStep;
    }
    while (state.scrollAccumulator <= -kScrollStep) {
        if (state.screen == Screen::NowPlaying) {
            const int64_t target = std::max<int64_t>(0, state.status.positionMs - 5000);
            controller.seek(target);
            state.status.positionMs = target;
        } else {
            stepSelection(state, -1, controller);
        }
        state.scrollAccumulator += kScrollStep;
    }
}

void goBack(UiState& state) {
    if (state.screen == Screen::Search) leaveSearch(state);
    if (state.screen == Screen::Home) return;
    if (state.screen == Screen::Tracks && state.listType == TrackListType::Local) {
        state.screen = Screen::Home;
        return;
    }
    state.screen = Screen::Home;
    state.listType = TrackListType::Local;
}

void activateSelection(UiState& state, resona::PlayerController& controller, bool& running) {
    (void)running;
    if (state.screen == Screen::Home) {
        activateHomeSelection(state, controller);
    } else if (state.screen == Screen::Tracks || state.screen == Screen::Search) {
        playSelectedTrack(controller, state);
    } else if (state.screen == Screen::NowPlaying) {
        controller.togglePause();
        state.status = controller.status();
    }
}

void handleScreenClick(int x, int y, const DeviceLayout& layout, UiState& state,
                       resona::PlayerController& controller, bool& running) {
    // 1. Power button click -> Toggle Tray Mode (per spec: hides to System Tray/Dock without stopping audio)
    int pCX = layout.powerButton.x + layout.powerButton.w / 2;
    int pCY = layout.powerButton.y + layout.powerButton.h / 2;
    int pDistSq = (x - pCX) * (x - pCX) + (y - pCY) * (y - pCY);
    if (pDistSq <= (layout.powerButton.w / 2) * (layout.powerButton.w / 2)) {
        state.isTrayMode = true;
        state.isMiniPlayer = false;
        state.popupOpen = false;
        return;
    }

    auto inButton = [x, y](const SDL_Rect& rect) {
        return x >= rect.x - 2 && x <= rect.x + rect.w + 2 &&
               y >= rect.y - 2 && y <= rect.y + rect.h + 2;
    };

    // 2. Minimize button click -> Toggle Mini-Player
    if (inButton(layout.minimizeButton)) {
        state.isMiniPlayer = true;
        return;
    }
    if (inButton(layout.titlePlayButton)) {
        controller.togglePause();
        state.status = controller.status();
        return;
    }
    if (inButton(layout.titleSearchButton)) {
        if (state.screen == Screen::Search) {
            leaveSearch(state);
            state.screen = Screen::Home;
        } else {
            openSearch(state);
        }
        return;
    }

    if (y >= layout.titleBar.y && y <= layout.titleBar.y + layout.titleBar.h) {
        if (state.screen == Screen::Search) {
            leaveSearch(state);
            state.screen = Screen::Home;
        } else {
            openSearch(state);
        }
        return;
    }

    if (state.screen == Screen::Home) {
        const int row = homeRowAt(x, y, layout);
        if (row >= 0) {
            state.homeSelected = row;
            activateHomeSelection(state, controller);
        }
        return;
    }

    if (state.screen == Screen::Tracks) {
        const int rowH = 36 + 4;
        int clickedIdx = (y - (layout.contentArea.y + 6)) / rowH + state.firstVisibleTrack;
        const auto& tracks = getTracksForType(state);
        if (clickedIdx >= 0 && clickedIdx < static_cast<int>(tracks.size())) {
            state.trackSelected = clickedIdx;
            playSelectedTrack(controller, state);
        }
        return;
    }

    if (state.screen == Screen::NowPlaying) {
        // Scrubber click to seek
        int barY = layout.contentArea.y + 132;
        int barX = layout.contentArea.x + 12;
        int barW = layout.contentArea.w - 24;
        if (std::abs(y - barY) <= 8 && x >= barX && x <= barX + barW) {
            float pct = static_cast<float>(x - barX) / barW;
            const int64_t duration = state.status.durationMs > 0
                                         ? state.status.durationMs
                                         : (state.demoMode ? 222000 : 0);
            int64_t target = static_cast<int64_t>(pct * duration);
            controller.seek(target);
            state.status.positionMs = target;
            return;
        }

        if (!state.status.current.id.empty()) {
            const SDL_Rect addBtn = nowPlayingActionRect(layout, 0);
            const SDL_Rect heartBtn = nowPlayingActionRect(layout, 1);
            const SDL_Rect dlBtn = nowPlayingActionRect(layout, 2);
            if (x >= addBtn.x && x <= addBtn.x + addBtn.w &&
                y >= addBtn.y && y <= addBtn.y + addBtn.h) {
                auto playlists = controller.playlists();
                if (playlists.empty()) playlists.push_back(controller.createPlaylist("Favorites"));
                controller.addToPlaylist(playlists.front().id, state.status.current.id);
                return;
            }
            if (x >= heartBtn.x && x <= heartBtn.x + heartBtn.w &&
                y >= heartBtn.y && y <= heartBtn.y + heartBtn.h) {
                bool currentLiked = controller.isLiked(state.status.current.id);
                controller.setLiked(state.status.current.id, !currentLiked);
                state.status = controller.status();
                return;
            }
            if (x >= dlBtn.x && x <= dlBtn.x + dlBtn.w &&
                y >= dlBtn.y && y <= dlBtn.y + dlBtn.h) {
                controller.download(state.status.current.id);
                return;
            }
        }

        // Artwork click -> Toggle pause
        if (x >= layout.contentArea.x + 12 && x <= layout.contentArea.x + 76 &&
            y >= layout.contentArea.y + 12 && y <= layout.contentArea.y + 76) {
            controller.togglePause();
            state.status = controller.status();
        }
        return;
    }

    if (state.screen == Screen::Playlists) {
        const int rowH = 38;
        const int idx = (y - (layout.contentArea.y + 6)) / rowH + state.firstVisibleTrack;
        if (idx >= 0 && idx < static_cast<int>(state.playlists.size())) {
            state.activePlaylistId = state.playlists[idx].id;
            refreshPlaylistTracks(state, controller);
            state.listType = TrackListType::Playlist;
            state.screen = Screen::Tracks;
            state.trackSelected = 0;
        }
        return;
    }

    if (state.screen == Screen::Search) {
        auto* search = state.searchService;

        const int inputY = layout.contentArea.y + 8;
        if (y >= inputY && y <= inputY + 34) {
            if (x >= layout.contentArea.x + layout.contentArea.w - 30) {
                state.searchQuery.clear();
                state.searchResults.clear();
                state.trackSelected = 0;
                state.firstVisibleTrack = 0;
                if (search) search->clear();
                SDL_StopTextInput();
            } else {
                SDL_StartTextInput();
            }
            return;
        }

        const int recentY = layout.contentArea.y + 52;

        if (!state.searchQuery.empty()) {
            if (search) {
                const int firstRow = inputY + 34 + 8;
                for (int i = 0; i < kSearchVisibleRows; ++i) {
                    const int rowY = firstRow + i * 38;
                    const int index = state.firstVisibleTrack + i;
                    if (y >= rowY && y <= rowY + 34 && index < static_cast<int>(state.searchResults.size())) {
                        state.trackSelected = index;
                        playSelectedTrack(controller, state);
                        return;
                    }
                }
                if (search->state().status == resona::search::SearchStatus::Error &&
                    y >= layout.contentArea.y + 104 && y <= layout.contentArea.y + 126 &&
                    x <= layout.contentArea.x + 70) {
                    search->submit(state.searchQuery);
                    return;
                }
            }
            return;
        }

        if (y >= recentY - 4 && y <= recentY + 16 && x >= layout.contentArea.x + layout.contentArea.w - 50) {
            controller.clearSearchHistory();
            if (search) search->clearHistory();
            return;
        }

        if (search) {
            const auto& history = search->state().history;
            for (int i = 0; i < std::min<int>(3, static_cast<int>(history.size())); ++i) {
                const int rowY = recentY + 18 + i * 26;
                if (y >= rowY - 2 && y <= rowY + 22) {
                    state.searchQuery = history[i];
                    triggerSearch(controller, state);
                    return;
                }
            }
        }
    }
}

void handleMiniPlayerClick(int mx, int my, int winW, int winH, UiState& state,
                           resona::PlayerController& controller) {
    int cardW = 312;
    int cardH = 128;
    int cardX = (winW - cardW) / 2;
    int cardY = (winH - cardH) / 2;

    // Expand button (↗↙) or artwork click -> restore full player
    if ((mx >= cardX + cardW - 48 && mx <= cardX + cardW - 26 && my >= cardY + 4 && my <= cardY + 24) ||
        (mx >= cardX + 14 && mx <= cardX + 66 && my >= cardY + 34 && my <= cardY + 86)) {
        state.isMiniPlayer = false;
        state.isTrayMode = false;
        state.popupOpen = false;
        return;
    }

    // Close button (×)
    if (mx >= cardX + cardW - 24 && mx <= cardX + cardW - 6 && my >= cardY + 4 && my <= cardY + 24) {
        state.isMiniPlayer = false;
        state.isTrayMode = false;
        state.popupOpen = false;
        return;
    }

    // Skip Previous
    int prevX = cardX + cardW - 104;
    int ctrlY = cardY + 44;
    if (mx >= prevX - 6 && mx <= prevX + 16 && my >= ctrlY && my <= ctrlY + 24) {
        controller.previous();
        state.status = controller.status();
        return;
    }

    // Hero Play/Pause
    int playCX = cardX + cardW - 66;
    int playCY = cardY + 54;
    int distSq = (mx - playCX) * (mx - playCX) + (my - playCY) * (my - playCY);
    if (distSq <= 18 * 18) {
        controller.togglePause();
        state.status = controller.status();
        return;
    }

    // Skip Next
    int nextX = cardX + cardW - 28;
    if (mx >= nextX - 6 && mx <= nextX + 16 && my >= ctrlY && my <= ctrlY + 24) {
        controller.next();
        state.status = controller.status();
        return;
    }

    // Progress Bar seek
    int barX = cardX + 14;
    int barW = cardW - 28;
    int barY = cardY + 104;
    if (std::abs(my - barY) <= 8 && mx >= barX && mx <= barX + barW) {
        float pct = static_cast<float>(mx - barX) / barW;
        const int64_t duration = state.status.durationMs > 0 ? state.status.durationMs
                                                              : (state.demoMode ? 222000 : 0);
        int64_t target = static_cast<int64_t>(pct * duration);
        controller.seek(target);
        state.status.positionMs = target;
        return;
    }
}

} // namespace

// ============================================================================
// Application Entry Point
// ============================================================================

int main(int argc, char** argv) {
    try {
        std::string screenshotPath;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
                screenshotPath = argv[++i];
            }
        }

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
            return 1;
        }

        constexpr int kInitW = resona::ui::OmnitrixTheme::kCanvasWidth;
        constexpr int kInitH = resona::ui::OmnitrixTheme::kCanvasHeight;
        SDL_Window* window = SDL_CreateWindow("Omnitrix iPod - Ben 10 Edition",
                                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                              kInitW, kInitH, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window) {
            std::cerr << "CreateWindow failed: " << SDL_GetError() << "\n";
            SDL_Quit();
            return 1;
        }

        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer) {
            std::cerr << "CreateRenderer failed: " << SDL_GetError() << "\n";
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        FontSet fonts;
        if (!fonts.load(renderer)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Omnitrix iPod", "Failed to load fonts", window);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        resona::Paths paths;
        resona::PlayerController controller{paths};
        resona::search::SearchService searchService{controller.searchHistory()};
        searchService.onRecordSearch = [&controller](const std::string& query) {
            controller.recordSearch(query);
        };
        searchService.onRemember = [&controller](const resona::Track& track) {
            controller.rememberTrack(track);
        };
        UiState state;
        state.searchService = &searchService;
#ifdef OMNITRIX_HAVE_DBUS
        std::optional<resona::tray::StatusNotifier> tray;
        tray.emplace();
        state.nativeTrayAvailable = tray->available();
        tray->onActivate = [&](int x, int y) {
            (void)x;
            (void)y;
            state.isTrayMode = false;
            state.isMiniPlayer = true;
            state.popupOpen = false;
        };
        tray->onScroll = [&](int delta) {
            applyScrollDelta(state, static_cast<double>(delta) / 120.0, controller);
        };
#endif

        // Populate Iconic Sample Tracks from design set
        auto makeSample = [](const std::string& id, const std::string& title, const std::string& artist,
                             int64_t durMs, const std::string& durText) {
            resona::Track t;
            t.id = id;
            t.title = title;
            t.artist = artist;
            t.album = "Omnitrix OST";
            t.durationMs = durMs;
            t.durationText = durText;
            return t;
        };

        std::vector<resona::Track> sampleTracks = {
            makeSample("alien-signal", "Alien signal", "Omnitrix Radio", 194000, "3:14"),
            makeSample("cosmic-drift", "Cosmic drift", "Omnitrix OST", 222000, "3:42"),
            makeSample("after-hours", "After hours on Earth", "Ben 10 Universe", 248000, "4:08"),
            makeSample("galvanic-pulse", "Galvanic pulse", "Galvan Prime", 210000, "3:30"),
            makeSample("somewhere-galaxy", "Somewhere in the galaxy", "Alien Force", 265000, "4:25"),
        };

        state.localTracks = controller.localTracks();
        state.demoMode = state.localTracks.empty();
        if (state.localTracks.empty()) {
            state.localTracks = sampleTracks;
        }

        state.status = controller.status();
        if (state.demoMode) {
            state.status.hasCurrent = true;
            state.status.current = state.localTracks[1]; // "Cosmic drift"
            state.status.durationMs = 222000;
            state.status.positionMs = 84000; // 1:24
            state.status.playing = true;
        }

        state.trackSelected = 1;
        state.screen = Screen::Home;

        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--sample") == 0) {
                state.demoMode = true;
                state.localTracks = sampleTracks;
                state.status.hasCurrent = true;
                state.status.current = sampleTracks[1];
                state.status.current.album = "Alien Force";
                state.status.queue = sampleTracks;
                for (size_t q = 0; q < 7; ++q) state.status.queue.push_back(sampleTracks[q % sampleTracks.size()]);
                state.status.queue[3] = state.status.current;
                state.status.durationMs = 222000;
                state.status.positionMs = 84000;
                state.status.playing = true;
            } else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
                std::string t = argv[++i];
                if (t == "white") state.chassisTheme = ChassisTheme::ClassicWhite;
                else if (t == "dark") state.chassisTheme = ChassisTheme::Dark;
            } else if (std::strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
                std::string v = argv[++i];
                if (v == "home") state.screen = Screen::Home;
                else if (v == "favorites") openFavorites(state);
                else if (v == "nowplaying") state.screen = Screen::NowPlaying;
                else if (v == "search") state.screen = Screen::Search;
                else if (v == "library" || v == "tracks") state.screen = Screen::Tracks;
                else if (v == "mini") state.isMiniPlayer = true;
                else if (v == "tray") state.isTrayMode = true;
            } else if (std::strcmp(argv[i], "--paused") == 0) {
                state.status.playing = false;
            }
        }

        bool running = true;
        int appliedWindowMode = -1;

        while (running) {
            Uint32 now = SDL_GetTicks();
            if (now - state.lastBlinkTime > 500) {
                state.cursorVisible = !state.cursorVisible;
                state.lastBlinkTime = now;
            }

            if (!state.demoMode) {
                state.status = controller.status();
                auto local = controller.localTracks();
                if (!local.empty()) state.localTracks = std::move(local);
                state.likedTracks = controller.liked();
                state.downloads.clear();
                for (const auto& download : controller.downloads()) {
                    resona::Track track;
                    track.id = download.id;
                    track.title = download.title;
                    track.artist = download.artist;
                    track.durationMs = download.durationMs;
                    track.durationText = resona::prettyDuration(download.durationMs);
                    state.downloads.push_back(std::move(track));
                }
                refreshPlaylistTracks(state, controller);
            }
            searchService.poll();
#ifdef OMNITRIX_HAVE_DBUS
            if (tray) {
                tray->poll();
                state.nativeTrayAvailable = tray->available();
                tray->setPlaying(state.status.playing);
                tray->setTitle(state.status.current.title.empty()
                                   ? "Omnitrix Player"
                                   : state.status.current.title);
            }
#endif

            int curW = kInitW, curH = kInitH;
            SDL_GetWindowSize(window, &curW, &curH);
            DeviceLayout layout = computeLayout(curW, curH);
            const float canvasScale = std::min(static_cast<float>(curW) / resona::ui::OmnitrixTheme::kCanvasWidth,
                                              static_cast<float>(curH) / resona::ui::OmnitrixTheme::kCanvasHeight);
            const int viewportW = std::max(1, static_cast<int>(resona::ui::OmnitrixTheme::kCanvasWidth * canvasScale));
            const int viewportH = std::max(1, static_cast<int>(resona::ui::OmnitrixTheme::kCanvasHeight * canvasScale));
            const SDL_Rect viewport{(curW - viewportW) / 2, (curH - viewportH) / 2, viewportW, viewportH};
            auto toLogicalX = [&](int value) {
                return (value - viewport.x) * resona::ui::OmnitrixTheme::kCanvasWidth / std::max(1, viewport.w);
            };
            auto toLogicalY = [&](int value) {
                return (value - viewport.y) * resona::ui::OmnitrixTheme::kCanvasHeight / std::max(1, viewport.h);
            };

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_QUIT:
                        running = false;
                        break;
                    case SDL_WINDOWEVENT:
                        if (event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
                        break;
                    case SDL_MOUSEBUTTONDOWN: {
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            int mx = event.button.x;
                            int my = event.button.y;
                            if (state.isTrayMode && !state.popupOpen) {
                                state.popupOpen = true;
                                state.popupX = mx;
                                state.popupY = my;
                                break;
                            }
                            if (!state.isMiniPlayer && !state.isTrayMode) {
                                mx = toLogicalX(mx);
                                my = toLogicalY(my);
                            }

                            if (state.isMiniPlayer) {
                                handleMiniPlayerClick(mx, my, curW, curH, state, controller);
                                break;
                            }

                            WheelRegion region = getWheelRegionAt(mx, my, layout);
                            if (region != WheelRegion::None) {
                                state.pressedRegion = region;
                                state.lastWheelAngle = getWheelAngleAt(mx, my, layout);
                                state.sweepAngle = state.lastWheelAngle;
                                state.accumulatedWheelAngle = 0.0;
                                state.wheelDidRotate = false;
                                state.rippleZone = region;
                                state.sweepUntil = SDL_GetTicks() + 350;
                                bool isOuter = (region == WheelRegion::Menu || region == WheelRegion::Next ||
                                                region == WheelRegion::PlayPause || region == WheelRegion::Previous);
                                state.isDraggingWheel = isOuter;
                            } else {
                                handleScreenClick(mx, my, layout, state, controller, running);
                            }
                        }
                        break;
                    }
                    case SDL_MOUSEMOTION: {
                        if (state.isDraggingWheel && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)) {
                            int mx = event.motion.x;
                            int my = event.motion.y;
                            if (!state.isMiniPlayer && !state.isTrayMode) {
                                mx = toLogicalX(mx);
                                my = toLogicalY(my);
                            }
                            double angle = getWheelAngleAt(mx, my, layout);
                            double delta = angle - state.lastWheelAngle;
                            if (delta > 3.14159265358979323846) delta -= 6.28318530717958647692;
                            if (delta < -3.14159265358979323846) delta += 6.28318530717958647692;

                            constexpr double kStepAngle = 0.24;
                            state.accumulatedWheelAngle += delta;
                            while (state.accumulatedWheelAngle >= kStepAngle) {
                                if (state.screen == Screen::NowPlaying) applyScrollDelta(state, 1.0, controller);
                                else stepSelection(state, 1, controller);
                                state.accumulatedWheelAngle -= kStepAngle;
                                state.wheelDidRotate = true;
                            }
                            while (state.accumulatedWheelAngle <= -kStepAngle) {
                                if (state.screen == Screen::NowPlaying) applyScrollDelta(state, -1.0, controller);
                                else stepSelection(state, -1, controller);
                                state.accumulatedWheelAngle += kStepAngle;
                                state.wheelDidRotate = true;
                            }
                            state.lastWheelAngle = angle;
                            state.sweepAngle = angle;
                        }
                        break;
                    }
                    case SDL_MOUSEBUTTONUP: {
                        if (event.button.button == SDL_BUTTON_LEFT) {
                            if (!state.wheelDidRotate && state.pressedRegion != WheelRegion::None) {
                                switch (state.pressedRegion) {
                                    case WheelRegion::Center:
                                        activateSelection(state, controller, running);
                                        break;
                                    case WheelRegion::Menu:
                                    case WheelRegion::InnerPrev:
                                        goBack(state);
                                        break;
                                    case WheelRegion::Previous:
                                        controller.previous();
                                        state.status = controller.status();
                                        break;
                                    case WheelRegion::Next:
                                        controller.next();
                                        state.status = controller.status();
                                        break;
                                    case WheelRegion::PlayPause:
                                        controller.togglePause();
                                        state.status = controller.status();
                                        break;
                                    case WheelRegion::InnerUp:
                                        stepSelection(state, -1, controller);
                                        break;
                                    case WheelRegion::InnerDown:
                                        stepSelection(state, 1, controller);
                                        break;
                                    case WheelRegion::InnerEnter:
                                        activateSelection(state, controller, running);
                                        break;
                                    case WheelRegion::None:
                                        break;
                                }
                            }
                            state.pressedRegion = WheelRegion::None;
                            state.isDraggingWheel = false;
                        }
                        break;
                    }
                    case SDL_MOUSEWHEEL: {
                        double dy = static_cast<double>(event.wheel.y);
                        if (event.wheel.preciseY != 0.0f) dy = event.wheel.preciseY;
                        applyScrollDelta(state, dy, controller);
                        break;
                    }
                    case SDL_KEYDOWN: {
                        const SDL_Keycode key = event.key.keysym.sym;
                        if (state.screen == Screen::Search) {
                            const bool allowed = key == SDLK_BACKSPACE || key == SDLK_RETURN ||
                                                 key == SDLK_KP_ENTER || key == SDLK_ESCAPE ||
                                                 key == SDLK_UP || key == SDLK_DOWN;
                            if (!allowed) break;
                        }
                        switch (key) {
                            case SDLK_t:
                            case SDLK_m:
                                state.isMiniPlayer = !state.isMiniPlayer;
                                state.isTrayMode = false;
                                state.popupOpen = false;
                                break;
                            case SDLK_u:
                                state.isTrayMode = !state.isTrayMode;
                                state.isMiniPlayer = false;
                                state.popupOpen = false;
                                break;
                            case SDLK_c:
                                state.chassisTheme = state.chassisTheme == ChassisTheme::Dark
                                                         ? ChassisTheme::ClassicWhite
                                                         : ChassisTheme::Dark;
                                break;
                            case SDLK_h:
                                state.screen = Screen::Home;
                                break;
                            case SDLK_f:
                                openFavorites(state);
                                break;
                            case SDLK_s: {
                                const std::array<TrackListType, 4> sections{
                                    TrackListType::Local, TrackListType::Liked,
                                    TrackListType::Downloads, TrackListType::Queue};
                                for (size_t i = 0; i < sections.size(); ++i) {
                                    if (state.listType == sections[i]) {
                                        state.listType = sections[(i + 1) % sections.size()];
                                        break;
                                    }
                                }
                                state.screen = Screen::Tracks;
                                state.trackSelected = 0;
                                break;
                            }
                            case SDLK_p:
                                state.screen = Screen::Playlists;
                                break;
                            case SDLK_n: {
                                const auto playlist = controller.createPlaylist("New Playlist");
                                state.activePlaylistId = playlist.id;
                                state.screen = Screen::Playlists;
                                break;
                            }
                            case SDLK_a:
                                if (!state.activePlaylistId.empty() && !state.status.current.id.empty()) {
                                    controller.addToPlaylist(state.activePlaylistId, state.status.current.id);
                                } else if (!state.status.current.id.empty()) {
                                    auto playlists = controller.playlists();
                                    if (playlists.empty()) playlists.push_back(controller.createPlaylist("Favorites"));
                                    controller.addToPlaylist(playlists.front().id, state.status.current.id);
                                }
                                break;
                            case SDLK_d: {
                                auto playlists = controller.playlists();
                                if (!playlists.empty()) {
                                    controller.deletePlaylist(playlists.back().id);
                                    if (state.activePlaylistId == playlists.back().id) state.activePlaylistId.clear();
                                }
                                break;
                            }
                            case SDLK_UP:
                                stepSelection(state, -1, controller);
                                break;
                            case SDLK_DOWN:
                                stepSelection(state, 1, controller);
                                break;
                            case SDLK_LEFT:
                                controller.previous();
                                state.status = controller.status();
                                break;
                            case SDLK_RIGHT:
                                controller.next();
                                state.status = controller.status();
                                break;
                            case SDLK_SPACE:
                                controller.togglePause();
                                state.status = controller.status();
                                break;
                            case SDLK_ESCAPE:
                                if (state.screen == Screen::Search && !state.searchQuery.empty()) {
                                    state.searchQuery.clear();
                                    state.trackSelected = 0;
                                    state.firstVisibleTrack = 0;
                                    if (state.searchService) state.searchService->clear();
                                } else {
                                    goBack(state);
                                }
                                break;
                            case SDLK_BACKSPACE:
                                if (state.screen == Screen::Search && !state.searchQuery.empty()) {
                                    state.searchQuery.pop_back();
                                    if (state.searchService) state.searchService->onQueryChange(state.searchQuery);
                                } else {
                                    goBack(state);
                                }
                                break;
                            case SDLK_RETURN:
                            case SDLK_KP_ENTER:
                                if (state.isMiniPlayer) {
                                    state.isMiniPlayer = false;
                                } else if (state.screen == Screen::Search && !state.searchQuery.empty()) {
                                    triggerSearch(controller, state);
                                } else {
                                    activateSelection(state, controller, running);
                                }
                                break;
                        }
                        break;
                    }
                    case SDL_TEXTINPUT: {
                        if (state.screen == Screen::Search) {
                            state.searchQuery += event.text.text;
                            if (state.searchService) state.searchService->onQueryChange(state.searchQuery);
                        }
                        break;
                    }
                }
            }

            const int windowMode = state.isTrayMode
                                       ? (state.popupOpen ? 2 : (state.nativeTrayAvailable ? 1 : 4))
                                       : (state.isMiniPlayer ? 3 : 0);
            if (windowMode != appliedWindowMode) {
                appliedWindowMode = windowMode;
                if (windowMode == 1) {
                    SDL_HideWindow(window);
                } else if (windowMode == 2) {
                    SDL_SetWindowBordered(window, SDL_FALSE);
                    SDL_SetWindowAlwaysOnTop(window, SDL_TRUE);
                    SDL_SetWindowSize(window, 312, 128);
                    SDL_SetWindowPosition(window, std::max(0, state.popupX - 156),
                                          std::max(0, state.popupY - 16));
                    SDL_ShowWindow(window);
                } else if (windowMode == 3) {
                    SDL_SetWindowBordered(window, SDL_FALSE);
                    SDL_SetWindowAlwaysOnTop(window, SDL_TRUE);
                    SDL_SetWindowSize(window, 312, 128);
                    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    SDL_ShowWindow(window);
                } else {
                    SDL_SetWindowAlwaysOnTop(window, SDL_FALSE);
                    SDL_SetWindowBordered(window, SDL_TRUE);
                    SDL_SetWindowSize(window, resona::ui::OmnitrixTheme::kCanvasWidth,
                                      resona::ui::OmnitrixTheme::kCanvasHeight);
                    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    SDL_ShowWindow(window);
                }
            }

            // Canvas Background
            SDL_RenderSetScale(renderer, 1.0f, 1.0f);
            SDL_RenderSetViewport(renderer, nullptr);
            SDL_SetRenderDrawColor(renderer, Palette::kDeskBg.r, Palette::kDeskBg.g, Palette::kDeskBg.b, 255);
            SDL_RenderClear(renderer);

            if (state.isTrayMode && !state.popupOpen && state.nativeTrayAvailable) {
            } else if (state.isMiniPlayer || (state.isTrayMode && state.popupOpen)) {
                drawMiniPlayerCard(renderer, curW, curH, state, fonts, controller);
            } else if (state.isTrayMode) {
                drawTrayBadge(renderer, curW, curH, state);
            } else {
                SDL_RenderSetViewport(renderer, &viewport);
                SDL_RenderSetScale(renderer, static_cast<float>(viewport.w) / resona::ui::OmnitrixTheme::kCanvasWidth,
                                   static_cast<float>(viewport.h) / resona::ui::OmnitrixTheme::kCanvasHeight);
                drawDeviceChassis(renderer, layout, state.pressedRegion, state.isDraggingWheel, state.lastWheelAngle,
                                  state.sweepAngle, state.sweepUntil, state.rippleZone, nullptr, state);

                drawTitleBar(renderer, layout, state, fonts);

                switch (state.screen) {
                    case Screen::Home:
                        drawHomeScreen(renderer, layout, state, fonts);
                        break;
                    case Screen::Tracks:
                    case Screen::Menu:
                        drawTrackListScreen(renderer, layout, state, fonts);
                        break;
                    case Screen::NowPlaying:
                        drawNowPlayingScreen(renderer, layout, state, fonts, controller);
                        break;
                    case Screen::Search:
                        drawSearchScreen(renderer, layout, state, fonts, controller);
                        break;
                    case Screen::Playlists:
                        drawPlaylistsScreen(renderer, layout, state, fonts);
                        break;
                    case Screen::Boot:
                        drawBootScreen(renderer, layout, state, fonts);
                        break;
                }
                SDL_RenderSetScale(renderer, 1.0f, 1.0f);
                SDL_RenderSetViewport(renderer, nullptr);
            }

            if (!screenshotPath.empty()) {
                static int frameCount = 0;
                if (++frameCount >= 2) {
                    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, curW, curH, 32, SDL_PIXELFORMAT_RGBA32);
                    if (s) {
                        SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, s->pixels, s->pitch);
                        SDL_SaveBMP(s, screenshotPath.c_str());
                        SDL_FreeSurface(s);
                    }
                    running = false;
                }
            }

            SDL_RenderPresent(renderer);
            SDL_Delay(12);
        }

        controller.stop();
        fonts.destroy();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
