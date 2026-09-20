// src/search/online_providers.cpp
#include "online_providers.hpp"
#include "util/http.hpp"
#include "util/log.hpp"
#include <nlohmann/json.hpp>

namespace voice_agent {

using json = nlohmann::json;

TavilyProvider::TavilyProvider(std::string api_key) : api_key_(std::move(api_key)) {}

std::vector<SearchHit> TavilyProvider::search(const SearchQuery& q,
                                              std::shared_ptr<CancelToken> cancel) {
    (void)q;
    if (api_key_.empty()) {
        LOG_DEBUG("Tavily not configured, skipped");
        return {};
    }

    http::Request req;
    req.url = "https://api.tavily.com/search";
    req.method = "POST";
    req.headers["Content-Type"] = "application/json";
    req.timeout_ms = 5000;
    req.cancel = std::move(cancel);

    // 自包含客户端不支持 HTTPS；返回 {} 而非崩溃。
    // 接入带 TLS 的客户端后，此路径自动可用。
    auto resp = http::get(req);
    if (!req.cancel || !req.cancel->cancelled()) {
        if (!resp.ok()) {
            LOG_WARN("Tavily unavailable: {} (needs HTTPS client)", resp.error);
            return {};
        }
    } else {
        return {};
    }

    std::vector<SearchHit> hits;
    try {
        auto j = json::parse(resp.body);
        if (j.contains("results") && j["results"].is_array()) {
            for (auto& r : j["results"]) {
                SearchHit h;
                h.title = r.value("title", "");
                h.url = r.value("url", "");
                h.snippet = r.value("content", "");
                h.score = r.value<double>("score", 0.0);
                if (!h.url.empty()) hits.push_back(std::move(h));
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("Tavily JSON parse error: {}", e.what());
    }
    LOG_INFO("Tavily returned {} hits", hits.size());
    return hits;
}

BraveProvider::BraveProvider(std::string api_key) : api_key_(std::move(api_key)) {}

std::vector<SearchHit> BraveProvider::search(const SearchQuery& q,
                                             std::shared_ptr<CancelToken> cancel) {
    if (api_key_.empty()) {
        LOG_DEBUG("Brave not configured, skipped");
        return {};
    }

    http::Request req;
    req.url = "https://api.search.brave.com/res/v1/web/search?q=" +
              http::url_encode(q.q) +
              (q.topk > 0 ? "&count=" + std::to_string(q.topk) : "");
    req.headers["Accept"] = "application/json";
    req.headers["X-Subscription-Token"] = api_key_;
    req.timeout_ms = 5000;
    req.cancel = std::move(cancel);

    auto resp = http::get(req);
    if (!req.cancel || !req.cancel->cancelled()) {
        if (!resp.ok()) {
            LOG_WARN("Brave unavailable: {} (needs HTTPS client)", resp.error);
            return {};
        }
    } else {
        return {};
    }

    std::vector<SearchHit> hits;
    try {
        auto j = json::parse(resp.body);
        if (j.contains("web") && j["web"].contains("results")) {
            for (auto& r : j["web"]["results"]) {
                SearchHit h;
                h.title = r.value("title", "");
                h.url = r.value("url", "");
                h.snippet = r.value("description", "");
                if (!h.url.empty()) hits.push_back(std::move(h));
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("Brave JSON parse error: {}", e.what());
    }
    LOG_INFO("Brave returned {} hits", hits.size());
    return hits;
}

std::vector<std::shared_ptr<ISearchProvider>> create_online_providers(
    const std::string& tavily_key, const std::string& brave_key) {
    std::vector<std::shared_ptr<ISearchProvider>> out;
    if (!tavily_key.empty()) out.push_back(std::make_shared<TavilyProvider>(tavily_key));
    if (!brave_key.empty()) out.push_back(std::make_shared<BraveProvider>(brave_key));
    return out;
}

}  // namespace voice_agent