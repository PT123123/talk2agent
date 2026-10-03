// tests/test_model_router.cpp
// R4: Model Router —— 简单问题不烧重模型、复杂问题能升级、引擎缺失会降级
#include "orchestrator/model_router.hpp"
#include "orchestrator/response_policy.hpp"
#include "util/log.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace std;
using namespace voice_agent;

namespace {

// 可控的假引擎：记录被应用的参数，可切换可用性
class FakeEngine : public IModelEngine {
public:
    FakeEngine(std::string n, bool avail = true, bool tools = true)
        : name_(std::move(n)), available_(avail), tools_(tools) {}

    std::string name() const override { return name_; }
    bool available() const override { return available_; }
    bool supports_tools() const override { return tools_; }

    bool apply_profile(const LLMGenerationProfile& p) override {
        applied.push_back(p);
        current_ = p;
        return true;
    }
    LLMGenerationProfile profile() const override { return current_; }

    void generate_stream(const std::string&,
                         const std::function<void(const std::string&)>&,
                         std::shared_ptr<CancelToken>) override {}
    void stop() override {}

    void set_available(bool v) { available_ = v; }

    std::vector<LLMGenerationProfile> applied;

private:
    std::string name_;
    bool available_;
    bool tools_;
    LLMGenerationProfile current_;
};

}  // namespace

// ---- 1. 默认参数档位符合语音场景直觉 ----
static void test_default_profiles() {
    cout << "TEST default generation profiles..." << endl;

    auto fast = default_profile_for(ModelTier::Fast);
    auto normal = default_profile_for(ModelTier::Normal);
    auto deep = default_profile_for(ModelTier::Deep);
    auto agent = default_profile_for(ModelTier::Agent);

    // FAST 要快：低温度 + 短输出
    assert(fast.temperature < normal.temperature);
    assert(fast.max_tokens < normal.max_tokens);
    assert(fast.top_k < 50);

    // DEEP 要准：温度比 NORMAL 低（宁可慢也不能胡编），但输出更长
    assert(deep.temperature < normal.temperature);
    assert(deep.max_tokens > normal.max_tokens);

    // AGENT 要稳：工具参数不能编
    assert(agent.temperature <= 0.2f);
    cout << "  (fast t=" << fast.temperature << "/" << fast.max_tokens
         << "tok, deep t=" << deep.temperature << "/" << deep.max_tokens
         << "tok) -> PASS" << endl;
}

// ---- 2. 简单问题不烧重模型（核心验收）----
static void test_simple_question_uses_cheap_profile() {
    cout << "TEST simple question uses cheap profile..." << endl;

    ModelRouter router;
    auto engine = make_shared<FakeEngine>("local");
    for (auto t : {ModelTier::Fast, ModelTier::Normal, ModelTier::Deep})
        router.register_engine(t, engine);

    // "现在几点" -> Policy 判 QuickReply / FAST
    auto r = router.route(ModelTier::Fast);
    assert(r.tier == ModelTier::Fast);
    assert(!r.degraded);
    assert(r.profile.max_tokens <= 128);
    assert(r.profile.temperature <= 0.35f);

    // 同一问题的开销应远低于深度档
    auto deep = router.route(ModelTier::Deep);
    assert(deep.profile.max_tokens > r.profile.max_tokens * 4);
    cout << "  (fast=" << r.profile.max_tokens << "tok vs deep="
         << deep.profile.max_tokens << "tok) -> PASS" << endl;
}

// ---- 3. 参数真的落到引擎上 ----
static void test_profile_applied_to_engine() {
    cout << "TEST profile reaches engine..." << endl;

    ModelRouter router;
    auto engine = make_shared<FakeEngine>("local");
    router.register_engine(ModelTier::Deep, engine);

    auto r = router.route(ModelTier::Deep);
    assert(r.engine == "local");
    assert(!engine->applied.empty());
    assert(engine->applied.back().temperature == r.profile.temperature);
    assert(engine->applied.back().max_tokens == r.profile.max_tokens);
    cout << "  -> PASS" << endl;
}

