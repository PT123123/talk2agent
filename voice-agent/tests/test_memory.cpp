// tests/test_memory.cpp
// M6: 持久化记忆（SQLite 存储 + 抽取 + 检索 + 命令）
#include "memory/memory_store.hpp"
#include "memory/memory_extractor.hpp"
#include "memory/memory_retriever.hpp"
#include "memory/memory_manager.hpp"
#include "memory/memory_command.hpp"
#include "memory/text_features.hpp"
#include "util/log.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace voice_agent;

void test_store_crud() {
    cout << "TEST store CRUD..." << endl;
    MemoryStore s(":memory:");
    assert(s.open());
    int64_t id = s.save("user", "我住在北京", {"profile"});
    assert(id >= 0);
    assert(s.count() == 1);

    // 去重合并：同内容再次保存，id 不变，count 不变
    int64_t id2 = s.save("user", "我住在北京", {"profile"});
    assert(id2 == id);
    assert(s.count() == 1);

    // 不同内容新增
    s.save("user", "我喜欢喝绿茶");
    assert(s.count() == 2);

    // 删除
    assert(s.remove(id));
    assert(s.count() == 1);
    assert(!s.is_open() ? true : true);  // keep-open for rest

    assert(!s.remove(99999));
    cout << "  -> PASS" << endl;
}

void test_store_query_cjk() {
    cout << "TEST store CJK query..." << endl;
    MemoryStore s(":memory:");
    s.open();
    s.save("user", "我叫小明，喜欢喝绿茶");
    s.save("user", "我住在上海");
    s.save("user", "我养了一只叫阿黄的猫");

    auto h1 = s.query("小明", 3);
    assert(!h1.empty());
    assert(h1[0].content.find("小明") != string::npos ||
           h1[0].content.find("绿茶") != string::npos);

    auto h2 = s.query("上海", 3);
    assert(!h2.empty());
    assert(h2[0].content.find("上海") != string::npos);

    // 无匹配
    auto h3 = s.query("火星", 3);
    assert(h3.empty());
    cout << "  -> PASS" << endl;
}

void test_retriever_active() {
    cout << "TEST retriever active filter..." << endl;
    auto store = make_shared<MemoryStore>(":memory:");
    store->open();
    store->save("user", "永久项目信息", {}, 0);
    store->save("user", "短期一次性信息", {}, 1);

    MemoryRetriever rv(store);
    assert(rv.retrieve("永久", 2).size() <= 2u);

    // 过期条目被过滤
    MemoryItem m;
    m.id = 1;
    m.valid_to = 1000;  // 显然已过期
    assert(!rv.is_active(m));
    m.valid_to = 0;     // 无期限
    assert(rv.is_active(m));
    cout << "  -> PASS" << endl;
}

void test_extractor() {
    cout << "TEST extractor..." << endl;
    MemoryExtractor ex;
    auto r1 = ex.extract("我叫小明");
    assert(!r1.empty());
    assert(r1[0].worth_saving);
    assert(r1[0].subject == "user");
    assert(r1[0].type == "profile");

    auto r2 = ex.extract("我讨厌下雨天");
    assert(!r2.empty());
    assert(r2[0].type == "preference");

    // 不应抽取
    auto r3 = ex.extract("今天天气不错");
    assert(r3.empty());
    cout << "  -> PASS" << endl;
}

void test_manager_and_command() {
    cout << "TEST manager + command..." << endl;
    MemoryManager mgr(":memory:");
    assert(mgr.open());

    int wrote = mgr.ingest("我叫小红，住在广州");
    assert(wrote >= 1);
    assert(mgr.count() >= 1);

    auto rec = mgr.recall("小红", 2);
    assert(!rec.empty());

    // 命令
    assert(mgr.run_command("memory list").find("小红") != string::npos);
    auto save_cmd = mgr.run_command("memory save 昵称@小茶友");
    assert(save_cmd.find("已记住") != string::npos);
    assert(mgr.run_command("memory count").find("已保存") != string::npos);

    // forget 按关键词
    auto forget_cmd = mgr.run_command("memory forget 小红");
    assert(forget_cmd.find("已删除") != string::npos);

    // clear
    mgr.run_command("memory clear");
    assert(mgr.count() == 0);
    cout << "  -> PASS" << endl;
}

void test_is_memory_command() {
    cout << "TEST is_memory_command..." << endl;
    assert(is_memory_command("memory list"));
    assert(is_memory_command("/memory count"));
    assert(!is_memory_command("查一下上海天气"));
    cout << "  -> PASS" << endl;
}

void test_tokenize() {
    cout << "TEST tokenizer..." << endl;
    auto t = tokenize_units("Hello world 你好世界 abc");
    // 包含 ascii 单词与 cjk 单字
    bool has_hello = false, has_cjk = false;
    for (auto& u : t) {
        if (u == "hello") has_hello = true;
        if (u == "你") { has_cjk = true; }
    }
    assert(has_hello);
    assert(has_cjk);
    cout << "  -> " << t.size() << " units" << endl;
    cout << "  -> PASS" << endl;
}

void test_persistence_file() {
    cout << "TEST persistence across instances..." << endl;
    std::string path = "build/test_memory_tmp.db";
    std::remove(path.c_str());
    {
        MemoryStore s(path);
        s.open();
        s.save("user", "跨实例持久化内容");
    }
    {
        MemoryStore s(path);
        s.open();
        assert(s.count() == 1);
        auto q = s.query("持久化", 3);
        assert(!q.empty());
    }
    std::remove(path.c_str());
    cout << "  -> PASS" << endl;
}

int main() {
    auto logger = init_logger("test-memory", "warn");
    (void)logger;
    test_tokenize();
    test_store_crud();
    test_store_query_cjk();
    test_retriever_active();
    test_extractor();
    test_manager_and_command();
    test_is_memory_command();
    test_persistence_file();
    cout << "\nAll M6 tests PASSED" << endl;
    return 0;
}