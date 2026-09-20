// tests/test_agent.cpp
// M5: Agent 结构化输出 + 工具循环 + 搜索
#include "agent/grammar.hpp"
#include "agent/tool_registry.hpp"
#include "agent/tools.hpp"
#include "agent/agent_loop.hpp"
#include "memory/memory_store.hpp"
#include "search/isearch.hpp"
#include "search/searxng_provider.hpp"
#include "search/online_providers.hpp"
#include "llm/llm.hpp"
#include "util/log.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;
using namespace voice_agent;

namespace {
std::shared_ptr<MemoryStore> test_memory() {
    auto s = std::make_shared<MemoryStore>(":memory:");
    s->open();
    return s;
}
}

void test_grammar() {
    cout << "TEST grammar generation..." << endl;
    ToolRegistry r;
    r.register_tool("web_search", "desc",
                    json{{"type", "object"},
                         {"properties", {{"query", {{"type", "string"}}}}},
                         {"required", json::array({"query"})}},
                    [](const json&, shared_ptr<CancelToken>) { return ToolResult{}; });
    r.register_tool("get_time", "desc", json{{"type", "object"}},
                    [](const json&, shared_ptr<CancelToken>) { return ToolResult{}; });

    auto defs = r.tool_defs();
    std::string gbnf = tool_calls_grammar(defs);
    assert(gbnf.find("root ::=") != string::npos);
    assert(gbnf.find("web_search") != string::npos);
    assert(gbnf.find("get_time") != string::npos);
    cout << "  GBNF size=" << gbnf.size() << " -> PASS" << endl;
}

void test_parse() {
    cout << "TEST parse_tool_calls..." << endl;
    const char* sample =
        "让我查一下。\n<tool_call>{\"name\":\"get_time\",\"arguments\":{}}</tool_call>\n"
        "<tool_call>{\"name\":\"web_search\",\"arguments\":{\"query\":\"北京天气\"}}</tool_call>";
    auto calls = parse_tool_calls(sample);
    assert(calls.size() == 2);
    assert(calls[0].name == "get_time");
    assert(calls[1].name == "web_search");
    assert(calls[1].arguments_json.find("北京天气") != string::npos);

    // 畸形块被跳过，不抛异常
    auto calls2 = parse_tool_calls("a<tool_call>{bad json</tool_call>b");
    assert(calls2.empty());
    cout << "  -> PASS" << endl;
}

void test_validate() {
    cout << "TEST validate_tool_args_json..." << endl;
    assert(validate_tool_args_json(R"({"query":"x"})", "web_search").empty());
    assert(!validate_tool_args_json(R"({"query":123})", "web_search").empty());
    assert(!validate_tool_args_json(R"({})", "web_search").empty());
    assert(validate_tool_args_json("not-json", "web_search").empty() == false);
    // get_time 无必填
    assert(validate_tool_args_json("{}", "get_time").empty());
    cout << "  -> PASS" << endl;
}

void test_registry_and_tools() {
    cout << "TEST registry + builtin tools..." << endl;
    auto router = make_shared<SearchRouter>();
    ToolKit kit{router, test_memory()};
    ToolRegistry r;
    register_builtin_tools(r, kit);

    assert(r.has("get_time"));
    assert(r.has("web_search"));
    assert(r.has("memory_save"));
    assert(r.has("memory_query"));

    ToolExecutor ex(r);

    // get_time
    auto t = ex.execute_one(ToolCall{"c1", "get_time", "{}"}, nullptr, 2000);
    assert(!t.is_error);
    assert(t.content.find("20") != string::npos);  // 年份

    // memory save + query
    auto s = ex.execute_one(ToolCall{"c2", "memory_save",
        R"({"subject":"user","content":"我住在上海，喜欢喝茶"})"}, nullptr, 2000);
    assert(!s.is_error);
    auto q = ex.execute_one(ToolCall{"c3", "memory_query", R"({"q":"上海"})"}, nullptr, 2000);
    assert(q.content.find("上海") != string::npos);

    // 未知工具
    auto unk = ex.execute_one(ToolCall{"c4", "nope", "{}"}, nullptr, 2000);
    assert(unk.is_error);

    // 参数校验失败（缺 query）
    auto bad = ex.execute_one(ToolCall{"c5", "web_search", "{}"}, nullptr, 2000);
    assert(bad.is_error);
    cout << "  -> PASS" << endl;
}

void test_executor_timeout_and_cancel() {
    cout << "TEST executor timeout + cancel..." << endl;
    ToolRegistry r;
    r.register_tool("slow", "desc", json{{"type", "object"}},
                    [](const json&, shared_ptr<CancelToken> ct) {
                        this_thread::sleep_for(chrono::milliseconds(500));
                        (void)ct;
                        return ToolResult{};
                    });
    ToolExecutor ex(r);

    auto res = ex.execute_one(ToolCall{"s1", "slow", "{}"}, nullptr, 60);
    assert(res.is_error);
    assert(res.content.find("timeout") != string::npos);
    cout << "  -> PASS" << endl;
}

void test_search_offline_safe() {
    cout << "TEST search offline-safe..." << endl;
    auto router = make_shared<SearchRouter>();
    router->add_provider(make_shared<SearxngProvider>("http://127.0.0.1:1"));  // 必然连不上
    router->set_fallback_threshold(4);
    SearchQuery q;
    q.q = "测试";
    auto hits = router->query(q, SearchPolicy::LocalFirst, nullptr);
    // 本地失败+无在线 key -> 不抛异常，返回空
    assert(hits.empty());
    cout << "  -> PASS" << endl;
}

void test_agent_loop() {
    cout << "TEST agent loop with mock LLM..." << endl;
    LLM llm;
    LLMConfig cfg;
    cfg.model_path = "mock";
    cfg.n_ctx = 4096;
    if (!llm.initialize(cfg)) {
        cout << "  LLM init failed, skipping" << endl;
        return;
    }

    ToolRegistry r;
    ToolKit kit{make_shared<SearchRouter>(), test_memory()};
    register_builtin_tools(r, kit);
    ToolExecutor ex(r);
    AgentLoop loop(llm, r, ex);
    loop.set_system_prompt("你是助手。");

    std::string streamed;
    loop.set_token_sink([&](const std::string& t) { streamed += t; });

    auto result = loop.run("你好", nullptr);
    assert(!result.final_text.empty());
    assert(!streamed.empty());
    cout << "  rounds=" << result.tool_rounds << " text='" << result.final_text << "' -> PASS" << endl;
}

void test_cancel_aborts() {
    cout << "TEST cancel aborts agent loop..." << endl;
    LLM llm;
    LLMConfig cfg;
    cfg.model_path = "mock";
    if (!llm.initialize(cfg)) return;
    ToolRegistry r;
    register_builtin_tools(r, ToolKit{make_shared<SearchRouter>(), test_memory()});
    ToolExecutor ex(r);
    AgentLoop loop(llm, r, ex);

    auto cancel = make_shared<CancelToken>();
    cancel->cancel();  // 预置取消
    auto result = loop.run("你好", cancel);
    assert(result.cancelled);
    cout << "  -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-agent", "warn");  // 仅打印错误，测试输出走 cout
    (void)logger;
    test_grammar();
    test_parse();
    test_validate();
    test_registry_and_tools();
    test_executor_timeout_and_cancel();
    test_search_offline_safe();
    test_agent_loop();
    test_cancel_aborts();
    cout << "\nAll M5 tests PASSED" << endl;
    return 0;
}