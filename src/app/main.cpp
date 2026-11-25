#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include "domain/models.hpp"
#include "player/player_controller.hpp"

namespace {

using resona::PlayerController;
using resona::PlayerSnapshot;
using resona::Track;

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"' && (i == 0 || line[i - 1] != '\\')) {
            quoted = !quoted;
        } else if (std::isspace(static_cast<unsigned char>(ch)) && !quoted) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

std::string join(const std::vector<std::string>& parts, size_t start) {
    std::string result;
    for (size_t i = start; i < parts.size(); ++i) {
        if (i > start) result.push_back(' ');
        result += parts[i];
    }
    return result;
}

void printTrack(const Track& track, size_t number = 0, bool current = false) {
    if (number > 0) std::cout << (current ? "* " : "  ") << number << "  ";
    std::cout << track.id << "  |  " << track.title << "  |  " << track.artist;
    if (!track.durationText.empty()) std::cout << "  |  " << track.durationText;
    std::cout << '\n';
}

void printTracks(const std::vector<Track>& tracks, size_t limit = 20) {
    if (tracks.empty()) {
        std::cout << "no results\n";
        return;
    }
    const size_t count = std::min(limit, tracks.size());
    for (size_t i = 0; i < count; ++i) printTrack(tracks[i], i + 1);
    if (tracks.size() > count) std::cout << "... " << (tracks.size() - count) << " more\n";
}

void printStatus(const PlayerSnapshot& snapshot) {
    if (snapshot.hasCurrent) {
        std::cout << "now: " << snapshot.current.title << " - " << snapshot.current.artist << '\n';
        std::cout << "time: " << resona::prettyDuration(snapshot.positionMs) << " / "
                  << resona::prettyDuration(snapshot.durationMs) << '\n';
    } else {
        std::cout << "now: nothing\n";
    }
    std::cout << "state: " << (snapshot.playing ? "playing" : snapshot.buffering ? "loading" : "stopped") << '\n';
    std::cout << "queue: " << (snapshot.queueIndex + 1) << "/" << snapshot.queue.size()
              << "  repeat=" << snapshot.repeat << "  shuffle=" << (snapshot.shuffle ? "on" : "off") << '\n';
    if (!snapshot.error.empty()) std::cout << "error: " << snapshot.error << '\n';
}

int64_t parseSeek(const std::string& value) {
    const auto colon = value.find(':');
    if (colon == std::string::npos) {
        try {
            return std::stoll(value) * 1000;
        } catch (...) {
            return 0;
        }
    }
    int64_t seconds = 0;
    std::istringstream stream(value);
    std::string part;
    while (std::getline(stream, part, ':')) {
        seconds = seconds * 60 + std::stoll(part);
    }
    return seconds * 1000;
}

void printHelp() {
    std::cout << "commands: scan, local, search, play, enqueue, queue, next, prev, pause, resume, "
                 "toggle, stop, seek, volume, shuffle, repeat, status, like, unlike, liked, history, "
                 "searches, clear-searches, playlist, download, downloads, quit\n";
}

