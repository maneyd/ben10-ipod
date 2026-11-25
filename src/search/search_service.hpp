#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include "domain/models.hpp"
#include "storage/paths.hpp"

namespace resona::search {

enum class SearchStatus { Idle, Empty, Loading, Success, Error };

struct SearchState {
    SearchStatus status = SearchStatus::Empty;
    std::vector<Track> songs;
    std::vector<ArtistSummary> artists;
    std::string continuationToken;
    bool loadingMore = false;
    std::string error;
    std::vector<std::string> history;
    std::vector<BrowseSection> browse;
    bool browseLoading = true;
};

class SearchService {
public:
    explicit SearchService(std::vector<std::string> history);
    ~SearchService();

    void onQueryChange(const std::string& query);
    void submit(const std::string& query);
    void clear();
    void clearHistory();
    void loadMore();
    void poll();

    const SearchState& state() const { return state_; }

    std::function<void(const std::string&)> onRecordSearch;
    std::function<void(const Track&)> onRemember;

private:
    struct QueryResult {
        uint64_t generation = 0;
        std::string query;
        SongSearchPage page;
        std::string error;
    };
    struct BrowseResult {
        std::vector<BrowseSection> sections;
    };

    void startQuery(const std::string& query);
    void loadBrowse();
    static std::string trim(const std::string& value);

    Paths paths_;
    SearchState state_;
    std::string query_;
    uint64_t generation_ = 0;
    uint64_t activeGeneration_ = 0;
    std::future<QueryResult> queryFuture_;
    std::future<SongSearchPage> moreFuture_;
    std::string moreForQuery_;
    std::future<BrowseResult> browseFuture_;
};

}  // namespace resona::search
