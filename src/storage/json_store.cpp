#include "storage/json_store.hpp"

#include <fstream>

namespace resona {

JsonFile::JsonFile(std::filesystem::path path) : path_(std::move(path)) {}

nlohmann::json JsonFile::load(nlohmann::json fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream input(path_);
    if (!input.good()) return fallback;
    try {
        nlohmann::json value;
        input >> value;
        return value;
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

void JsonFile::save(const nlohmann::json& value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::create_directories(path_.parent_path());
    const auto temp = path_.string() + ".tmp";
    std::ofstream output(temp, std::ios::trunc);
    output << value.dump(2) << '\n';
    output.close();
    std::filesystem::rename(temp, path_);
}

}  // namespace resona