// ---- 4. 引擎不可用时降级，而不是静默失败 ----
static void test_degrades_when_engine_unavailable() {
    cout << "TEST degrade when engine unavailable..." << endl;

    ModelRouter::Config cfg;
    cfg.allow_degrade = true;
    ModelRouter router(cfg);

    auto broken = make_shared<FakeEngine>("broken", /*avail=*/false);
    auto backup = make_shared<FakeEngine>("backup", /*avail=*/true);
    router.register_engine(ModelTier::Deep, broken);
    router.register_engine(ModelTier::Deep, backup);   // 同一档第二个候选

    auto r = router.route(ModelTier::Deep);
    // 应回退到可用的 backup，而不是失败
    assert(r.engine == "backup");
    cout << "  -> PASS" << endl;
}

// ---- 5. 整档缺失时也不该崩，走默认引擎 ----
static void test_missing_tier_uses_default() {
    cout << "TEST missing tier falls back to default engine..." << endl;

    ModelRouter router;   // 什么都不注册
    auto r = router.route(ModelTier::Deep);
    assert(r.tier == ModelTier::Deep);          // tier 保持不变
    assert(r.engine == "default");              // 引擎是默认
    assert(r.profile.max_tokens > 0);           // 参数仍是 Deep 档的
    assert(r.profile.max_tokens == default_profile_for(ModelTier::Deep).max_tokens);
    cout << "  -> PASS" << endl;
}

// ---- 6. 自定义 profile 覆盖默认 ----
static void test_custom_profile_overrides() {
    cout << "TEST custom profile overrides default..." << endl;

    ModelRouter router;
    LLMGenerationProfile custom;
    custom.temperature = 0.9f;
    custom.max_tokens = 2048;
    router.set_profile(ModelTier::Fast, custom);

    auto p = router.profile_for(ModelTier::Fast);
    assert(p.temperature == 0.9f);
    assert(p.max_tokens == 2048);
    cout << "  -> PASS" << endl;
}

// ---- 7. 统计：能看出"简单问题占比" ----
static void test_routing_stats() {
    cout << "TEST routing stats..." << endl;

    ModelRouter router;
    for (int i = 0; i < 7; ++i) router.route(ModelTier::Fast);
    for (int i = 0; i < 2; ++i) router.route(ModelTier::Normal);
    router.route(ModelTier::Deep);

    auto s = router.stats();
    assert(s.fast == 7);
    assert(s.normal == 2);
    assert(s.deep == 1);

    router.reset_stats();
    assert(router.stats().fast == 0);
    cout << "  (7 fast / 2 normal / 1 deep) -> PASS" << endl;
}

// ---- 8. Policy 的 tier 与 Router 打通 ----
static void test_policy_tier_feeds_router() {
    cout << "TEST policy tier feeds router..." << endl;

    ResponsePolicy policy;
    ModelRouter router;
    auto engine = make_shared<FakeEngine>("local");
    for (auto t : {ModelTier::Fast, ModelTier::Normal, ModelTier::Deep})
        router.register_engine(t, engine);

    struct Case { const char* text; ModelTier expect; };
    const Case cases[] = {
        {"现在几点", ModelTier::Fast},
        {"我最近真的有好多事情", ModelTier::Normal},
        {"帮我对比一下 Vulkan 和 DirectML 的架构差异", ModelTier::Deep},
    };

    for (const auto& c : cases) {
        ResponsePolicyInput in;
        in.user_text = c.text;
        in.intent = UserSpeechIntent::Content;
        auto d = policy.decide(in);
        assert(d.tier == c.expect);

        // Policy 选出的 tier 必须能被 Router 落地
        auto r = router.route(d.tier);
        assert(r.tier == c.expect);
        assert(!r.engine.empty());
    }
    cout << "  -> PASS" << endl;
}

// ---- 9. 工具能力差异：FAST 引擎不该拿工具 ----
static void test_engine_capability() {
    cout << "TEST engine capability flags..." << endl;

    ModelRouter router;
    auto no_tools = make_shared<FakeEngine>("plain", true, /*tools=*/false);
    auto with_tools = make_shared<FakeEngine>("tools", true, /*tools=*/true);
    router.register_engine(ModelTier::Fast, no_tools);
    router.register_engine(ModelTier::Agent, with_tools);

    assert(!router.engine_for(ModelTier::Fast)->supports_tools());
    assert(router.engine_for(ModelTier::Agent)->supports_tools());
    cout << "  -> PASS" << endl;
}

