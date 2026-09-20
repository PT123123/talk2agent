// src/search/isearch.cpp
#include "isearch.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cstdint>
#include <set>

namespace voice_agent {

const char* search_policy_to_string(SearchPolicy p) {
    switch (p) {
        case SearchPolicy::LocalFirst: return "local-first";
        case SearchPolicy::LocalOnly:  return "local-only";
        case SearchPolicy::Remote:     return "remote";
    }
    return "unknown";
}

void SearchRouter::add_provider(std::shared_ptr<ISearchProvider> provider) {
    if (!provider) return;
    if (provider->is_online()) {
        online_providers_.push_back(std::move(provider));
    } else {
        providers_.push_back(std::move(provider));
    }
}

std::vector<SearchHit> SearchRouter::query(const SearchQuery& q, SearchPolicy policy,
                                           std::shared_ptr<CancelToken> cancel) {
    if (policy == SearchPolicy::Remote) {
        return query_online_(q, cancel);
    }

    auto local = query_local_(q, cancel);
    if (policy == SearchPolicy::LocalOnly) {
        return local;
    }

    if (cancel && cancel->cancelled()) return local;

    if (static_cast<int>(local.size()) < fallback_threshold_) {
        LOG_INFO("Search: local returned {} hits (<{}), falling back to online",
                 local.size(), fallback_threshold_);
        auto online = query_online_(q, cancel);
        local.insert(local.end(), online.begin(), online.end());
    }

    return dedup_(std::move(local));
}

std::vector<SearchHit> SearchRouter::query_local_(const SearchQuery& q,
                                                  std::shared_ptr<CancelToken> cancel) {
    std::vector<SearchHit> out;
    for (auto& p : providers_) {
        if (cancel && cancel->cancelled()) break;
        auto hits = p->search(q, cancel);
        out.insert(out.end(), hits.begin(), hits.end());
    }
    return out;
}

std::vector<SearchHit> SearchRouter::query_online_(const SearchQuery& q,
                                                   std::shared_ptr<CancelToken> cancel) {
    std::vector<SearchHit> out;
    for (auto& p : online_providers_) {
        if (cancel && cancel->cancelled()) break;
        auto hits = p->search(q, cancel);
        out.insert(out.end(), hits.begin(), hits.end());
    }
    return out;
}

// 归一化 URL（去 fragment 与排序忽略大小写），按 URL 去重。
std::vector<SearchHit> SearchRouter::dedup_(std::vector<SearchHit> hits) {
    std::vector<SearchHit> out;
    std::set<std::string> seen;
    for (auto& h : hits) {
        std::string key = h.url;
        auto frag = key.find('#');
        if (frag != std::string::npos) key.erase(frag);
        while (!key.empty() && key.back() == '/') key.pop_back();
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key.empty() || !seen.insert(key).second) continue;
        out.push_back(std::move(h));
    }
    return out;
}

}  // namespace voice_agent