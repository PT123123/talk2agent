// src/search/searxng_provider.hpp
#pragma once
#include "search/isearch.hpp"
#include <string>

namespace voice_agent {

// ========== SearXNG Provider（本地，自托管）==========
class SearxngProvider : public ISearchProvider {
public:
    explicit SearxngProvider(std::string base_url)
        : base_url_(std::move(base_url)) {}

    std::string name() const override { return "searxng"; }
    bool is_online() const override { return false; }

    std::vector<SearchHit> search(const SearchQuery& q,
                                  std::shared_ptr<CancelToken> cancel) override;

private:
    std::string base_url_;  // 例如 "http://127.0.0.1:8080"
};

}  // namespace voice_agent