#pragma once

#include <optional>
#include <string>

#include "network/http.hpp"
#include "storage/paths.hpp"

namespace resona {

class Visitor {
public:
    explicit Visitor(const Paths& paths, Http& http);

    std::optional<std::string> current() const;
    void seed(const std::string& token);
    void clear();

    void ensure(const std::string& videoId);
    std::optional<std::string> remint();
    std::optional<std::string> fetchPlayerJsUrl(const std::string& videoId);

private:
    Http& http_;
    std::string tokenFile_;
    mutable std::string token_;
};

}  // namespace resona
