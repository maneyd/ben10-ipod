#include "domain/models.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <sstream>

namespace resona {

std::string trackIdForVideo(const std::string& videoId) {
    return "yt:" + videoId;
}

std::string trackIdForPath(const std::string& path) {
    const auto hash = std::hash<std::string>{}(path);
    std::ostringstream out;
    out << "local:" << std::hex << hash;
    return out.str();
}

std::string prettyDuration(int64_t ms) {
    if (ms <= 0) return "--:--";
    const int64_t total = ms / 1000;
    const int64_t hours = total / 3600;
    const int64_t minutes = (total % 3600) / 60;
    const int64_t seconds = total % 60;
    std::ostringstream out;
    if (hours > 0) {
        out << hours << ':' << std::setw(2) << std::setfill('0') << minutes << ':'
            << std::setw(2) << std::setfill('0') << seconds;
    } else {
        out << minutes << ':' << std::setw(2) << std::setfill('0') << seconds;
    }
    return out.str();
}

int64_t parseDurationMs(const std::string& text) {
    std::vector<int> parts;
    std::string current;
    for (char ch : text) {
        if (ch == ':') {
            if (current.empty() || !std::all_of(current.begin(), current.end(), [](unsigned char c) { return std::isdigit(c); })) {
                return 0;
            }
            parts.push_back(std::stoi(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (current.empty() || !std::all_of(current.begin(), current.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return 0;
    }
    parts.push_back(std::stoi(current));
    if (parts.empty() || parts.size() > 3) return 0;
    int64_t seconds = 0;
    for (int part : parts) seconds = seconds * 60 + part;
    return seconds * 1000;
}

std::string urlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto hexDigit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = hexDigit(value[i + 1]);
            const int low = hexDigit(value[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+') out.push_back(' ');
        else out.push_back(value[i]);
    }
    return out;
}

}  // namespace resona
