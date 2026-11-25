#include "player/player_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <fstream>
#include <regex>
#include <thread>

namespace resona {

namespace {

constexpr int64_t kDownloadChunk = 900000;

void schedule(std::function<void()> task) {
    std::thread([task = std::move(task)] {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        task();
    }).detach();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string sanitize(const std::string& value) {
    std::string result;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') result.push_back(ch);
        else result.push_back('_');
    }
    return result;
}

std::string downloadFileName(const Track& track) {
    std::string base;
    if (!track.artist.empty()) base += track.artist + " - ";
    base += track.title.empty() ? track.id : track.title;
    base = sanitize(base);
    while (!base.empty() && base.front() == '_') base.erase(base.begin());
    if (base.empty()) base = sanitize(track.id);
    if (base.size() > 120) base.resize(120);
    return base + ".m4a";
}

bool contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

bool looksLikeReference(const std::string& value) {
    if (value.rfind("local:", 0) == 0 || value.rfind("yt:", 0) == 0) return true;
    if (contains(value, "youtu.be/") || contains(value, "v=")) return true;
    if (value.size() != 11) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
}

}  // namespace

PlayerController::PlayerController(Paths paths)
    : paths_(std::move(paths)),
      store_(paths_),
      visitor_(paths_, http_),
      innerTube_(http_, visitor_),
      extractor_(http_, visitor_),
      scanner_(store_),
      rng_(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())) {
    engine_.setEndCallback([this] { handleEnd(); });
    engine_.setErrorCallback([this](const std::string& message) { handleError(message); });
}

std::vector<Track> PlayerController::scan(const std::vector<std::filesystem::path>& roots) {
    return scanner_.scan(roots);
}

std::vector<Track> PlayerController::localTracks() const {
    return scanner_.cached();
}

std::vector<Track> PlayerController::searchLocal(const std::string& query) const {
    const std::string needle = lower(query);
    std::vector<Track> result;
    for (const auto& track : localTracks()) {
        if (contains(lower(track.title), needle) || contains(lower(track.artist), needle) ||
            contains(lower(track.album), needle)) {
            result.push_back(track);
        }
    }
    return result;
}

std::vector<Track> PlayerController::search(const std::string& query) {
    store_.recordSearch(query);
    std::vector<Track> result = searchLocal(query);
    try {
        for (const auto& track : innerTube_.searchAll(query)) {
            store_.remember(track);
            if (std::none_of(result.begin(), result.end(), [&](const Track& item) { return item.id == track.id; })) {
                result.push_back(track);
            }
        }
    } catch (const std::exception& error) {
        if (result.empty()) throw error;
        lastError_ = error.what();
    }
    return result;
}

void PlayerController::play(const Track& track) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        store_.remember(track);
        queue_ = {track};
        index_ = 0;
        retries_ = 0;
        excludedClients_.clear();
        lastError_.clear();
    }
    startCurrent();
}

std::vector<Track> PlayerController::playQuery(const std::string& query) {
    std::error_code ec;
    if (std::filesystem::exists(query, ec)) {
        const auto tracks = scan({std::filesystem::path(query)});
        if (!tracks.empty()) {
            play(tracks.front());
            return tracks;
        }
    }
    if (looksLikeReference(query)) {
        if (const auto resolved = resolveTrack(query)) {
            play(*resolved);
            return {*resolved};
        }
    }
    const auto results = search(query);
    if (results.empty()) return results;
    play(results.front());
    return results;
}

void PlayerController::playById(const std::string& id) {
    const auto resolved = resolveTrack(id);
    if (!resolved) throw std::runtime_error("No track found for: " + id);
    play(*resolved);
}

void PlayerController::playPlaylist(const std::string& playlistId) {
    std::vector<Playlist> lists = playlists();
    const auto it = std::find_if(lists.begin(), lists.end(), [&](const Playlist& item) { return item.id == playlistId; });
    if (it == lists.end()) throw std::runtime_error("Playlist not found");
    std::vector<Track> tracks;
    for (const auto& id : it->trackIds) {
        if (auto item = track(id)) tracks.push_back(*item);
    }
    if (tracks.empty()) throw std::runtime_error("Playlist is empty");
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        queue_ = tracks;
        index_ = 0;
        retries_ = 0;
        excludedClients_.clear();
    }
    startCurrent();
}

void PlayerController::enqueue(const Track& track) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    store_.remember(track);
    queue_.push_back(track);
}

void PlayerController::enqueueById(const std::string& id) {
    const auto resolved = resolveTrack(id);
    if (!resolved) throw std::runtime_error("No track found for: " + id);
    enqueue(*resolved);
}

