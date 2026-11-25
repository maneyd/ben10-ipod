#pragma once

#include <optional>
#include <string>
#include <vector>

#include "domain/models.hpp"
#include "storage/json_store.hpp"
#include "storage/paths.hpp"

namespace resona {

nlohmann::json trackToJson(const Track& track);
Track trackFromJson(const nlohmann::json& value);

class LibraryStore {
public:
    explicit LibraryStore(const Paths& paths);

    void remember(const Track& track);
    std::optional<Track> track(const std::string& id) const;
    std::vector<Track> catalog() const;

    bool isLiked(const std::string& id) const;
    void setLiked(const std::string& id, bool liked);
    std::vector<Track> liked() const;

    void recordPlay(const Track& track);
    std::vector<PlayHistoryEntry> history() const;

    void recordSearch(const std::string& query);
    std::vector<std::string> searchHistory() const;
    void removeSearchHistory(const std::string& query);
    void clearSearchHistory();

    std::vector<Playlist> playlists() const;
    Playlist createPlaylist(const std::string& name);
    void addToPlaylist(const std::string& playlistId, const std::string& trackId);
    void removeFromPlaylist(const std::string& playlistId, const std::string& trackId);
    void deletePlaylist(const std::string& playlistId);

    std::vector<DownloadedSong> downloads() const;
    void addDownload(const DownloadedSong& song);
    void removeDownload(const std::string& id);
    std::optional<DownloadedSong> download(const std::string& id) const;

    std::vector<Track> localTracks() const;
    void saveLocalTracks(const std::vector<Track>& tracks);

private:
    JsonFile catalog_;
    JsonFile likes_;
    JsonFile history_;
    JsonFile searchHistory_;
    JsonFile playlists_;
    JsonFile downloads_;
    JsonFile localIndex_;
};

}  // namespace resona
