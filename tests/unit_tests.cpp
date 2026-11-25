#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "domain/models.hpp"
#include "local/scanner.hpp"
#include "ui/ipod_model.hpp"
#include "storage/library_store.hpp"
#include "youtube/innertube.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "PASS " << name << '\n';
    } else {
        std::cout << "FAIL " << name << '\n';
        ++failures;
    }
}

nlohmann::json searchFixture() {
    return nlohmann::json::parse(R"({
      "contents": {
        "itemSectionRenderer": {
          "contents": [{
            "musicShelfRenderer": {
              "contents": [{
                "musicResponsiveListItemRenderer": {
                  "flexColumns": [
                    {"musicResponsiveListItemFlexColumnRenderer": {"text": {"runs": [{
                      "text": "Test Song",
                      "navigationEndpoint": {"watchEndpoint": {"videoId": "abcdefghijk"}}
                    }]}}},
                    {"musicResponsiveListItemFlexColumnRenderer": {"text": {"runs": [
                      {"text": "Test Artist"}, {"text": " • "}, {"text": "3:21"}
                    ]}}}
                  ],
                  "thumbnail": {"musicThumbnailRenderer": {"thumbnail": {"thumbnails": [
                    {"url": "small.jpg"}, {"url": "large.jpg"}
                  ]}}}
                }
              }]
            }
          }]
        }
      }
    })");
}

}  // namespace

int main() {
    using namespace resona;

    check(parseDurationMs("3:21") == 201000, "parseDurationMs m:ss");
    check(parseDurationMs("1:02:03") == 3723000, "parseDurationMs h:mm:ss");
    check(parseDurationMs("bad") == 0, "parseDurationMs invalid");
    check(prettyDuration(201000) == "3:21", "prettyDuration");

    check(ui::stepSelection(0, -1, 5) == 4, "stepSelection wraps backwards");
    check(ui::stepSelection(4, 1, 5) == 0, "stepSelection wraps forwards");
    check(ui::stepSelection(2, 5, 5) == 2, "stepSelection handles full cycles");
    check(ui::stepSelection(0, 1, 0) == 0, "stepSelection handles empty list");
    check(ui::progressPercent(5000, 10000) == 50, "progressPercent midpoint");
    check(ui::progressPercent(-1, 10000) == 0, "progressPercent lower clamp");
    check(ui::progressPercent(20000, 10000) == 100, "progressPercent upper clamp");
    check(ui::progressPercent(1, 0) == 0, "progressPercent zero duration");
    check(ui::volumePercent(0.0f) == 0, "volumePercent mute");
    check(ui::volumePercent(1.0f) == 50, "volumePercent default");
    check(ui::volumePercent(2.0f) == 100, "volumePercent max");
    check(ui::centeredLabel("abc", 7) == "  abc", "centeredLabel");

    // Wheel zone classification: inner navigation band
    const double cR = 30.0, iR = 50.0, oR = 100.0;
    check(ui::classifyWheelZone(0, -40, cR, iR, oR) == ui::WheelZone::InnerUp, "inner zone up (top)");
    check(ui::classifyWheelZone(0, 40, cR, iR, oR) == ui::WheelZone::InnerDown, "inner zone down (bottom)");
    check(ui::classifyWheelZone(-40, 0, cR, iR, oR) == ui::WheelZone::InnerEnter, "inner zone enter (left)");
    check(ui::classifyWheelZone(40, 0, cR, iR, oR) == ui::WheelZone::InnerPrev, "inner zone prev (right)");
    check(ui::classifyWheelZone(0, 0, cR, iR, oR) == ui::WheelZone::Center, "center button");
    check(ui::classifyWheelZone(0, -120, cR, iR, oR) == ui::WheelZone::None, "outside wheel");

    // Wheel zone classification: outer ring quadrants (unchanged)
    check(ui::classifyWheelZone(0, -70, cR, iR, oR) == ui::WheelZone::Menu, "outer zone menu (top)");
    check(ui::classifyWheelZone(70, 0, cR, iR, oR) == ui::WheelZone::Next, "outer zone next (right)");
    check(ui::classifyWheelZone(0, 70, cR, iR, oR) == ui::WheelZone::PlayPause, "outer zone play/pause (bottom)");
    check(ui::classifyWheelZone(-70, 0, cR, iR, oR) == ui::WheelZone::Previous, "outer zone previous (left)");

    // Enter-to-play behavior
    check(ui::enterPlaysTrack(true, false), "enter plays when results exist");
    check(ui::enterPlaysTrack(true, true), "enter plays when results exist (with query)");
    check(ui::enterPlaysTrack(false, true), "enter triggers search when query non-empty");
    check(!ui::enterPlaysTrack(false, false), "enter does nothing when empty");

    const auto tracks = extractFilteredSongs(searchFixture());
    check(tracks.size() == 1, "extractFilteredSongs count");
    if (!tracks.empty()) {
        check(tracks[0].videoId == "abcdefghijk", "extractFilteredSongs video id");
        check(tracks[0].title == "Test Song", "extractFilteredSongs title");
        check(tracks[0].artist == "Test Artist", "extractFilteredSongs artist");
        check(tracks[0].durationMs == 201000, "extractFilteredSongs duration");
        check(tracks[0].location == "large.jpg", "extractFilteredSongs thumbnail");
    }

    const auto root = std::filesystem::temp_directory_path() / ("resona-player-tests-" + std::to_string(getpid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);

    Paths paths;
    LibraryStore store(paths);
    Track track;
    track.source = TrackSource::YouTube;
    track.id = trackIdForVideo("abcdefghijk");
    track.videoId = "abcdefghijk";
    track.title = "Persisted Song";
    track.artist = "Persisted Artist";
    track.durationMs = 123000;

    store.remember(track);
    check(store.track(track.id).has_value(), "catalog persistence");
    store.setLiked(track.id, true);
    check(store.isLiked(track.id), "liked persistence");
    store.recordPlay(track);
    check(store.history().size() == 1, "history persistence");
    const auto playlist = store.createPlaylist("Road Trip");
    store.addToPlaylist(playlist.id, track.id);
    check(store.playlists().size() == 1 && store.playlists()[0].trackIds.size() == 1, "playlist persistence");
    store.recordSearch("ambient");
    check(store.searchHistory().size() == 1, "search history persistence");

    const auto music = root / "music";
    std::filesystem::create_directories(music / "nested");
    std::ofstream(music / "nested" / "sample.mp3") << "not-an-mp3";
    LocalScanner scanner(store);
    const auto local = scanner.scan({music});
    check(local.size() == 1, "local scanner count");
    if (!local.empty()) {
        check(local[0].title == "sample", "local scanner title fallback");
        check(local[0].source == TrackSource::Local, "local scanner source");
    }

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? 0 : 1;
}