void PlayerController::next() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (queue_.empty()) return;
        if (repeat_ == "one") {
        } else if (shuffle_ && queue_.size() > 1) {
            std::uniform_int_distribution<size_t> distribution(0, queue_.size() - 1);
            size_t candidate = index_;
            while (candidate == index_) candidate = distribution(rng_);
            index_ = candidate;
        } else if (index_ + 1 < queue_.size()) {
            ++index_;
        } else if (repeat_ == "all") {
            index_ = 0;
        } else {
            engine_.stop();
            return;
        }
        retries_ = 0;
        excludedClients_.clear();
    }
    startCurrent();
}

void PlayerController::previous() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (queue_.empty()) return;
        if (index_ == 0) {
            if (repeat_ != "all") {
                engine_.seekMs(0);
                return;
            }
            index_ = queue_.size() - 1;
        } else {
            --index_;
        }
        retries_ = 0;
        excludedClients_.clear();
    }
    startCurrent();
}

void PlayerController::pause() {
    engine_.setPaused(true);
}

void PlayerController::resume() {
    engine_.setPaused(false);
}

void PlayerController::togglePause() {
    engine_.togglePause();
}

void PlayerController::stop() {
    engine_.stop();
}

void PlayerController::seek(int64_t ms) {
    engine_.seekMs(ms);
}

void PlayerController::setVolume(float volume) {
    engine_.setVolume(volume);
}

void PlayerController::setShuffle(bool enabled) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    shuffle_ = enabled;
}

void PlayerController::setRepeat(const std::string& mode) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (mode == "off" || mode == "all" || mode == "one") repeat_ = mode;
}

void PlayerController::wait() {
    engine_.wait();
}

void PlayerController::setLiked(const std::string& id, bool liked) {
    store_.setLiked(id, liked);
}

bool PlayerController::isLiked(const std::string& id) const {
    return store_.isLiked(id);
}

std::vector<Track> PlayerController::liked() const {
    return store_.liked();
}

std::vector<Track> PlayerController::playHistory() const {
    std::vector<Track> result;
    for (const auto& entry : store_.history()) {
        if (auto track = store_.track(entry.trackId)) result.push_back(*track);
    }
    return result;
}

std::vector<std::string> PlayerController::searchHistory() const {
    return store_.searchHistory();
}

void PlayerController::recordSearch(const std::string& query) {
    store_.recordSearch(query);
}

void PlayerController::rememberTrack(const Track& track) {
    store_.remember(track);
}

void PlayerController::removeSearchHistory(const std::string& query) {
    store_.removeSearchHistory(query);
}

void PlayerController::clearSearchHistory() {
    store_.clearSearchHistory();
}

std::vector<Playlist> PlayerController::playlists() const {
    return store_.playlists();
}

Playlist PlayerController::createPlaylist(const std::string& name) {
    return store_.createPlaylist(name);
}

void PlayerController::addToPlaylist(const std::string& playlistId, const std::string& trackId) {
    store_.addToPlaylist(playlistId, trackId);
}

void PlayerController::removeFromPlaylist(const std::string& playlistId, const std::string& trackId) {
    store_.removeFromPlaylist(playlistId, trackId);
}

void PlayerController::deletePlaylist(const std::string& playlistId) {
    store_.deletePlaylist(playlistId);
}

std::vector<DownloadedSong> PlayerController::downloads() const {
    return store_.downloads();
}

std::string PlayerController::download(const std::string& id) {
    const auto resolved = resolveTrack(id);
    if (!resolved) throw std::runtime_error("No track found for: " + id);
    if (resolved->source == TrackSource::Local) return resolved->location;

    const auto existing = store_.download(resolved->id);
    if (existing) return existing->path;

    const auto stream = extractor_.resolve(resolved->videoId, {});
    const auto directory = paths_.musicDir();
    std::filesystem::create_directories(directory);
    const auto destination = directory / downloadFileName(*resolved);
    const auto temporary = destination.string() + ".part";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.good()) throw std::runtime_error("Could not create download file");

    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Origin", "https://www.youtube.com"},
        {"Referer", "https://www.youtube.com/"},
        {"Accept", "*/*"},
    };
    int64_t offset = 0;
    while (true) {
        const auto result = http_.range(stream.url, offset, kDownloadChunk, headers, stream.userAgent);
        if (result.status < 200 || result.status >= 300 || result.body.empty()) {
            output.close();
            std::filesystem::remove(temporary);
            throw std::runtime_error("Download failed with HTTP " + std::to_string(result.status));
        }
        output.write(result.body.data(), static_cast<std::streamsize>(result.body.size()));
        if (result.body.size() < kDownloadChunk) break;
        offset += static_cast<int64_t>(result.body.size());
    }
    output.close();
    std::filesystem::rename(temporary, destination);

    DownloadedSong song{resolved->id, destination.string(), resolved->title, resolved->artist, resolved->durationMs};
    store_.addDownload(song);
    store_.remember(*resolved);
    return song.path;
}

