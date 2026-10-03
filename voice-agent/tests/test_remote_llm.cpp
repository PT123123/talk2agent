// tests/test_remote_llm.cpp
// R7: OpenAI 兼容远程模型 —— 请求组装 / SSE 增量解析 / 路由与降级
//
// 注意：这里**不联网**。SSE 解析与请求组装都是纯函数，可以离线精确断言；
// 真实链路留给手工/端到端验证（需要 API key）。
#include "orchestrator/remote_llm.hpp"
#include "orchestrator/model_router.hpp"
#include "orchestrator/model_engines.hpp"
#include "util/http.hpp"
#include "util/log.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <iostream>
#include <string>

using namespace std;
using namespace voice_agent;
using json = nlohmann::json;

namespace {

RemoteLLMConfig base_cfg() {
    RemoteLLMConfig c;
    c.base_url = "https://api.example.com/v1";
    c.model = "test-model";
    c.api_key = "sk-test";
    return c;
}

// 直接调公开的纯函数（刻意做成 public，让协议层能脱离网络单测）
using Probe = RemoteLLM;

}  // namespace

// ============================================================
// 1. URL 解析支持 https
// ============================================================
static void test_url_https() {
    cout << "TEST url parsing supports https..." << endl;

    auto u = http::parse_url("https://api.openai.com/v1/chat/completions");
    assert(u.ok());
    assert(u.scheme == "https");
    assert(u.host == "api.openai.com");
    assert(u.effective_port() == 443);
    assert(u.is_tls());
    assert(u.path == "/v1/chat/completions");
    assert(u.target() == "/v1/chat/completions");

    // 显式端口
    u = http::parse_url("http://127.0.0.1:8080/search");
    assert(u.ok());
    assert(u.effective_port() == 8080);
    assert(!u.is_tls());

    // query
    u = http::parse_url("https://h/p?a=1&b=2");
    assert(u.query == "a=1&b=2");
    assert(u.target() == "/p?a=1&b=2");

    // 非法 scheme 必须被拒
    u = http::parse_url("ftp://x/y");
    assert(!u.ok());
    assert(!u.error.empty());
    cout << "  -> PASS" << endl;
}

// ============================================================
// 2. 请求 JSON 组装：只发服务端认识的字段
// ============================================================
static void test_request_json() {
    cout << "TEST request json assembly..." << endl;

    RemoteLLM::ChatRequest req;
    RemoteMessage sys;
    sys.role = "system";
    sys.content = "你是助手";
    req.messages.push_back(sys);
    RemoteMessage u;
    u.role = "user";
    u.content = "对比 Vulkan 和 DirectML";
    req.messages.push_back(u);

    LLMGenerationProfile p = default_profile_for(ModelTier::Deep);
    std::string body = Probe::build_request_json(req, p, base_cfg());
    json j = json::parse(body);

    assert(j["model"] == "test-model");
    assert(j["stream"] == true);
    assert(j["messages"].size() == 2);
    assert(j["messages"][0]["role"] == "system");
    assert(j["messages"][1]["content"] == "对比 Vulkan 和 DirectML");

    // 关键：top_k / top_p 不是 OpenAI 标准字段，发过去会 400
    assert(!j.contains("top_k"));
    assert(!j.contains("top_p"));
    assert(j.contains("temperature"));
    assert(j.contains("max_tokens"));
    cout << "  -> PASS" << endl;
}

// ============================================================
// 3. 工具定义组装
// ============================================================
static void test_request_json_with_tools() {
    cout << "TEST request json with tools..." << endl;

    RemoteLLM::ChatRequest req;
    RemoteMessage u;
    u.role = "user";
    u.content = "查一下天气";
    req.messages.push_back(u);

    RemoteLLM::ToolSpec t;
    t.name = "get_weather";
    t.description = "查询天气";
    t.json_schema = R"({"type":"object","properties":{"city":{"type":"string"}}})";
    req.tools.push_back(t);

    LLMGenerationProfile p = default_profile_for(ModelTier::Agent);
    json j = json::parse(Probe::build_request_json(req, p, base_cfg()));

    assert(j.contains("tools"));
    assert(j["tools"].size() == 1);
    assert(j["tools"][0]["type"] == "function");
    assert(j["tools"][0]["function"]["name"] == "get_weather");
    assert(j["tool_choice"] == "auto");

    // 空 schema 也要给出合法 parameters，不能崩也不能发空
    RemoteLLM::ChatRequest req2;
    req2.messages.push_back(u);
    RemoteLLM::ToolSpec t2;
    t2.name = "ping";
    t2.json_schema = "";
    req2.tools.push_back(t2);
    j = json::parse(Probe::build_request_json(req2, p, base_cfg()));
    assert(j["tools"][0]["function"]["parameters"].is_object());
    cout << "  -> PASS" << endl;
}

// ============================================================
// 4. SSE 文本增量解析
// ============================================================
static void test_sse_text_delta() {
    cout << "TEST SSE text delta parsing..." << endl;

    std::string text;
    bool done = false;

    // 典型 OpenAI 流式帧
    const std::string line =
        R"({"choices":[{"delta":{"content":"你好"},"index":0}]})";
    bool has = Probe::parse_delta(line, text, nullptr, &done);
    assert(has);
    assert(text == "你好");
    assert(!done);

    // 结束帧
    has = Probe::parse_delta(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})",
                       text, nullptr, &done);
    assert(!has);
    assert(done);

    // [DONE] 标记
    has = Probe::parse_delta("[DONE]", text, nullptr, &done);
    assert(!has);
    assert(done);

    // 非 JSON（心跳）必须被忽略而不是抛异常
    has = Probe::parse_delta(": ping", text, nullptr, &done);
    assert(!has);

    // 空 content 不算增量
    has = Probe::parse_delta(R"({"choices":[{"delta":{"content":""}}]})",
                       text, nullptr, &done);
    assert(!has);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 5. SSE 工具调用增量（跨帧拼接 arguments）
