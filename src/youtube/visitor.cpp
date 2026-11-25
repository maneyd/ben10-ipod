#include "youtube/visitor.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>

#include "domain/models.hpp"

namespace resona {

Visitor::Visitor(const Paths& paths, Http& http) : http_(http) {
    tokenFile_ = (paths.configDir() / "visitor.txt").string();
    std::ifstream input(tokenFile_);
    if (input.good()) std::getline(input, token_);
}

std::optional<std::string> Visitor::current() const {
    if (token_.empty()) return std::nullopt;
    return token_;
}

void Visitor::seed(const std::string& token) {
    const std::string normalized = urlDecode(token);
    if (normalized.empty() || normalized == token_) return;
    token_ = normalized;
    std::ofstream output(tokenFile_, std::ios::trunc);
    output << token_;
}

void Visitor::clear() {
    token_.clear();
    std::error_code ec;
    std::filesystem::remove(tokenFile_, ec);
}

void Visitor::ensure(const std::string& videoId) {
    if (!token_.empty()) return;
    (void)fetchPlayerJsUrl(videoId);
}

std::optional<std::string> Visitor::fetchPlayerJsUrl(const std::string& videoId) {
    const auto response = http_.get("https://www.youtube.com/watch?v=" + videoId, {},
                                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    const std::string& html = response.body;
    const std::regex visitorPattern(R"RX("VISITOR_DATA":"([^"]+)")RX");
    std::smatch match;
    if (std::regex_search(html, match, visitorPattern) && match.size() > 1) seed(match[1].str());

    const std::regex jsPattern(R"RX("jsUrl"\s*:\s*"(\/s\/player\/[a-f0-9]+\/[^"]+base\.js)")RX");
    const std::regex fallbackPattern(R"RX((/s\/player\/[a-f0-9]+\/player_ias\.vflset\/[^"]+base\.js))RX");
    if (std::regex_search(html, match, jsPattern) && match.size() > 1) {
        const std::string url = match[1].str();
        return url.rfind("http", 0) == 0 ? url : "https://www.youtube.com" + url;
    }
    if (std::regex_search(html, match, fallbackPattern) && match.size() > 1) {
        const std::string url = match[1].str();
        return url.rfind("http", 0) == 0 ? url : "https://www.youtube.com" + url;
    }
    return std::nullopt;
}

std::optional<std::string> Visitor::remint() {
    const std::string body =
        R"({"context":{"client":{"clientName":"WEB","clientVersion":"2.20260114.08.00","hl":"en","gl":"US"}}})";
    const auto response = http_.post("https://www.youtube.com/youtubei/v1/visitor_id?prettyPrint=false", body,
                                     {{"Content-Type", "application/json"}},
                                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    const std::regex pattern(R"RX("visitorData"\s*:\s*"([^"]+)")RX");
    std::smatch match;
    if (std::regex_search(response.body, match, pattern) && match.size() > 1) {
        seed(match[1].str());
        return current();
    }
    return std::nullopt;
}

}  // namespace resona
