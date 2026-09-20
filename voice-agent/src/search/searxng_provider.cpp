// src/search/searxng_provider.cpp
#include "searxng_provider.hpp"
#include "util/http.hpp"
#include "util/log.hpp"
#include <nlohmann/json.hpp>
#include <sstream>

namespace voice_agent {

using json = nlohmann::json;

std::vector<SearchHit> SearxngProvider::search(const SearchQuery& q,
                                               std::shared_ptr<CancelToken> cancel) {
    // 去掉末尾的 '/'
    std::string base = base_url_;
    while (!base.empty() && base.back() == '/') base.pop_back();

    std::string url = base + "/search?";
    std::ostringstream args;
    args << "q=" << http::url_encode(q.q)
         << "&format=json";
    if (!q.lang.empty()) args << "&language=" << http::url_encode(q.lang);
    if (q.topk > 0) args << "&num=" << q.topk;  // SearXNG 可能忽略 num
    if (q.time_range) args << "&time_range=" << http::url_encode(*q.time_range);
    url += args.str();

    http::Request req;
    req.url = url;
    req.timeout_ms = 5000;
    req.cancel = std::move(cancel);

    auto resp = http::get(req);
    if (!req.cancel || !req.cancel->cancelled()) {
        if (!resp.ok()) {
            LOG_WARN("SearXNG query failed: {} (status {})", resp.error, resp.status);
            return {};
        }
    } else {
        return {};
    }

    // 解析 JSON results[]
    std::vector<SearchHit> hits;
    try {
        auto j = json::parse(resp.body);
        if (!j.contains("results") || !j["results"].is_array()) {
            LOG_WARN("SearXNG response missing 'results' array");
            return {};
        }
        for (auto& r : j["results"]) {
            if (!r.is_object()) continue;
            SearchHit h;
            h.title = r.value("title", "");
            h.url = r.value("url", "");
            h.snippet = r.value("content", "");
            if (r.contains("publishedDate") && r["publishedDate"].is_string())
                h.published_at = r["publishedDate"].get<std::string>();
            if (r.contains("score") && r["score"].is_number())
                h.score = r["score"].get<double>();
            // 过滤无链接结果
            if (h.url.empty()) continue;
            hits.push_back(std::move(h));
        }
    } catch (const std::exception& e) {
        LOG_WARN("SearXNG JSON parse error: {}", e.what());
        return {};
    }

    LOG_INFO("SearXNG returned {} hits for '{}'", hits.size(), q.q);
    return hits;
}

}  // namespace voice_agent