// ============================================================
static void test_sse_tool_call_delta() {
    cout << "TEST SSE tool call delta..." << endl;

    RemoteLLM::ToolCallDelta tool;
    std::string text;
    bool done = false;

    // 第一帧：给 id + name
    Probe::parse_delta(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
        R"("function":{"name":"get_weather","arguments":""}}]}}]})",
        text, &tool, &done);
    assert(tool.name == "get_weather");
    assert(tool.id == "call_1");

    // 第二帧：arguments 分片到达（这是 SSE 的常态）
    Probe::parse_delta(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
        R"("function":{"arguments":"{\"ci"}}]}}]})",
        text, &tool, &done);
    Probe::parse_delta(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
        R"("function":{"arguments":"ty\":\"北京\"}"}}]}}]})",
        text, &tool, &done);

    // arguments 必须是逐帧拼接的完整 JSON
    json args = json::parse(tool.arguments, nullptr, false);
    assert(!args.is_discarded());
    assert(args["city"] == "北京");
    cout << "  (args=" << tool.arguments << ") -> PASS" << endl;
}

// ============================================================
// 6. 错误帧不崩
// ============================================================
static void test_sse_error_frame() {
    cout << "TEST SSE error frame does not crash..." << endl;
    std::string text;
    bool done = false;
    // 超限/欠额等错误以 data 帧形式返回，必须被静默忽略
    bool has = Probe::parse_delta(
        R"({"error":{"message":"Rate limit reached","type":"rate_limit_error"}})",
        text, nullptr, &done);
    assert(!has);
    // 缺 choices 的帧
    has = Probe::parse_delta(R"({"id":"1","object":"chat.completion.chunk"})",
                       text, nullptr, &done);
    assert(!has);
    cout << "  -> PASS" << endl;
}

// ============================================================
// 7. available()：未配置就不可用，Router 据此降级
// ============================================================
static void test_availability() {
    cout << "TEST remote availability gates routing..." << endl;

    RemoteLLM ok(base_cfg());
    assert(ok.available());

    // 无端点
    RemoteLLMConfig c1 = base_cfg();
    c1.base_url = "";
    RemoteLLM no_url(c1);
    assert(!no_url.available());

    // 无模型名
    RemoteLLMConfig c2 = base_cfg();
    c2.model = "";
    RemoteLLM no_model(c2);
    assert(!no_model.available());

    // 本地 vLLM：无 api_key 也要算可用
    RemoteLLMConfig c3;
    c3.base_url = "http://127.0.0.1:8000/v1";
    c3.model = "local-model";
    RemoteLLM local(c3);
    assert(local.available());
    cout << "  -> PASS" << endl;
}

// ============================================================
// 8. 路由：DEEP 有远程时用远程，没远程时降级到本地
// ============================================================
static void test_routing_with_and_without_remote() {
    cout << "TEST routing with/without remote engine..." << endl;

    // 有远程
    {
        ModelRouter router;
        auto remote = std::make_shared<RemoteLLM>(base_cfg());
        auto local = std::make_shared<LocalLlamaEngine>(nullptr);
        router.register_engine(ModelTier::Deep, remote);
        router.register_engine(ModelTier::Deep, local);
        router.register_engine(ModelTier::Normal, local);

        auto r = router.route(ModelTier::Deep);
        assert(!r.degraded);
        assert(r.engine.rfind("remote:", 0) == 0);   // 优先远程
    }

    // 无远程：必须降级到本地，且记录 degraded
    {
        ModelRouter router;
        auto local = std::make_shared<LocalLlamaEngine>(nullptr);
        router.register_engine(ModelTier::Normal, local);
        router.register_engine(ModelTier::Fast, local);

        auto r = router.route(ModelTier::Deep);
        assert(r.degraded);
        assert(r.requested == ModelTier::Deep);
        assert(r.engine.rfind("remote:", 0) != 0);
        assert(r.tier == ModelTier::Normal);
    }
    cout << "  -> PASS" << endl;
}

// ============================================================
// 9. 降级绝不向上：只有远程引擎时，FAST 也该留在本地档
// ============================================================
static void test_fast_never_goes_remote() {
    cout << "TEST FAST tier never routed to remote..." << endl;

    ModelRouter router;
    auto remote = std::make_shared<RemoteLLM>(base_cfg());
    router.register_engine(ModelTier::Deep, remote);
    // 只给 DEEP 注册远程，FAST 什么都不注册

    auto r = router.route(ModelTier::Fast);
    // FAST 不在降级链上（kFast 为空），所以不会去找远程
    assert(r.tier == ModelTier::Fast);
    assert(r.engine.rfind("remote:", 0) != 0);
    assert(r.profile.max_tokens <= 128);
    cout << "  -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-remote-llm", "warn");
    (void)logger;

    test_url_https();
    test_request_json();
    test_request_json_with_tools();
    test_sse_text_delta();
    test_sse_tool_call_delta();
    test_sse_error_frame();
    test_availability();
    test_routing_with_and_without_remote();
    test_fast_never_goes_remote();

    cout << "\nAll R7 remote LLM tests PASSED" << endl;
    return 0;
}
