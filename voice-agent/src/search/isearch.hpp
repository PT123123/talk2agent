// src/search/isearch.hpp
#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include "core/types.hpp"
#include "core/cancel_token.hpp"

namespace voice_agent {

// ========== 搜索策略 ==========
enum class SearchPolicy {
    LocalFirst,  // 本地优先，不足/超时回退在线
    LocalOnly,   // 仅本地
    Remote,      // 仅在线
};

const char* search_policy_to_string(SearchPolicy p);

// ========== 搜索 Provider 抽象 ==========
class ISearchProvider {
public:
    virtual ~ISearchProvider() = default;

    virtual std::string name() const = 0;
    virtual bool is_online() const = 0;   // true = 需要网络/凭证

    // 执行搜索；网络错误/超时应返回空 vector 而非抛异常。
    // 支持通过 cancel 取消。
    virtual std::vector<SearchHit> search(
        const SearchQuery& q,
        std::shared_ptr<CancelToken> cancel = nullptr) = 0;
};

// ========== 搜索路由（本地优先 + 回退 + 去重）==========
class SearchRouter {
public:
    SearchRouter() = default;

    // 注册一个 provider；优先级按加入顺序：靠前的先查。
    void add_provider(std::shared_ptr<ISearchProvider> provider);

    // 建议的合格结果数：结果 < threshold 时触发在线回退
    void set_fallback_threshold(int threshold) { fallback_threshold_ = threshold; }

    // 查询。本地优先，不足则回退在线。返回去重后的结果。
    std::vector<SearchHit> query(const SearchQuery& q, SearchPolicy policy,
                                 std::shared_ptr<CancelToken> cancel = nullptr);

private:
    std::vector<SearchHit> query_local_(const SearchQuery& q, std::shared_ptr<CancelToken>);
    std::vector<SearchHit> query_online_(const SearchQuery& q, std::shared_ptr<CancelToken>);
    static std::vector<SearchHit> dedup_(std::vector<SearchHit> hits);

    std::vector<std::shared_ptr<ISearchProvider>> providers_;
    std::vector<std::shared_ptr<ISearchProvider>> online_providers_;
    int fallback_threshold_{4};
};

}  // namespace voice_agent