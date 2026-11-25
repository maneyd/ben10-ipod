#include "local/scanner.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>

namespace resona {

namespace {

const std::set<std::string> kExtensions = {".mp3", ".m4a", ".mp4", ".aac", ".flac", ".ogg", ".opus", ".wav", ".wma", ".aiff"};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

int64_t modifiedAt(const std::filesystem::path& path) {
    const auto time = std::filesystem::last_write_time(path).time_since_epoch().count();
    return static_cast<int64_t>(time);
}

Track readTags(const std::filesystem::path& path) {
    TagLib::FileRef file(path.c_str());
    Track track;
    track.source = TrackSource::Local;
    track.location = path.string();
    track.id = trackIdForPath(track.location);
    track.title = path.stem().string();
    track.artist = "Unknown Artist";
    const auto status = std::filesystem::status(path);
    track.fileSize = static_cast<int64_t>(std::filesystem::file_size(path));
    track.modifiedAt = modifiedAt(path);
    (void)status;

    if (!file.isNull() && file.tag()) {
        const auto title = file.tag()->title().toCString(true);
        const auto artist = file.tag()->artist().toCString(true);
        const auto album = file.tag()->album().toCString(true);
        if (title != nullptr && *title != '\0') track.title = title;
        if (artist != nullptr && *artist != '\0') track.artist = artist;
        if (album != nullptr && *album != '\0') track.album = album;
        if (file.audioProperties()) track.durationMs = static_cast<int64_t>(file.audioProperties()->lengthInMilliseconds());
    }
    track.durationText = prettyDuration(track.durationMs);
    return track;
}

}  // namespace

LocalScanner::LocalScanner(LibraryStore& store) : store_(store) {}

std::vector<Track> LocalScanner::scan(const std::vector<std::filesystem::path>& roots) {
    auto cached = store_.localTracks();
    std::vector<Track> result;
    std::vector<Track> updated;

    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        if (std::filesystem::is_regular_file(root, ec)) {
            if (kExtensions.count(lower(root.extension().string())) == 0) continue;
            updated.push_back(readTags(root));
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (kExtensions.count(lower(entry.path().extension().string())) == 0) continue;
            updated.push_back(readTags(entry.path()));
        }
    }

    std::sort(updated.begin(), updated.end(), [](const Track& a, const Track& b) {
        return a.location < b.location;
    });
    updated.erase(std::unique(updated.begin(), updated.end(), [](const Track& a, const Track& b) {
        return a.location == b.location;
    }), updated.end());

    std::set<std::string> seen;
    for (const auto& track : updated) {
        seen.insert(track.location);
        const auto it = std::find_if(cached.begin(), cached.end(), [&](const Track& old) {
            return old.location == track.location && old.fileSize == track.fileSize && old.modifiedAt == track.modifiedAt;
        });
        result.push_back(it != cached.end() ? *it : track);
    }
    for (const auto& old : cached) {
        if (seen.count(old.location) == 0 && std::filesystem::exists(old.location)) result.push_back(old);
    }

    std::sort(result.begin(), result.end(), [](const Track& a, const Track& b) {
        return a.title < b.title;
    });
    store_.saveLocalTracks(result);
    for (const auto& track : result) store_.remember(track);
    return result;
}

std::vector<Track> LocalScanner::cached() const {
    return store_.localTracks();
}

}  // namespace resona
