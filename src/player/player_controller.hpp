#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "audio/audio_engine.hpp"
#include "domain/models.hpp"
#include "local/scanner.hpp"
#include "network/http.hpp"
#include "storage/library_store.hpp"
#include "storage/paths.hpp"
#include "youtube/extractor.hpp"
#include "youtube/innertube.hpp"
#include "youtube/visitor.hpp"

namespace resona {

class PlayerController {
public:
    explicit PlayerController(Paths paths);

    std::vector<Track> scan(const std::vector<std::filesystem::path>& roots);
    std::vector<Track> localTracks() const;
    std::vector<Track> searchLocal(const std::string& query) const;
    std::vector<Track> search(const std::string& query);

    void play(const Track& track);
    std::vector<Track> playQuery(const std::string& query);
    void playById(const std::string& id);
    void playPlaylist(const std::string& playlistId);
    void enqueue(const Track& track);
    void enqueueById(const std::string& id);

    void next();
    void previous();
    void pause();
    void resume();
    void togglePause();
    void stop();
    void seek(int64_t ms);
    void setVolume(float volume);
    void setShuffle(bool enabled);
    void setRepeat(const std::string& mode);
    void wait();

    void setLiked(const std::string& id, bool liked);
    bool isLiked(const std::string& id) const;
    std::vector<Track> liked() const;
    std::vector<Track> playHistory() const;
    std::vector<std::string> searchHistory() const;
    void recordSearch(const std::string& query);
    void rememberTrack(const Track& track);
    void removeSearchHistory(const std::string& query);
    void clearSearchHistory();

    std::vector<Playlist> playlists() const;
    Playlist createPlaylist(const std::string& name);
    void addToPlaylist(const std::string& playlistId, const std::string& trackId);
    void removeFromPlaylist(const std::string& playlistId, const std::string& trackId);
    void deletePlaylist(const std::string& playlistId);

    std::vector<DownloadedSong> downloads() const;
    std::string download(const std::string& id);
    void removeDownload(const std::string& id);

    std::optional<Track> track(const std::string& id) const;
    PlayerSnapshot status() const;

private:
    Paths paths_;
    Http http_;
    LibraryStore store_;
    Visitor visitor_;
    InnerTube innerTube_;
    StreamExtractor extractor_;
    LocalScanner scanner_;
    AudioEngine engine_;

    mutable std::mutex stateMutex_;
    std::vector<Track> queue_;
    size_t index_ = 0;
    bool shuffle_ = false;
    std::string repeat_ = "off";
    std::string lastError_;
    Track current_;
    StreamSource currentStream_;
    std::set<std::string> excludedClients_;
    int retries_ = 0;
    std::atomic<bool> retryScheduled_{false};
    std::mt19937 rng_;

    void startCurrent();
    void handleEnd();
    void handleError(const std::string& message);
    std::optional<Track> resolveTrack(const std::string& reference);
};

}  // namespace resona
