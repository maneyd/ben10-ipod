#include "youtube/innertube.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace resona {

namespace {

constexpr const char* kApiKey = "AIzaSyC9XL3ZjWddXya6X74dJoCTL-WEYFDNX30";
constexpr const char* kClientVersion = "1.20240101.01.00";
constexpr const char* kSongsParams = "EgWKAQIIAWoKEAoQAxAEEAkQBQ%3D%3D";
constexpr const char* kVideosParams = "EgWKAQIQAWoKEAoQAxAEEAkQBQ%3D%3D";
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

std::string stringValue(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool isDuration(const std::string& value) {
    static const std::regex pattern(R"(^\d{1,2}(:\d{2}){1,2}$)");
    return std::regex_match(value, pattern);
}

const nlohmann::json* findKey(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return nullptr;
    const auto it = value.find(key);
    if (it == value.end()) return nullptr;
    return &*it;
}

std::vector<const nlohmann::json*> findRenderers(const nlohmann::json& root) {
    std::vector<const nlohmann::json*> result;
    if (root.is_object()) {
        if (const auto* renderer = findKey(root, "musicResponsiveListItemRenderer")) result.push_back(renderer);
        for (const auto& item : root) {
            auto nested = findRenderers(item);
            result.insert(result.end(), nested.begin(), nested.end());
        }
    } else if (root.is_array()) {
        for (const auto& item : root) {
            auto nested = findRenderers(item);
            result.insert(result.end(), nested.begin(), nested.end());
        }
    }
    return result;
}

std::optional<std::string> findContinuation(const nlohmann::json& root) {
    if (root.is_object()) {
        if (const auto* continuations = findKey(root, "continuations"); continuations && continuations->is_array()) {
            for (const auto& item : *continuations) {
                const auto token = stringValue(item.value("nextContinuationData", nlohmann::json::object()), "continuation");
                if (!token.empty()) return token;
            }
        }
        for (const auto& item : root) {
            if (auto token = findContinuation(item)) return token;
        }
    } else if (root.is_array()) {
        for (const auto& item : root) {
            if (auto token = findContinuation(item)) return token;
        }
    }
    return std::nullopt;
}

Track parseRenderer(const nlohmann::json& renderer) {
    const auto* flexColumns = findKey(renderer, "flexColumns");
    if (flexColumns == nullptr || !flexColumns->is_array() || flexColumns->empty()) return {};

    auto runsOf = [](const nlohmann::json& columns, size_t index) -> std::vector<nlohmann::json> {
        if (!columns.is_array() || columns.size() <= index) return {};
        const auto* flex = findKey(columns[index], "musicResponsiveListItemFlexColumnRenderer");
        if (flex == nullptr) return {};
        const auto* text = findKey(*flex, "text");
        if (text == nullptr) return {};
        const auto* runs = findKey(*text, "runs");
        if (runs == nullptr || !runs->is_array()) return {};
        return std::vector<nlohmann::json>(runs->begin(), runs->end());
    };

    const auto titleRuns = runsOf(*flexColumns, 0);
    if (titleRuns.empty()) return {};
    const auto& titleRun = titleRuns.front();
    const std::string title = stringValue(titleRun, "text");
    const auto* endpoint = findKey(titleRun, "navigationEndpoint");
    const auto* watch = endpoint == nullptr ? nullptr : findKey(*endpoint, "watchEndpoint");
    const std::string videoId = watch == nullptr ? "" : stringValue(*watch, "videoId");
    if (title.empty() || videoId.empty()) return {};

    const auto subtitleRuns = runsOf(*flexColumns, 1);
    std::vector<std::string> subtitles;
    for (const auto& run : subtitleRuns) {
        const std::string text = stringValue(run, "text");
        if (!text.empty() && text != " • ") subtitles.push_back(text);
    }

    std::string durationText;
    std::string artist;
    for (const auto& item : subtitles) {
        if (durationText.empty() && isDuration(item)) durationText = item;
        if (artist.empty() && !isDuration(item)) artist = item;
    }

    std::string thumbnail;
    const auto* thumbnailObj = findKey(renderer, "thumbnail");
    if (thumbnailObj != nullptr) {
        const auto* music = findKey(*thumbnailObj, "musicThumbnailRenderer");
        if (music != nullptr) {
            const auto* thumb = findKey(*music, "thumbnail");
            if (thumb != nullptr) {
                const auto* thumbs = findKey(*thumb, "thumbnails");
                if (thumbs != nullptr && thumbs->is_array() && !thumbs->empty()) {
                    thumbnail = stringValue(thumbs->back(), "url");
                }
            }
        }
    }

    Track track;
    track.source = TrackSource::YouTube;
    track.id = trackIdForVideo(videoId);
    track.videoId = videoId;
    track.title = title;
    track.artist = artist.empty() ? "Unknown Artist" : artist;
    track.durationText = durationText;
    track.durationMs = parseDurationMs(durationText);
    track.location = thumbnail;
    return track;
}

void appendUnique(std::vector<Track>& target, const std::vector<Track>& source) {
    std::set<std::string> seen;
    for (const auto& track : target) seen.insert(track.id);
    for (const auto& sourceTrack : source) {
        if (sourceTrack.id.empty() || seen.count(sourceTrack.id) != 0) continue;
        seen.insert(sourceTrack.id);
        target.push_back(sourceTrack);
    }
}

std::string normalizedForMatching(const std::string& value) {
    std::string result;
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

std::string trackKey(const Track& track) {
    return normalizedForMatching(track.title) + "|" + normalizedForMatching(track.artist);
}

std::vector<Track> dedupeByVideoId(const std::vector<Track>& tracks) {
    std::set<std::string> seen;
    std::vector<Track> result;
    for (const auto& track : tracks) {
        if (track.videoId.empty() || seen.count(track.videoId) != 0) continue;
        seen.insert(track.videoId);
        result.push_back(track);
    }
    return result;
}

// Mirrors MusicRepositoryImpl.mergeInVideoUploads(): the Videos shelf carries
// regional uploads missing from YTM's catalog, but also karaoke/reaction noise.
// Keep a video only if its uploader is already established by the Songs shelf.
std::vector<Track> mergeInVideoUploads(const std::vector<Track>& songs, const std::vector<Track>& videos) {
    if (videos.empty()) return songs;

    std::set<std::string> knownArtists;
    std::set<std::string> songIds;
    std::set<std::string> seenTracks;
    for (const auto& song : songs) {
        knownArtists.insert(normalizedForMatching(song.artist));
        songIds.insert(song.videoId);
        seenTracks.insert(trackKey(song));
    }

    std::map<size_t, Track> keptByRank;
    for (size_t rank = 0; rank < videos.size(); ++rank) {
        const auto& video = videos[rank];
        const bool byKnownArtist = songs.empty() ||
                                   knownArtists.count(normalizedForMatching(video.artist)) != 0;
        if (!byKnownArtist || songIds.count(video.videoId) != 0) continue;
        if (!seenTracks.insert(trackKey(video)).second) continue;
        keptByRank[rank] = video;
    }
    if (keptByRank.empty()) return songs;

    std::vector<Track> merged;
    const size_t count = std::max(songs.size(), videos.size());
    for (size_t rank = 0; rank < count; ++rank) {
        if (rank < songs.size()) merged.push_back(songs[rank]);
        if (auto it = keptByRank.find(rank); it != keptByRank.end()) merged.push_back(it->second);
    }
    return merged;
}

std::vector<ArtistSummary> parseArtists(const nlohmann::json& body) {
    std::vector<ArtistSummary> result;
    std::set<std::string> seen;
    for (const auto* renderer : findRenderers(body)) {
        const auto* flexColumns = findKey(*renderer, "flexColumns");
        if (flexColumns == nullptr || !flexColumns->is_array() || flexColumns->empty()) continue;
        const auto* flex = findKey((*flexColumns)[0], "musicResponsiveListItemFlexColumnRenderer");
        if (flex == nullptr) continue;
        const auto* text = findKey(*flex, "text");
        if (text == nullptr) continue;
        const auto* runs = findKey(*text, "runs");
        if (runs == nullptr || !runs->is_array() || runs->empty()) continue;

        std::string name;
        std::string browseId;
        for (const auto& run : *runs) {
            if (name.empty()) name = stringValue(run, "text");
            const auto* endpoint = findKey(run, "navigationEndpoint");
            if (endpoint == nullptr) continue;
            const auto* browse = findKey(*endpoint, "browseEndpoint");
            if (browse != nullptr) browseId = stringValue(*browse, "browseId");
        }
        if (name.empty() || browseId.empty() || browseId.rfind("UC", 0) != 0) continue;
        if (!seen.insert(browseId).second) continue;

        ArtistSummary artist;
        artist.id = browseId;
        artist.name = name;
        const auto* thumbnailObj = findKey(*renderer, "thumbnail");
        if (thumbnailObj != nullptr) {
            const auto* music = findKey(*thumbnailObj, "musicThumbnailRenderer");
            if (music != nullptr) {
                const auto* thumb = findKey(*music, "thumbnail");
                if (thumb != nullptr) {
                    const auto* thumbs = findKey(*thumb, "thumbnails");
                    if (thumbs != nullptr && thumbs->is_array() && !thumbs->empty()) {
                        artist.thumbnail = stringValue(thumbs->back(), "url");
                    }
                }
            }
        }
        result.push_back(std::move(artist));
    }
    return result;
}

}  // namespace


std::vector<Track> extractFilteredSongs(const nlohmann::json& body) {
    const auto* root = findKey(body, "continuationContents");
    if (root == nullptr) root = findKey(body, "contents");
    if (root == nullptr) return {};
    std::vector<Track> result;
    for (const auto* renderer : findRenderers(*root)) {
        Track track = parseRenderer(*renderer);
        if (!track.videoId.empty()) result.push_back(track);
    }
    return result;
}

std::optional<std::string> extractSearchContinuation(const nlohmann::json& body) {
    const auto* root = findKey(body, "continuationContents");
    if (root == nullptr) root = findKey(body, "contents");
    if (root == nullptr) return std::nullopt;
    return findContinuation(*root);
}

InnerTube::InnerTube(Http& http, Visitor& visitor) : http_(http), visitor_(visitor) {}

nlohmann::json InnerTube::searchRequest(const std::string& query, const std::string& params) {
    const std::string url = std::string("https://music.youtube.com/youtubei/v1/search?key=") + kApiKey + "&prettyPrint=false";
    nlohmann::json body;
    body["context"]["client"]["clientName"] = "WEB_REMIX";
    body["context"]["client"]["clientVersion"] = kClientVersion;
    body["query"] = query;
    if (!params.empty()) body["params"] = params;

    const auto response = http_.post(url, body.dump(),
                                     {
                                         {"Content-Type", "application/json"},
                                         {"Origin", "https://music.youtube.com"},
                                         {"Referer", "https://music.youtube.com/"},
                                         {"X-Goog-Api-Format-Version", "1"},
                                     },
                                     kUserAgent);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("InnerTube search failed with HTTP " + std::to_string(response.status));
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response.body);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("InnerTube search returned invalid JSON");
    }
    const auto visitor = stringValue(parsed.value("responseContext", nlohmann::json::object()), "visitorData");
    if (!visitor.empty()) visitor_.seed(visitor);
    return parsed;
}

