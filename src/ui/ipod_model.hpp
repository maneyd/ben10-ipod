#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace resona::ui {

enum class WheelZone {
    None,
    Center,
    // Inner navigation band: up(top), down(bottom), enter(left), prev(right)
    InnerUp,
    InnerDown,
    InnerEnter,
    InnerPrev,
    // Outer ring quadrants (unchanged)
    Menu,
    Next,
    PlayPause,
    Previous
};

inline WheelZone classifyWheelZone(double dx, double dy,
                                   double centerRadius, double innerRadius, double outerRadius) {
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist <= centerRadius) return WheelZone::Center;
    if (dist > outerRadius) return WheelZone::None;

    const double angleDeg = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;

    // Inner ring band: centerRadius < dist <= innerRadius
    if (dist <= innerRadius) {
        if (angleDeg >= -135.0 && angleDeg < -45.0) return WheelZone::InnerUp;
        if (angleDeg >= 45.0 && angleDeg < 135.0) return WheelZone::InnerDown;
        if (angleDeg >= -45.0 && angleDeg < 45.0) return WheelZone::InnerPrev;
        return WheelZone::InnerEnter;
    }

    // Outer ring quadrants
    if (angleDeg >= -135.0 && angleDeg < -45.0) return WheelZone::Menu;
    if (angleDeg >= -45.0 && angleDeg < 45.0) return WheelZone::Next;
    if (angleDeg >= 45.0 && angleDeg < 135.0) return WheelZone::PlayPause;
    return WheelZone::Previous;
}

inline bool enterPlaysTrack(bool hasResults, bool hasQuery) {
    if (hasResults) return true;
    if (hasQuery) return true;
    return false;
}

inline int stepSelection(int current, int delta, std::size_t count) {
    if (count == 0) return 0;
    const int size = static_cast<int>(count);
    int next = (current + delta) % size;
    if (next < 0) next += size;
    return next;
}

inline int progressPercent(int64_t positionMs, int64_t durationMs) {
    if (durationMs <= 0) return 0;
    const int64_t percent = positionMs * 100 / durationMs;
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return static_cast<int>(percent);
}

inline int volumePercent(float volume) {
    const float percent = volume * 50.0f;
    if (percent <= 0) return 0;
    if (percent >= 100) return 100;
    return static_cast<int>(percent);
}

inline std::string centeredLabel(const std::string& text, int width) {
    if (width <= 0 || text.size() >= static_cast<std::size_t>(width)) return text;
    const int padding = (width - static_cast<int>(text.size())) / 2;
    return std::string(static_cast<std::size_t>(padding), ' ') + text;
}

}  // namespace resona::ui
