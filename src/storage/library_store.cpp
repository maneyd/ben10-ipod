#include "storage/library_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace resona {

namespace {

std::string getString(const nlohmann::json& value, const char* key, const std::string& fallback = "") {
    if (!value.is_object()) return fallback;
    const auto it = value.find(key);
    if (it == value.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

int64_t getInt(const nlohmann::json& value, const char* key, int64_t fallback = 0) {
    if (!value.is_object()) return fallback;
    const auto it = value.find(key);
    if (it == value.end()) return fallback;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

std::string nowIso() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::vector<std::string> stringArray(const nlohmann::json& value) {
    std::vector<std::string> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

}  // namespace

nlohmann::json trackToJson(const Track& track) {
    return {
        {"source", track.source == TrackSource::YouTube ? "youtube" : "local"},
        {"id", track.id},
        {"title", track.title},
        {"artist", track.artist},
        {"album", track.album},
        {"durationText", track.durationText},
        {"durationMs", track.durationMs},
        {"location", track.location},
        {"videoId", track.videoId},
        {"fileSize", track.fileSize},
        {"modifiedAt", track.modifiedAt},
    };
}

Track trackFromJson(const nlohmann::json& value) {
    Track track;
    track.source = getString(value, "source") == "youtube" ? TrackSource::YouTube : TrackSource::Local;
    track.id = getString(value, "id");
    track.title = getString(value, "title");
    track.artist = getString(value, "artist");
    track.album = getString(value, "album");
    track.durationText = getString(value, "durationText");
    track.durationMs = getInt(value, "durationMs");
    track.location = getString(value, "location");
    track.videoId = getString(value, "videoId");
    track.fileSize = getInt(value, "fileSize");
    track.modifiedAt = getInt(value, "modifiedAt");
    return track;
}

LibraryStore::LibraryStore(const Paths& paths)
    : catalog_(paths.dataDir() / "catalog.json"),
      likes_(paths.dataDir() / "liked_songs.json"),
      history_(paths.dataDir() / "play_history.json"),
      searchHistory_(paths.dataDir() / "search_history.json"),
      playlists_(paths.dataDir() / "user_playlists.json"),
      downloads_(paths.dataDir() / "downloaded_songs.json"),
      localIndex_(paths.dataDir() / "local_index.json") {}

void LibraryStore::remember(const Track& track) {
    auto all = catalog_.load(nlohmann::json::object());
    if (!all.is_object()) all = nlohmann::json::object();
    all[track.id] = trackToJson(track);
    catalog_.save(all);
}

std::optional<Track> LibraryStore::track(const std::string& id) const {
    const auto all = catalog_.load(nlohmann::json::object());
    if (!all.is_object()) return std::nullopt;
    const auto it = all.find(id);
    if (it == all.end()) return std::nullopt;
    return trackFromJson(*it);
}

std::vector<Track> LibraryStore::catalog() const {
    std::vector<Track> result;
    const auto all = catalog_.load(nlohmann::json::object());
    if (!all.is_object()) return result;
    for (const auto& [id, value] : all.items()) {
        (void)id;
        result.push_back(trackFromJson(value));
    }
    return result;
}

bool LibraryStore::isLiked(const std::string& id) const {
    const auto values = stringArray(likes_.load());
    return std::find(values.begin(), values.end(), id) != values.end();
}

void LibraryStore::setLiked(const std::string& id, bool liked) {
    auto values = stringArray(likes_.load());
    values.erase(std::remove(values.begin(), values.end(), id), values.end());
    if (liked) values.insert(values.begin(), id);
    likes_.save(values);
}

std::vector<Track> LibraryStore::liked() const {
    std::vector<Track> result;
    for (const auto& id : stringArray(likes_.load())) {
        if (auto item = track(id)) result.push_back(*item);
    }
    return result;
}

void LibraryStore::recordPlay(const Track& track) {
    auto entries = history_.load();
    if (!entries.is_array()) entries = nlohmann::json::array();
    bool found = false;
    for (auto& entry : entries) {
        if (getString(entry, "trackId") == track.id) {
            entry["playedAt"] = nowIso();
            entry["count"] = getInt(entry, "count") + 1;
            found = true;
            break;
        }
    }
    if (!found) {
        entries.push_back({{"trackId", track.id}, {"playedAt", nowIso()}, {"count", 1}});
    }
    while (entries.size() > 100) entries.erase(entries.begin());
    history_.save(entries);
}

std::vector<PlayHistoryEntry> LibraryStore::history() const {
    std::vector<PlayHistoryEntry> result;
    const auto entries = history_.load();
    if (!entries.is_array()) return result;
    for (const auto& entry : entries) {
        result.push_back({getString(entry, "trackId"), getString(entry, "playedAt"), static_cast<int>(getInt(entry, "count"))});
    }
    return result;
}

void LibraryStore::recordSearch(const std::string& query) {
    if (query.empty()) return;
    auto values = stringArray(searchHistory_.load());
    std::string lowered = query;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    values.erase(std::remove_if(values.begin(), values.end(), [&](const std::string& item) {
        std::string itemLower = item;
        std::transform(itemLower.begin(), itemLower.end(), itemLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return itemLower == lowered;
    }), values.end());
    values.insert(values.begin(), query);
    while (values.size() > 20) values.pop_back();
    searchHistory_.save(values);
}

std::vector<std::string> LibraryStore::searchHistory() const {
    return stringArray(searchHistory_.load());
}

void LibraryStore::removeSearchHistory(const std::string& query) {
    auto values = stringArray(searchHistory_.load());
    values.erase(std::remove(values.begin(), values.end(), query), values.end());
    searchHistory_.save(values);
}

void LibraryStore::clearSearchHistory() {
    searchHistory_.save(nlohmann::json::array());
}

std::vector<Playlist> LibraryStore::playlists() const {
    std::vector<Playlist> result;
    const auto values = playlists_.load();
    if (!values.is_array()) return result;
    for (const auto& value : values) {
        result.push_back({getString(value, "id"), getString(value, "name"), stringArray(value.value("trackIds", nlohmann::json::array()))});
    }
    return result;
}

Playlist LibraryStore::createPlaylist(const std::string& name) {
    auto values = playlists_.load();
    if (!values.is_array()) values = nlohmann::json::array();
    Playlist playlist;
    playlist.id = "pl:" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    playlist.name = name;
    values.push_back({{"id", playlist.id}, {"name", playlist.name}, {"trackIds", nlohmann::json::array()}});
    playlists_.save(values);
    return playlist;
}

void LibraryStore::addToPlaylist(const std::string& playlistId, const std::string& trackId) {
    auto values = playlists_.load();
    if (!values.is_array()) return;
    for (auto& value : values) {
        if (getString(value, "id") != playlistId) continue;
        auto tracks = stringArray(value.value("trackIds", nlohmann::json::array()));
        if (std::find(tracks.begin(), tracks.end(), trackId) == tracks.end()) tracks.push_back(trackId);
        value["trackIds"] = tracks;
        break;
    }
    playlists_.save(values);
}

void LibraryStore::removeFromPlaylist(const std::string& playlistId, const std::string& trackId) {
    auto values = playlists_.load();
    if (!values.is_array()) return;
    for (auto& value : values) {
        if (getString(value, "id") != playlistId) continue;
        auto tracks = stringArray(value.value("trackIds", nlohmann::json::array()));
        tracks.erase(std::remove(tracks.begin(), tracks.end(), trackId), tracks.end());
        value["trackIds"] = tracks;
        break;
    }
    playlists_.save(values);
}

void LibraryStore::deletePlaylist(const std::string& playlistId) {
    auto values = playlists_.load();
    if (!values.is_array()) return;
    values.erase(std::remove_if(values.begin(), values.end(), [&](const nlohmann::json& value) {
        return getString(value, "id") == playlistId;
    }), values.end());
    playlists_.save(values);
}

std::vector<DownloadedSong> LibraryStore::downloads() const {
    std::vector<DownloadedSong> result;
    const auto values = downloads_.load();
    if (!values.is_array()) return result;
    for (const auto& value : values) {
        result.push_back({getString(value, "id"), getString(value, "path"), getString(value, "title"),
                          getString(value, "artist"), getInt(value, "durationMs")});
    }
    return result;
}

void LibraryStore::addDownload(const DownloadedSong& song) {
    auto values = downloads_.load();
    if (!values.is_array()) values = nlohmann::json::array();
    values.erase(std::remove_if(values.begin(), values.end(), [&](const nlohmann::json& value) {
        return getString(value, "id") == song.id;
    }), values.end());
    values.push_back({{"id", song.id}, {"path", song.path}, {"title", song.title},
                      {"artist", song.artist}, {"durationMs", song.durationMs}});
    downloads_.save(values);
}

void LibraryStore::removeDownload(const std::string& id) {
    auto values = downloads_.load();
    if (!values.is_array()) return;
    values.erase(std::remove_if(values.begin(), values.end(), [&](const nlohmann::json& value) {
        return getString(value, "id") == id;
    }), values.end());
    downloads_.save(values);
}

std::optional<DownloadedSong> LibraryStore::download(const std::string& id) const {
    for (const auto& song : downloads()) {
        if (song.id == id) return song;
    }
    return std::nullopt;
}

std::vector<Track> LibraryStore::localTracks() const {
    std::vector<Track> result;
    const auto values = localIndex_.load();
    if (!values.is_array()) return result;
    for (const auto& value : values) result.push_back(trackFromJson(value));
    return result;
}

void LibraryStore::saveLocalTracks(const std::vector<Track>& tracks) {
    auto values = nlohmann::json::array();
    for (const auto& track : tracks) values.push_back(trackToJson(track));
    localIndex_.save(values);
}

}  // namespace resona
