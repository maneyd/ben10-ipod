#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace resona {

struct HttpResponse {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct RangeResult {
    long status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    int64_t total = -1;
    int64_t start = -1;
    int64_t end = -1;
};

class Http {
public:
    Http();

    HttpResponse request(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers = {},
                         const std::string& body = {},
                         const std::string& userAgent = {});

    HttpResponse get(const std::string& url,
                     const std::vector<std::pair<std::string, std::string>>& headers = {},
                     const std::string& userAgent = {}) {
        return request("GET", url, headers, {}, userAgent);
    }

    HttpResponse post(const std::string& url, const std::string& body,
                      const std::vector<std::pair<std::string, std::string>>& headers = {},
                      const std::string& userAgent = {}) {
        return request("POST", url, headers, body, userAgent);
    }

    RangeResult range(const std::string& url, int64_t start, int64_t length,
                      const std::vector<std::pair<std::string, std::string>>& headers = {},
                      const std::string& userAgent = {});
};

}  // namespace resona