bool runCommand(PlayerController& controller, const std::vector<std::string>& args, bool oneShot) {
    if (args.empty()) return true;
    const std::string command = args[0];

    if (command == "quit" || command == "exit") return false;
    if (command == "help") {
        printHelp();
    } else if (command == "scan") {
        std::vector<std::filesystem::path> roots;
        if (args.size() > 1) {
            for (size_t i = 1; i < args.size(); ++i) roots.emplace_back(args[i]);
        } else {
            roots.push_back(resona::Paths().musicDir());
        }
        const auto tracks = controller.scan(roots);
        std::cout << "indexed " << tracks.size() << " tracks\n";
    } else if (command == "local") {
        printTracks(args.size() > 1 ? controller.searchLocal(join(args, 1)) : controller.localTracks());
    } else if (command == "search") {
        if (args.size() < 2) throw std::runtime_error("usage: search <query>");
        printTracks(controller.search(join(args, 1)));
    } else if (command == "play") {
        if (args.size() < 2) throw std::runtime_error("usage: play <query|path|id|url>");
        const auto results = controller.playQuery(join(args, 1));
        if (results.empty()) std::cout << "no results\n";
        if (oneShot) controller.wait();
    } else if (command == "enqueue") {
        if (args.size() < 2) throw std::runtime_error("usage: enqueue <id|query>");
        controller.enqueueById(join(args, 1));
        std::cout << "added to queue\n";
    } else if (command == "queue") {
        const auto snapshot = controller.status();
        for (size_t i = 0; i < snapshot.queue.size(); ++i) {
            printTrack(snapshot.queue[i], i + 1, i == snapshot.queueIndex);
        }
        if (snapshot.queue.empty()) std::cout << "queue empty\n";
    } else if (command == "next") {
        controller.next();
    } else if (command == "prev" || command == "previous") {
        controller.previous();
    } else if (command == "pause") {
        controller.pause();
    } else if (command == "resume") {
        controller.resume();
    } else if (command == "toggle") {
        controller.togglePause();
    } else if (command == "stop") {
        controller.stop();
    } else if (command == "seek") {
        if (args.size() < 2) throw std::runtime_error("usage: seek <seconds|mm:ss>");
        controller.seek(parseSeek(args[1]));
    } else if (command == "volume") {
        if (args.size() < 2) throw std::runtime_error("usage: volume <0.0-2.0>");
        controller.setVolume(static_cast<float>(std::stof(args[1])));
    } else if (command == "shuffle") {
        if (args.size() < 2) throw std::runtime_error("usage: shuffle on|off");
        controller.setShuffle(args[1] == "on");
    } else if (command == "repeat") {
        if (args.size() < 2) throw std::runtime_error("usage: repeat off|all|one");
        controller.setRepeat(args[1]);
    } else if (command == "status") {
        printStatus(controller.status());
    } else if (command == "like") {
        if (args.size() < 2) throw std::runtime_error("usage: like <track-id>");
        controller.setLiked(args[1], true);
    } else if (command == "unlike") {
        if (args.size() < 2) throw std::runtime_error("usage: unlike <track-id>");
        controller.setLiked(args[1], false);
    } else if (command == "liked") {
        printTracks(controller.liked());
    } else if (command == "history") {
        printTracks(controller.playHistory());
    } else if (command == "searches") {
        for (const auto& item : controller.searchHistory()) std::cout << item << '\n';
    } else if (command == "clear-searches") {
        controller.clearSearchHistory();
    } else if (command == "playlist") {
        if (args.size() < 2) throw std::runtime_error("usage: playlist list|create|play|add|remove|delete");
        const std::string action = args[1];
        if (action == "list") {
            for (const auto& playlist : controller.playlists()) {
                std::cout << playlist.id << "  |  " << playlist.name << "  |  " << playlist.trackIds.size() << " tracks\n";
            }
        } else if (action == "create") {
            if (args.size() < 3) throw std::runtime_error("usage: playlist create <name>");
            const auto playlist = controller.createPlaylist(join(args, 2));
            std::cout << playlist.id << '\n';
        } else if (action == "play") {
            if (args.size() < 3) throw std::runtime_error("usage: playlist play <id>");
            controller.playPlaylist(args[2]);
            if (oneShot) controller.wait();
        } else if (action == "add") {
            if (args.size() < 4) throw std::runtime_error("usage: playlist add <playlist-id> <track-id>");
            controller.addToPlaylist(args[2], args[3]);
        } else if (action == "remove") {
            if (args.size() < 4) throw std::runtime_error("usage: playlist remove <playlist-id> <track-id>");
            controller.removeFromPlaylist(args[2], args[3]);
        } else if (action == "delete") {
            if (args.size() < 3) throw std::runtime_error("usage: playlist delete <id>");
            controller.deletePlaylist(args[2]);
        } else {
            throw std::runtime_error("unknown playlist action: " + action);
        }
    } else if (command == "download") {
        if (args.size() < 2) throw std::runtime_error("usage: download <track-id>");
        std::cout << controller.download(args[1]) << '\n';
    } else if (command == "downloads") {
        for (const auto& song : controller.downloads()) {
            std::cout << song.id << "  |  " << song.title << "  |  " << song.path << '\n';
        }
    } else {
        throw std::runtime_error("unknown command: " + command);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        resona::Paths paths;
        PlayerController controller(paths);
        if (argc > 1) {
            std::vector<std::string> args;
            for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
            runCommand(controller, args, true);
            return 0;
        }

        printHelp();
        std::string line;
        while (true) {
            std::cout << "player> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            const auto args = tokenize(line);
            if (args.empty()) continue;
            try {
                if (!runCommand(controller, args, false)) break;
            } catch (const std::exception& error) {
                std::cout << "error: " << error.what() << '\n';
            }
        }
        controller.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
