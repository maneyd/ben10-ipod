#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

#include "domain/models.hpp"
#include "network/http.hpp"
#include "youtube/visitor.hpp"

namespace resona {

std::vector<Track> extractFilteredSongs(const nlohmann::json& root);
std::optional<std::string> extractSearchContinuation(const nlohmann::json& body);

class InnerTube {
public:
    InnerTube(Http& http, Visitor& visitor);

    std::vector<Track> searchSongs(const std::string& query);
    std::vector<Track> searchVideos(const std::string& query);
    std::vector<Track> searchAll(const std::string& query);

    SongSearchPage searchSongsPage(const std::string& query);
    SongSearchPage loadMoreSongResults(const std::string& continuation);
    std::vector<ArtistSummary> searchArtists(const std::string& query);

private:
    nlohmann::json searchRequest(const std::string& query, const std::string& params);
    nlohmann::json continuationRequest(const std::string& continuation);

    Http& http_;
    Visitor& visitor_;
};

}  // namespace resona
