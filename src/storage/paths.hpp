#pragma once

#include <filesystem>
#include <string>

namespace resona {

class Paths {
public:
    Paths();

    const std::filesystem::path& configDir() const { return configDir_; }
    const std::filesystem::path& dataDir() const { return dataDir_; }
    const std::filesystem::path& cacheDir() const { return cacheDir_; }
    std::filesystem::path musicDir() const;

private:
    std::filesystem::path configDir_;
    std::filesystem::path dataDir_;
    std::filesystem::path cacheDir_;
};

}  // namespace resona