void PlayerController::removeDownload(const std::string& id) {
    const auto song = store_.download(id);
    if (song) {
        std::error_code ec;
        std::filesystem::remove(song->path, ec);
    }
    store_.removeDownload(id);
}

std::optional<Track> PlayerController::track(const std::string& id) const {
    return store_.track(id);
}

PlayerSnapshot PlayerController::status() const {
    PlayerSnapshot snapshot = engine_.snapshot();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot.queue = queue_;
        snapshot.queueIndex = index_;
        snapshot.shuffle = shuffle_;
        snapshot.repeat = repeat_;
        if (snapshot.error.empty()) snapshot.error = lastError_;
    }
    return snapshot;
}

void PlayerController::startCurrent() {
    Track track;
    std::string clientToExclude;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (queue_.empty()) return;
        if (index_ >= queue_.size()) index_ = 0;
        track = queue_[index_];
        current_ = track;
        clientToExclude = currentStream_.clientName;
    }

    store_.remember(track);
    if (track.source != TrackSource::YouTube) {
        currentStream_ = {};
        engine_.play(track);
        store_.recordPlay(track);
        return;
    }

    if (const auto downloaded = store_.download(track.id)) {
        Track local = track;
        local.source = TrackSource::Local;
        local.location = downloaded->path;
        currentStream_ = {};
        engine_.play(local);
        store_.recordPlay(track);
        return;
    }

    try {
        const auto stream = extractor_.resolve(track.videoId, excludedClients_);
        currentStream_ = stream;
        engine_.play(track, stream);
        store_.recordPlay(track);
    } catch (const std::exception& error) {
        lastError_ = error.what();
        engine_.reportError(error.what());
    }
    (void)clientToExclude;
}

void PlayerController::handleEnd() {
    schedule([this] {
        PlayerSnapshot snapshot = status();
        if (snapshot.repeat == "one") {
            startCurrent();
            return;
        }
        next();
    });
}

void PlayerController::handleError(const std::string& message) {
    Track current;
    StreamSource stream;
    int attempts = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        current = current_;
        stream = currentStream_;
        attempts = retries_;
        lastError_ = message;
    }
    if (current.source != TrackSource::YouTube || stream.clientName.empty() || attempts >= 4) return;
    if (retryScheduled_.exchange(true)) return;
    schedule([this, stream] {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            excludedClients_.insert(stream.clientName);
            ++retries_;
        }
        extractor_.refreshVisitor();
        retryScheduled_.store(false);
        startCurrent();
    });
}

std::optional<Track> PlayerController::resolveTrack(const std::string& reference) {
    if (auto known = store_.track(reference)) return known;
    if (auto saved = store_.download(reference)) {
        Track track;
        track.source = TrackSource::Local;
        track.id = saved->id;
        track.title = saved->title;
        track.artist = saved->artist;
        track.durationMs = saved->durationMs;
        track.durationText = prettyDuration(saved->durationMs);
        track.location = saved->path;
        return track;
    }

    std::error_code ec;
    if (std::filesystem::exists(reference, ec)) {
        const auto tracks = scanner_.scan({std::filesystem::path(reference)});
        if (!tracks.empty()) return tracks.front();
    }

    std::string videoId;
    if (reference.rfind("yt:", 0) == 0) videoId = reference.substr(3);
    else if (contains(reference, "youtu.be/")) videoId = reference.substr(reference.find("youtu.be/") + 9);
    else if (contains(reference, "v=")) videoId = reference.substr(reference.find("v=") + 2);
    else if (reference.size() == 11) videoId = reference;

    if (!videoId.empty()) {
        const auto question = videoId.find('?');
        if (question != std::string::npos) videoId.erase(question);
        Track track;
        track.source = TrackSource::YouTube;
        track.id = trackIdForVideo(videoId);
        track.videoId = videoId;
        track.title = videoId;
        track.artist = "YouTube";
        return track;
    }

    const auto results = search(reference);
    if (!results.empty()) return results.front();
    return std::nullopt;
}

}  // namespace resona
