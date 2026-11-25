#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "domain/models.hpp"
#include "storage/library_store.hpp"

namespace resona {

class LocalScanner {
public:
    explicit LocalScanner(LibraryStore& store);

    std::vector<Track> scan(const std::vector<std::filesystem::path>& roots);
    std::vector<Track> cached() const;

private:
    LibraryStore& store_;
};

}  // namespace resona
