#include "youtube/extractor.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace resona {

namespace {

constexpr const char* kPlayerKey = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";

std::string stringValue(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

int64_t intValue(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return 0;
    const auto it = value.find(key);
    if (it == value.end()) return 0;
    if (it->is_number()) return it->get<int64_t>();
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

std::map<std::string, std::string> parseQuery(const std::string& value) {
    std::map<std::string, std::string> result;
    size_t start = 0;
    while (start <= value.size()) {
        const auto amp = value.find('&', start);
        const auto part = value.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const auto equals = part.find('=');
        if (equals != std::string::npos) {
            result[urlDecode(part.substr(0, equals))] = urlDecode(part.substr(equals + 1));
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return result;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}  // namespace

StreamExtractor::StreamExtractor(Http& http, Visitor& visitor) : http_(http), visitor_(visitor) {
    clients_ = {
        {"VISIONOS", "101", "0.1",
         "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15",
         0, true, "", {{"osName", "visionOS"}, {"osVersion", "1.3.21O771"}, {"deviceMake", "Apple"}, {"deviceModel", "RealityDevice14,1"}}, false},
        {"ANDROID_VR", "28", "1.65.10",
         "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",
         32, true, "", {{"osName", "Android"}, {"osVersion", "12L"}, {"deviceMake", "Oculus"}, {"deviceModel", "Quest 3"}}, false},
        {"ANDROID", "3", "21.02.35", "com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip",
         30, true, "", {{"osName", "Android"}, {"osVersion", "11"}}, false},
        {"TVHTML5_SIMPLY_EMBEDDED_PLAYER", "85", "2.0",
         "Mozilla/5.0 (SMART-TV; LINUX; Tizen 6.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/6.0 TV Safari/538.1",
         0, true, "", {}, true},
        {"IOS", "5", "21.02.3", "com.google.ios.youtube/21.02.3 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
         0, true, "", {{"osName", "iPhone"}, {"osVersion", "18.3.2.22D82"}, {"deviceMake", "Apple"}, {"deviceModel", "iPhone16,2"}}, false},
        {"MWEB", "2", "2.20260115.01.00",
         "Mozilla/5.0 (iPad; CPU OS 16_7_10 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1,gzip(gfe)",
         0, false, kPlayerKey, {}, false},
        {"WEB", "1", "2.20260114.08.00",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
         0, true, kPlayerKey, {{"originalUrl", "https://www.youtube.com"}, {"platform", "DESKTOP"}, {"utcOffsetMinutes", "0"}}, false},
    };
}

StreamExtractor::Attempt StreamExtractor::fetch(const std::string& videoId, const ClientConfig& config,
                                                const std::optional<std::string>& visitor) {
    nlohmann::json client = {
        {"clientName", config.name},
        {"clientVersion", config.version},
        {"userAgent", config.userAgent},
        {"hl", "en"},
        {"gl", "US"},
    };
    if (config.sdk != 0) client["androidSdkVersion"] = config.sdk;
    if (visitor && !visitor->empty()) client["visitorData"] = *visitor;
    for (const auto& [key, value] : config.extra) client[key] = value;

    nlohmann::json body;
    body["context"]["client"] = client;
    body["videoId"] = videoId;
    if (config.thirdPartyEmbed) body["thirdParty"]["embedUrl"] = "https://www.youtube.com/watch?v=" + videoId;
    if (config.playbackContext) {
        body["playbackContext"]["contentPlaybackContext"]["html5Preference"] = "HTML5_PREF_WANTS";
    }
    body["racyCheckOk"] = true;
    body["contentCheckOk"] = true;

    std::string url = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
    if (!config.apiKey.empty()) url += "&key=" + config.apiKey;

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Origin", "https://www.youtube.com"},
        {"Referer", "https://www.youtube.com/"},
        {"X-YouTube-Client-Name", config.number},
        {"X-YouTube-Client-Version", config.version},
    };
    if (visitor && !visitor->empty()) headers.emplace_back("X-Goog-Visitor-Id", *visitor);

    const auto response = http_.post(url, body.dump(), headers, config.userAgent);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("Player request failed with HTTP " + std::to_string(response.status));
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response.body);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Player response was not valid JSON");
    }
    const auto learned = stringValue(parsed.value("responseContext", nlohmann::json::object()), "visitorData");
    if (!learned.empty()) visitor_.seed(learned);
    return {std::move(parsed), visitor.value_or(""), config};
}

std::optional<StreamSource> StreamExtractor::sourceFrom(const nlohmann::json& response, const ClientConfig& config) {
    if (stringValue(response.value("playabilityStatus", nlohmann::json::object()), "status") != "OK") return std::nullopt;
    const auto streamingIt = response.find("streamingData");
    if (streamingIt == response.end() || !streamingIt->is_object()) return std::nullopt;

    std::vector<nlohmann::json> formats;
    for (const char* key : {"adaptiveFormats", "formats"}) {
        const auto it = streamingIt->find(key);
        if (it == streamingIt->end() || !it->is_array()) continue;
        for (const auto& format : *it) formats.push_back(format);
    }
    std::sort(formats.begin(), formats.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return intValue(a, "bitrate") > intValue(b, "bitrate");
    });

    for (const bool direct : {true, false}) {
        for (const auto& format : formats) {
            const auto mime = lower(stringValue(format, "mimeType"));
            if (mime.rfind("audio/", 0) != 0) continue;
            std::string url = stringValue(format, "url");
            if (url.empty() && !direct) {
                const auto cipher = parseQuery(stringValue(format, "signatureCipher"));
                const auto urlIt = cipher.find("url");
                if (urlIt != cipher.end()) url = urlIt->second;
            }
            if (url.empty() || (!direct && !stringValue(format, "url").empty())) continue;
            return StreamSource{url, config.userAgent, config.name};
        }
    }
    return std::nullopt;
}

StreamSource StreamExtractor::resolve(const std::string& videoId, const std::set<std::string>& excluded) {
    visitor_.ensure(videoId);
    const auto initialVisitor = visitor_.current();

    auto attemptChain = [&](const std::optional<std::string>& visitor) -> std::optional<StreamSource> {
        std::string lastError;
        for (const auto& config : clients_) {
            if (excluded.count(config.name) != 0) continue;
            try {
                const auto response = fetch(videoId, config, visitor);
                if (auto source = sourceFrom(response.body, config)) return source;
                const auto reason = stringValue(response.body.value("playabilityStatus", nlohmann::json::object()), "reason");
                lastError = reason.empty() ? config.name + " returned no playable audio" : reason;
            } catch (const std::exception& error) {
                lastError = std::string(error.what());
            }
        }
        if (!lastError.empty()) throw std::runtime_error("No playable stream: " + lastError);
        throw std::runtime_error("Every InnerTube client was excluded");
    };

    try {
        return *attemptChain(initialVisitor);
    } catch (const std::exception& firstError) {
        const auto learned = visitor_.current();
        if (!learned || learned == initialVisitor) throw;
        try {
            return *attemptChain(learned);
        } catch (...) {
            throw firstError;
        }
    }
}

void StreamExtractor::refreshVisitor() {
    visitor_.remint();
}

}  // namespace resona