// ---- 10. 跨档降级链：DEEP 引擎缺失应退到 NORMAL，再退到 FAST ----
static void test_cross_tier_degrade_chain() {
    cout << "TEST cross-tier degrade chain..." << endl;

    ModelRouter::Config cfg;
    cfg.allow_degrade = true;
    ModelRouter router(cfg);

    // 只有 FAST 可用
    auto fast = make_shared<FakeEngine>("fast-engine", true);
    router.register_engine(ModelTier::Fast, fast);

    // 请求 DEEP，但 DEEP/NORMAL 都没引擎 -> 应降级到 FAST
    auto r = router.route(ModelTier::Deep);
    assert(r.degraded);
    assert(r.tier == ModelTier::Fast);
    assert(r.requested == ModelTier::Deep);    // 原始请求要保留
    assert(r.engine == "fast-engine");
    assert(r.profile.max_tokens == default_profile_for(ModelTier::Fast).max_tokens);
    assert(r.reason.find("degraded") != string::npos);
    cout << "  (DEEP -> " << model_tier_to_string(r.tier) << ") -> PASS" << endl;
}

// ---- 11. 降级链的第一站应该是 NORMAL（若可用）----
static void test_degrade_prefers_nearest_tier() {
    cout << "TEST degrade prefers nearest tier..." << endl;

    ModelRouter router;
    auto normal = make_shared<FakeEngine>("normal-engine", true);
    auto fast = make_shared<FakeEngine>("fast-engine", true);
    router.register_engine(ModelTier::Normal, normal);
    router.register_engine(ModelTier::Fast, fast);

    auto r = router.route(ModelTier::Deep);
    // DEEP 无引擎，NORMAL 有 -> 应停在 NORMAL，不该直接跳到 FAST
    assert(r.degraded);
    assert(r.tier == ModelTier::Normal);
    assert(r.engine == "normal-engine");
    cout << "  -> PASS" << endl;
}

// ---- 12. 降级不该往更贵的方向走 ----
static void test_never_degrade_upward() {
    cout << "TEST never degrades upward..." << endl;

    ModelRouter router;
    auto deep = make_shared<FakeEngine>("deep", true);
    router.register_engine(ModelTier::Deep, deep);

    // 请求 FAST，但只有 DEEP 有引擎。
    // 正确行为：保持 FAST 档 + 用默认引擎出话，而不是升级到 DEEP ——
    // 否则"简单问题不烧重模型"就被破坏了。
    auto r = router.route(ModelTier::Fast);
    assert(r.tier == ModelTier::Fast);
    assert(r.engine == "default");
    assert(r.profile.max_tokens <= 128);
    cout << "  (kept FAST, engine=default) -> PASS" << endl;
}

// ---- 13. 关掉降级后不回退 ----
static void test_degrade_can_be_disabled() {
    cout << "TEST degrade can be disabled..." << endl;

    ModelRouter::Config cfg;
    cfg.allow_degrade = false;
    ModelRouter router(cfg);

    auto fast = make_shared<FakeEngine>("fast", true);
    router.register_engine(ModelTier::Fast, fast);

    auto r = router.route(ModelTier::Deep);
    assert(!r.degraded);
    assert(r.tier == ModelTier::Deep);
    assert(r.engine == "default");
    cout << "  -> PASS" << endl;
}

// ---- 14. 降级要计入统计 ----
static void test_degrade_counted_in_stats() {
    cout << "TEST degrade counted in stats..." << endl;

    ModelRouter router;
    auto fast = make_shared<FakeEngine>("fast", true);
    router.register_engine(ModelTier::Fast, fast);

    router.route(ModelTier::Deep);
    router.route(ModelTier::Deep);
    router.route(ModelTier::Fast);

    auto s = router.stats();
    assert(s.fast == 1);      // 按"请求档位"计数，不是实际用的档
    assert(s.deep == 2);
    assert(s.degraded == 2);
    cout << "  (2 deep requests -> 2 degraded) -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-model-router", "warn");
    (void)logger;

    test_default_profiles();
    test_simple_question_uses_cheap_profile();
    test_profile_applied_to_engine();
    test_degrades_when_engine_unavailable();
    test_missing_tier_uses_default();
    test_custom_profile_overrides();
    test_routing_stats();
    test_policy_tier_feeds_router();
    test_engine_capability();
    test_cross_tier_degrade_chain();
    test_degrade_prefers_nearest_tier();
    test_never_degrade_upward();
    test_degrade_can_be_disabled();
    test_degrade_counted_in_stats();

    cout << "\nAll R4 model router tests PASSED" << endl;
    return 0;
}
