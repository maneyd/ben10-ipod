#include "search/search_service.hpp"

#include <algorithm>
#include <cctype>
#include <future>
#include <set>

#include "network/http.hpp"
#include "youtube/innertube.hpp"
#include "youtube/visitor.hpp"

namespace resona::search {

namespace {

const char* kTrendingQueries[] = {
    "trending music",
    "trending songs right now",
    "trending pop",
    "trending hip hop",
    "trending electronic dance music",
    "trending rock",
    "trending k-pop",
};

SongSearchPage fetchSongs(const Paths& paths, const std::string& query) {
    Http http;
    Visitor visitor(paths, http);
    InnerTube innerTube(http, visitor);
    return innerTube.searchSongsPage(query);
}

}  // namespace

std::string SearchService::trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

SearchService::SearchService(std::vector<std::string> history) {
    state_.history = std::move(history);
    loadBrowse();
}

SearchService::~SearchService() {
    if (queryFuture_.valid()) queryFuture_.wait();
    if (moreFuture_.valid()) moreFuture_.wait();
    if (browseFuture_.valid()) browseFuture_.wait();
}

void SearchService::onQueryChange(const std::string& query) {
    const std::string trimmed = trim(query);
    if (trimmed.empty()) {
        clear();
        return;
    }
    ++generation_;
    activeGeneration_ = generation_;
    query_ = trimmed;
    state_.status = SearchStatus::Idle;
    state_.songs.clear();
    state_.artists.clear();
    state_.continuationToken.clear();
    state_.loadingMore = false;
    state_.error.clear();
}

void SearchService::submit(const std::string& query) {
    const std::string trimmed = trim(query);
    if (trimmed.empty()) return;
    query_ = trimmed;
    if (onRecordSearch) onRecordSearch(trimmed);
    state_.history.erase(std::remove_if(state_.history.begin(), state_.history.end(),
                                        [&](const std::string& item) {
                                            std::string a = item, b = trimmed;
                                            std::transform(a.begin(), a.end(), a.begin(),
                                                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                                            std::transform(b.begin(), b.end(), b.begin(),
                                                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                                            return a == b;
                                        }),
                         state_.history.end());
    state_.history.insert(state_.history.begin(), trimmed);
    startQuery(trimmed);
}

void SearchService::clear() {
    ++generation_;
    activeGeneration_ = generation_;
    query_.clear();
    auto history = state_.history;
    auto browse = state_.browse;
    state_ = SearchState{};
    state_.status = SearchStatus::Idle;
    state_.history = std::move(history);
    state_.browse = std::move(browse);
    state_.browseLoading = browseFuture_.valid();
}

void SearchService::clearHistory() {
    state_.history.clear();
}

void SearchService::startQuery(const std::string& query) {
    const uint64_t generation = ++generation_;
    activeGeneration_ = generation;
    state_.status = SearchStatus::Loading;
    state_.songs.clear();
    state_.artists.clear();
    state_.continuationToken.clear();
    state_.loadingMore = false;
    state_.error.clear();

    queryFuture_ = std::async(std::launch::async, [this, generation, query]() {
        QueryResult result;
        result.generation = generation;
        result.query = query;
        try {
            result.page = fetchSongs(paths_, query);
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        return result;
    });
}

void SearchService::loadMore() {
    if (state_.status != SearchStatus::Success || state_.loadingMore ||
        state_.continuationToken.empty()) {
        return;
    }
    const std::string token = state_.continuationToken;
    const std::string query = query_;
    state_.loadingMore = true;
    moreForQuery_ = query;
    moreFuture_ = std::async(std::launch::async, [this, token]() {
        Http http;
        Visitor visitor(paths_, http);
        InnerTube innerTube(http, visitor);
        return innerTube.loadMoreSongResults(token);
    });
}

void SearchService::loadBrowse() {
    state_.browseLoading = true;
    const std::string trending = kTrendingQueries[0];
    std::string recommended = "recommended music";
    if (!state_.history.empty()) recommended = state_.history.front();
    browseFuture_ = std::async(std::launch::async, [this, trending, recommended]() {
        BrowseResult result;
        for (const auto& [title, query] : {
                 std::pair<std::string, std::string>{"Trending Now", trending},
                 std::pair<std::string, std::string>{"Suggested For You", recommended},
             }) {
            try {
                auto page = fetchSongs(paths_, query);
                if (page.songs.empty()) continue;
                if (page.songs.size() > 12) page.songs.resize(12);
                result.sections.push_back({title, std::move(page.songs)});
            } catch (...) {
            }
        }
        return result;
    });
}

void SearchService::poll() {
    if (browseFuture_.valid() && browseFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = browseFuture_.get();
        state_.browse = std::move(result.sections);
        state_.browseLoading = false;
    }

    if (queryFuture_.valid() && queryFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = queryFuture_.get();
        if (result.generation == activeGeneration_) {
            if (!result.error.empty()) {
                state_.status = SearchStatus::Error;
                state_.error = result.error;
                state_.songs.clear();
                state_.artists.clear();
            } else if (result.page.songs.empty() && result.page.artists.empty()) {
                state_.status = SearchStatus::Empty;
            } else {
                state_.status = SearchStatus::Success;
                state_.songs = std::move(result.page.songs);
                state_.artists = std::move(result.page.artists);
                state_.continuationToken = std::move(result.page.continuationToken);
                if (onRemember) {
                    for (const auto& song : state_.songs) onRemember(song);
                }
            }
        }
    }

    if (moreFuture_.valid() && moreFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto page = moreFuture_.get();
        const bool stillCurrent = moreForQuery_ == query_ && state_.status == SearchStatus::Success;
        moreForQuery_.clear();
        state_.loadingMore = false;
        if (stillCurrent) {
            std::set<std::string> seen;
            for (const auto& song : state_.songs) seen.insert(song.videoId);
            for (const auto& song : page.songs) {
                if (seen.insert(song.videoId).second) {
                    state_.songs.push_back(song);
                    if (onRemember) onRemember(song);
                }
            }
            state_.continuationToken = std::move(page.continuationToken);
        }
    }
}

}  // namespace resona::search
