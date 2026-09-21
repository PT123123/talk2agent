// tests/test_probe.cpp — 临时隔离 probe：仅调用 validate_tool_args_json
#include "agent/grammar.hpp"
#include <iostream>
#include <string>

int main() {
    using namespace voice_agent;
    try {
    std::cout << "probe start" << std::endl;
    std::string r = validate_tool_args_json(R"({"query":"x"})", "web_search");
    std::cout << "r1='" << r << "' empty=" << r.empty() << std::endl;
    r = validate_tool_args_json(R"({"query":123})", "web_search");
    std::cout << "r2='" << r << "'" << std::endl;
    r = validate_tool_args_json(R"({})", "web_search");
    std::cout << "r3='" << r << "'" << std::endl;
    r = validate_tool_args_json("not-json", "web_search");
    std::cout << "r4='" << r << "'" << std::endl;
    r = validate_tool_args_json("{}", "get_time");
    std::cout << "r5='" << r << "'" << std::endl;
    std::cout << "probe done" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "CAUGHT std::exception: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "CAUGHT ..." << std::endl;
    }
    return 0;
}