namespace {

std::string urlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string result;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(hex[(ch >> 4) & 0x0F]);
            result.push_back(hex[ch & 0x0F]);
        }
    }
    return result;
}

}  // namespace

nlohmann::json InnerTube::continuationRequest(const std::string& continuation) {
    const std::string encoded = urlEncode(continuation);
    const std::string url = std::string("https://music.youtube.com/youtubei/v1/search?key=") + kApiKey +
                            "&prettyPrint=false&ctoken=" + encoded + "&continuation=" + encoded + "&type=next";
    nlohmann::json body;
    body["context"]["client"]["clientName"] = "WEB_REMIX";
    body["context"]["client"]["clientVersion"] = kClientVersion;

    const auto response = http_.post(url, body.dump(),
                                     {
                                         {"Content-Type", "application/json"},
                                         {"Origin", "https://music.youtube.com"},
                                         {"Referer", "https://music.youtube.com/"},
                                         {"X-Goog-Api-Format-Version", "1"},
                                     },
                                     kUserAgent);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("InnerTube continuation failed with HTTP " + std::to_string(response.status));
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response.body);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("InnerTube continuation returned invalid JSON");
    }
    const auto visitor = stringValue(parsed.value("responseContext", nlohmann::json::object()), "visitorData");
    if (!visitor.empty()) visitor_.seed(visitor);
    return parsed;
}

