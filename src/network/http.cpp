#include "network/http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>

namespace resona {

namespace {

std::once_flag curlInitFlag;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

size_t writeCallback(char* data, size_t size, size_t count, void* userData) {
    auto* output = static_cast<std::string*>(userData);
    output->append(data, size * count);
    return size * count;
}

size_t headerCallback(char* data, size_t size, size_t count, void* userData) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userData);
    const std::string line(data, size * count);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        (*headers)[key] = value;
    }
    return size * count;
}

void parseContentRange(RangeResult& result) {
    const auto it = result.headers.find("content-range");
    if (it == result.headers.end()) return;
    const auto unitPos = it->second.find(' ');
    if (unitPos == std::string::npos) return;
    const auto range = it->second.substr(unitPos + 1);
    const auto slash = range.find('/');
    if (slash == std::string::npos) return;
    const auto dash = range.find('-');
    if (dash == std::string::npos) return;
    try {
        result.start = std::stoll(range.substr(0, dash));
        result.end = std::stoll(range.substr(dash + 1, slash - dash - 1));
        const auto totalText = range.substr(slash + 1);
        if (totalText != "*") result.total = std::stoll(totalText);
    } catch (...) {
    }
}

}  // namespace

Http::Http() {
    std::call_once(curlInitFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpResponse Http::request(const std::string& method, const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body, const std::string& userAgent) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("Could not initialize libcurl");

    HttpResponse response;
    curl_slist* headerList = nullptr;
    for (const auto& [key, value] : headers) {
        const std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (!userAgent.empty()) curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        const std::string message = curl_easy_strerror(code);
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP request failed: " + message);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return response;
}

RangeResult Http::range(const std::string& url, int64_t start, int64_t length,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        const std::string& userAgent) {
    auto allHeaders = headers;
    allHeaders.emplace_back("Range", "bytes=" + std::to_string(start) + "-" + std::to_string(start + length - 1));
    const auto response = request("GET", url, allHeaders, {}, userAgent);
    RangeResult result;
    result.status = response.status;
    result.body = response.body;
    result.headers = response.headers;
    parseContentRange(result);
    return result;
}

}  // namespace resona
