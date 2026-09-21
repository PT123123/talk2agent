// tests/test_sapi.cpp
// 简单 TTS（Windows 系统语音 SAPI）冒烟测试：初始化 → 朗读一段中文 → 不崩溃。
// 运行本程序会从扬声器出声（用于人工验证实时朗读），故不作为 ctest 自动测试。
#include "tts/sapi_speaker.hpp"
#include "util/log.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <iostream>

int main() {
    voice_agent::SapiSpeaker sp;
    if (!sp.initialize()) {
        std::cerr << "SapiSpeaker 初始化失败（检查系统语音）\n";
        return 1;
    }
    std::printf("SapiSpeaker 就绪，voice=%s\n", sp.voice_name().c_str());

    // 逐块喂入：模拟"出一个字念一个字"的流式体验
    const char* parts[] = {"大家好，", "这是一段", "流式语音", "朗读测试。"};
    for (const char* p : parts) {
        sp.speak(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    // 等播完（最多 ~8s）
    int waited = 0;
    while (sp.is_speaking() && waited < 80) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++waited;
    }

    sp.stop();
    LOG_INFO("test_sapi: finished, okay");
    return 0;
}