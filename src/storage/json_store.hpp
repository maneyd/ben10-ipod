#pragma once

#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>

namespace resona {

class JsonFile {
public:
    explicit JsonFile(std::filesystem::path path);

    nlohmann::json load(nlohmann::json fallback = nlohmann::json::array()) const;
    void save(const nlohmann::json& value) const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
};

}  // namespace resona
