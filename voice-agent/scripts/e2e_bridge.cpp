// 端到端：C++ TtsBridge 客户端 ↔ 真实 Python bridge 进程
// 用法: e2e_bridge <engine> [port] [wait_budget_ms]
#include "tts/tts_bridge.hpp"
#include "util/log.hpp"
#include <iostream>
#include <cassert>
#include <cstdlib>
using namespace voice_agent;

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: e2e <engine> [port] [wait_ms]\n"; return 2; }
    const std::string eng = argv[1];
    const int port = argc > 2 ? atoi(argv[2]) : 8773;
    const int wait_ms = argc > 3 ? atoi(argv[3]) : 0;

    TtsBridgeConfig cfg;
    cfg.endpoint = "http://127.0.0.1:" + std::to_string(port);
    cfg.engine = tts_bridge_engine_from_string(eng);
    TtsBridge b(cfg);

    std::string detail;
    const TtsBridgeHealth h = b.probe(&detail, wait_ms);
    std::cout << "probe: health=" << tts_bridge_health_name(h)
              << " detail='" << detail << "'" << std::endl;
    assert(b.available() == (h == TtsBridgeHealth::Ready));

    TtsBridgeControls c;
    c.instruction = "温柔亲切";
    c.speed = 1.1f;
    c.exaggeration = 0.8f;
    c.cfg_weight = 0.4f;
    c.lang = "zh";
    std::vector<int16_t> out;
    std::string err;
    const bool s = b.synthesize("你好，这是端到端测试", c, out, &err);
    std::cout << "synth: ok=" << s << " samples=" << out.size()
              << " err='" << err << "'" << std::endl;
    if (s) {
        assert(out.size() > 1000);
        int16_t peak = 0;
        for (int16_t v : out) { const int a = v < 0 ? -v : v; if (a > peak) peak = a; }
        std::cout << "peak amplitude: " << peak << std::endl;
        assert(peak > 100);
        std::cout << "E2E OK" << std::endl;
    }
    b.interrupt();
    return 0;
}
