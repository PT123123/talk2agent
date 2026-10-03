// src/orchestrator/model_router.hpp
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "llm/llm.hpp"
#include "orchestrator/response_policy.hpp"

namespace voice_agent {

// ========== 生成参数档位 ==========
// 关键：Tier 不只是"换模型"，很多时候同一个模型换采样参数就够。
// llama.cpp 的 sampler chain 在 initialize 时固定，运行时改不了，
// 所以参数档位靠"路由时决定 + 引擎按档位重配"来落地。
struct LLMGenerationProfile {
    float temperature{0.7f};
    int max_tokens{512};
    float repeat_penalty{1.1f};
    int top_k{50};
    float top_p{0.95f};

    bool operator==(const LLMGenerationProfile& o) const {
        return temperature == o.temperature && max_tokens == o.max_tokens &&
               repeat_penalty == o.repeat_penalty && top_k == o.top_k &&
               top_p == o.top_p;
    }
};

// ========== Tier 路由结果 ==========
struct RouteResult {
    ModelTier tier{ModelTier::Normal};
    ModelTier requested{ModelTier::Normal};   // 原始请求（降级前）
    bool degraded{false};                     // 是否发生了降级
    std::string reason;                       // 理由（写进 trace）
    std::string engine;                       // 实际用的引擎标识
    LLMGenerationProfile profile;             // 实际用的生成参数
};

// ========== 引擎提供者 ==========
// 一个 Tier 可以映射到不同的引擎（本地 GGUF / 在线 API / 强模型）。
// 引擎不可用时 Router 会沿降级链继续找，而不是直接失败。
class IModelEngine {
public:
    virtual ~IModelEngine() = default;

    virtual std::string name() const = 0;
    virtual bool available() const = 0;          // 是否可用（加载成功 / 网络通）
    virtual bool supports_tools() const = 0;     // 是否支持 tool calling

    // 按档位配置生成参数。引擎若不支持运行时改参数，应返回 false
    // 并在下一次 generate 时重建 sampler。
    virtual bool apply_profile(const LLMGenerationProfile& p) = 0;
    virtual LLMGenerationProfile profile() const = 0;

    // 生成（流式）。token 回调 + 完成回调。
    virtual void generate_stream(const std::string& prompt,
                                const std::function<void(const std::string&)>& on_token,
                                std::shared_ptr<class CancelToken> cancel) = 0;

    virtual void stop() = 0;
};

// ========== ModelRouter ==========
// 决定"这一轮用哪一档"。目标：简单问题不烧重模型，复杂问题能升级。
//
// 降级链（引擎不可用时）：
//   FAST -> Normal -> Fast（保底必须能出话）
//   DEEP -> Normal -> Fast
//   任何 Tier 在引擎缺失时都不会静默失败，而是降级并记录 degraded。
class ModelRouter {
public:
    struct Config {
        // 是否允许降级。关掉则引擎不可用时直接返回失败（严格模式）
        bool allow_degrade{true};
        // 路由决策缓存：相同 tier 在无新信息时不重复查可用性
        bool cache_availability{true};
    };

    explicit ModelRouter(Config cfg = {}) : cfg_(cfg) {}

    // 注册引擎到某个 tier（一个 tier 可有多个候选，按注册顺序优先）
    void register_engine(ModelTier tier, std::shared_ptr<IModelEngine> engine);

    // 设置某 tier 的参数档位
    void set_profile(ModelTier tier, const LLMGenerationProfile& profile);

    // 取得某 tier 的参数档位（没有设置则返回默认）
    LLMGenerationProfile profile_for(ModelTier tier) const;

    // 路由。返回实际可用的档位 + 引擎 + 参数。
    // engine 可为 nullptr（表示"这一档不需要引擎"，如纯 Background/Search 摘要）
    //
    // 降级行为：
    //   1. 先在**同档**内按注册顺序找首个 available 引擎
    //   2. 同档都没有可用引擎时，沿**跨档降级链**往下找
    //      （如 DEEP 引擎缺失 -> NORMAL -> FAST）
    //   3. 降级结果记录 degraded=true 与实际 tier；仍找不到则用默认引擎（不失败）
    RouteResult route(ModelTier requested) const;

    // 取某档位的首选可用引擎（路由时用）
    std::shared_ptr<IModelEngine> engine_for(ModelTier tier) const;

    // 某档位的降级链（不含自身，从高到低）
    static const std::vector<ModelTier>& degrade_chain(ModelTier tier);

    // ---- 观测 ----
    // 各档位路由次数 / 降级次数（长跑调参用）
    struct Stats {
        int fast{0}, normal{0}, deep{0}, agent{0}, search{0}, background{0};
        int degraded{0};
    };
    Stats stats() const;

    void reset_stats();

private:
    static bool is_countable(ModelTier t);

    Config cfg_;
    mutable std::mutex mutex_;
    std::unordered_map<int, std::vector<std::shared_ptr<IModelEngine>>> engines_;
    std::unordered_map<int, LLMGenerationProfile> profiles_;
    mutable Stats stats_;
};

// ========== 默认参数档位 ==========
// 分档依据（针对语音场景，不是通用 LLM 常识）：
//   FAST   —— 抢答/ack/简短直答。要快、要稳，不要发散。
//   NORMAL —— 常规对话。默认值。
//   DEEP   —— 复杂推理。要长、要准、宁可慢也不能胡编。
LLMGenerationProfile default_profile_for(ModelTier tier);

}  // namespace voice_agent
