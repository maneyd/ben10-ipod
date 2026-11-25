#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace resona {

enum class TrackSource { Local, YouTube };

struct Track {
    TrackSource source = TrackSource::Local;
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string durationText;
    int64_t durationMs = 0;
    std::string location;
    std::string videoId;
    int64_t fileSize = 0;
    int64_t modifiedAt = 0;
};

struct ArtistSummary {
    std::string id;
    std::string name;
    std::string thumbnail;
};

struct SongSearchPage {
    std::vector<Track> songs;
    std::vector<ArtistSummary> artists;
    std::string continuationToken;
};

struct BrowseSection {
    std::string title;
    std::vector<Track> songs;
};

struct StreamSource {
    std::string url;
    std::string userAgent;
    std::string clientName;
};

struct Playlist {
    std::string id;
    std::string name;
    std::vector<std::string> trackIds;
};

struct DownloadedSong {
    std::string id;
    std::string path;
    std::string title;
    std::string artist;
    int64_t durationMs = 0;
};

struct PlayHistoryEntry {
    std::string trackId;
    std::string playedAt;
    int count = 0;
};

struct PlayerSnapshot {
    Track current;
    bool hasCurrent = false;
    bool playing = false;
    bool buffering = false;
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    size_t queueIndex = 0;
    std::vector<Track> queue;
    bool shuffle = false;
    std::string repeat = "off";
    double volume = 1.0;
    std::string error;
};

std::string trackIdForVideo(const std::string& videoId);
std::string trackIdForPath(const std::string& path);
std::string prettyDuration(int64_t ms);
int64_t parseDurationMs(const std::string& text);
std::string urlDecode(const std::string& value);

}  // namespace resona