std::vector<Track> InnerTube::searchSongs(const std::string& query) {
    return extractFilteredSongs(searchRequest(query, kSongsParams));
}

std::vector<Track> InnerTube::searchVideos(const std::string& query) {
    return extractFilteredSongs(searchRequest(query, kVideosParams));
}

std::vector<Track> InnerTube::searchAll(const std::string& query) {
    std::vector<Track> result;
    try {
        result = searchSongs(query);
    } catch (const std::exception& error) {
        try {
            result = searchVideos(query);
        } catch (...) {
            throw error;
        }
    }
    try {
        appendUnique(result, searchVideos(query));
    } catch (...) {
    }
    return result;
}

SongSearchPage InnerTube::searchSongsPage(const std::string& query) {
    const auto songsBody = searchRequest(query, kSongsParams);
    std::vector<Track> videos;
    try {
        videos = extractFilteredSongs(searchRequest(query, kVideosParams));
    } catch (...) {
    }

    SongSearchPage page;
    page.songs = mergeInVideoUploads(dedupeByVideoId(extractFilteredSongs(songsBody)), videos);
    page.continuationToken = extractSearchContinuation(songsBody).value_or("");
    try {
        page.artists = parseArtists(searchRequest(query, ""));
    } catch (...) {
    }
    return page;
}

SongSearchPage InnerTube::loadMoreSongResults(const std::string& continuation) {
    if (continuation.empty()) return {};
    const auto body = continuationRequest(continuation);
    SongSearchPage page;
    page.songs = dedupeByVideoId(extractFilteredSongs(body));
    page.continuationToken = extractSearchContinuation(body).value_or("");
    return page;
}

std::vector<ArtistSummary> InnerTube::searchArtists(const std::string& query) {
    try {
        return parseArtists(searchRequest(query, ""));
    } catch (...) {
        return {};
    }
}

}  // namespace resona
