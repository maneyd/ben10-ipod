#include "storage/paths.hpp"

#include <cstdlib>

namespace resona {

namespace {

std::filesystem::path envOr(const char* name, const std::filesystem::path& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return std::filesystem::path(value);
}

}  // namespace

Paths::Paths() {
    const auto home = envOr("HOME", std::filesystem::path("/tmp"));
    configDir_ = envOr("XDG_CONFIG_HOME", home / ".config") / "resona-player";
    dataDir_ = envOr("XDG_DATA_HOME", home / ".local/share") / "resona-player";
    cacheDir_ = envOr("XDG_CACHE_HOME", home / ".cache") / "resona-player";
    std::filesystem::create_directories(configDir_);
    std::filesystem::create_directories(dataDir_);
    std::filesystem::create_directories(cacheDir_);
}

std::filesystem::path Paths::musicDir() const {
    return envOr("XDG_MUSIC_DIR", envOr("HOME", std::filesystem::path("/tmp")) / "Music");
}

}  // namespace resona
