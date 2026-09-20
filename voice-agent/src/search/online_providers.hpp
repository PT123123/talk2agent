// src/search/online_providers.hpp
#pragma once
#include "search/isearch.hpp"
#include <memory>
#include <string>
#include <map>

namespace voice_agent {

// ========== Tavily Provider（在线，需 API key）==========
// 默认禁用；TLS 支持需在 util/http 中接入 cpr/libcurl。
class TavilyProvider : public ISearchProvider {
public:
    explicit TavilyProvider(std::string api_key);
    std::string name() const override { return "tavily"; }
    bool is_online() const override { return true; }
    std::vector<SearchHit> search(const SearchQuery& q,
                                  std::shared_ptr<CancelToken> cancel) override;

private:
    std::string api_key_;
};

// ========== Brave Provider（在线，需 API key）==========
class BraveProvider : public ISearchProvider {
public:
    explicit BraveProvider(std::string api_key);
    std::string name() const override { return "brave"; }
    bool is_online() const override { return true; }
    std::vector<SearchHit> search(const SearchQuery& q,
                                  std::shared_ptr<CancelToken> cancel) override;

private:
    std::string api_key_;
};

// 依据已配置的 key 创建在线 provider 列表；返回空表示均未配置。
std::vector<std::shared_ptr<ISearchProvider>> create_online_providers(
    const std::string& tavily_key, const std::string& brave_key);

}  // namespace voice_agent