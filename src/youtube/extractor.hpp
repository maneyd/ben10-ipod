#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "domain/models.hpp"
#include "network/http.hpp"
#include "youtube/visitor.hpp"

namespace resona {

struct ClientConfig {
    std::string name;
    std::string number;
    std::string version;
    std::string userAgent;
    int sdk = 0;
    bool playbackContext = false;
    std::string apiKey;
    std::vector<std::pair<std::string, std::string>> extra;
    bool thirdPartyEmbed = false;
};

class StreamExtractor {
public:
    StreamExtractor(Http& http, Visitor& visitor);

    StreamSource resolve(const std::string& videoId, const std::set<std::string>& excluded = {});
    void refreshVisitor();

private:
    struct Attempt {
        nlohmann::json body;
        std::string visitor;
        ClientConfig config;
    };

    Attempt fetch(const std::string& videoId, const ClientConfig& config, const std::optional<std::string>& visitor);
    std::optional<StreamSource> sourceFrom(const nlohmann::json& response, const ClientConfig& config);

    Http& http_;
    Visitor& visitor_;
    std::vector<ClientConfig> clients_;
};

}  // namespace resona
