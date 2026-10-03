// src/orchestrator/model_router.cpp
#include "model_router.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace voice_agent {

// ========== 默认参数档位 ==========

LLMGenerationProfile default_profile_for(ModelTier tier) {
    LLMGenerationProfile p;
    switch (tier) {
        case ModelTier::Fast:
            // 抢答/ack/简短直答：要快、要稳、不要发散。
            // 低温度是关键 —— 抢答时说错话比慢半秒糟糕得多。
            p.temperature = 0.3f;
            p.max_tokens = 96;
            p.top_k = 20;
            p.top_p = 0.9f;
            break;
        case ModelTier::Normal:
            p.temperature = 0.7f;
            p.max_tokens = 512;
            break;
        case ModelTier::Deep:
            // 复杂推理：宁可慢也不能胡编，所以温度压低、长度放开。
            p.temperature = 0.4f;
            p.max_tokens = 1536;
            p.top_p = 0.95f;
            break;
        case ModelTier::Agent:
            // 工具调用要稳，温度太高会编出不存在的参数
            p.temperature = 0.2f;
            p.max_tokens = 768;
            break;
        case ModelTier::Search:
            // 摘要类：要忠于原文，不要自行发挥
            p.temperature = 0.3f;
            p.max_tokens = 640;
            break;
        case ModelTier::Background:
            p.temperature = 0.5f;
            p.max_tokens = 512;
            break;
    }
    return p;
}

// ========== ModelRouter ==========

bool ModelRouter::is_countable(ModelTier t) {
    return true;   // 六档都计数
}

void ModelRouter::register_engine(ModelTier tier,
                                  std::shared_ptr<IModelEngine> engine) {
    if (!engine) return;
    std::lock_guard<std::mutex> lock(mutex_);
    engines_[static_cast<int>(tier)].push_back(std::move(engine));
    LOG_INFO("Model engine registered for tier {}", model_tier_to_string(tier));
}

void ModelRouter::set_profile(ModelTier tier, const LLMGenerationProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_[static_cast<int>(tier)] = profile;
}

LLMGenerationProfile ModelRouter::profile_for(ModelTier tier) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = profiles_.find(static_cast<int>(tier));
    if (it != profiles_.end()) return it->second;
    return default_profile_for(tier);
}

std::shared_ptr<IModelEngine> ModelRouter::engine_for(ModelTier tier) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = engines_.find(static_cast<int>(tier));
    if (it == engines_.end()) return nullptr;
    for (const auto& e : it->second) {
        if (e && e->available()) return e;
    }
    return nullptr;
}

// ========== 降级链 ==========
// 原则：宁可降级出话，也不要静默失败 —— 用户不该感知到"某个档没模型"。
// 但**绝不降级到比请求更低质量却更贵的档**：降级只往"更便宜/更快"走。
const std::vector<ModelTier>& ModelRouter::degrade_chain(ModelTier tier) {
    static const std::vector<ModelTier> kDeep = {
        ModelTier::Normal, ModelTier::Fast
    };
    static const std::vector<ModelTier> kNormal = {
        ModelTier::Fast
    };
    // Agent 档：工具不可用就退到 Normal（不带工具也能答）
    static const std::vector<ModelTier> kAgent = {
        ModelTier::Normal, ModelTier::Fast
    };
    // Search 档：摘要在本地模型上也能做
    static const std::vector<ModelTier> kSearch = {
        ModelTier::Normal, ModelTier::Fast
    };
    static const std::vector<ModelTier> kFast = {};
    static const std::vector<ModelTier> kBackground = {
        ModelTier::Normal, ModelTier::Fast
    };
    switch (tier) {
        case ModelTier::Deep:       return kDeep;
        case ModelTier::Normal:     return kNormal;
        case ModelTier::Agent:      return kAgent;
        case ModelTier::Search:     return kSearch;
        case ModelTier::Fast:       return kFast;
        case ModelTier::Background: return kBackground;
    }
    return kFast;
}

RouteResult ModelRouter::route(ModelTier requested) const {
    RouteResult r;
    r.requested = requested;

    // 计数按"请求的档位"记，而不是实际用的 —— 统计要回答的是
    // "用户在问什么量级的问题"，不是"最后用了什么"。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (requested) {
            case ModelTier::Fast:       ++stats_.fast; break;
            case ModelTier::Normal:     ++stats_.normal; break;
            case ModelTier::Deep:       ++stats_.deep; break;
            case ModelTier::Agent:      ++stats_.agent; break;
            case ModelTier::Search:     ++stats_.search; break;
            case ModelTier::Background: ++stats_.background; break;
        }
    }

    // ---- 1. 同档内找可用引擎 ----
    if (auto e = engine_for(requested)) {
        r.tier = requested;
        r.degraded = false;
        r.engine = e->name();
        r.profile = profile_for(requested);
        e->apply_profile(r.profile);
        r.reason = "routed " + std::string(model_tier_to_string(requested));
        return r;
    }

    // ---- 2. 跨档降级 ----
    if (cfg_.allow_degrade) {
        for (ModelTier fallback : degrade_chain(requested)) {
            if (auto e = engine_for(fallback)) {
                r.tier = fallback;
                r.degraded = true;
                r.engine = e->name();
                r.profile = profile_for(fallback);
                e->apply_profile(r.profile);
                r.reason = std::string("degraded ") +
                           model_tier_to_string(requested) + " -> " +
                           model_tier_to_string(fallback) +
                           " (no available engine at requested tier)";
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.degraded;
                }
                LOG_WARN("Model router degraded: {}", r.reason);
                return r;
            }
        }
    }

    // ---- 3. 全都没有：用默认引擎出话，不静默失败 ----
    r.tier = requested;
    r.degraded = false;
    r.engine = "default";
    r.profile = profile_for(requested);
    r.reason = "no engine available at any tier, using default (tier kept)";
    return r;
}

ModelRouter::Stats ModelRouter::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void ModelRouter::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
}

}  // namespace voice_agent
