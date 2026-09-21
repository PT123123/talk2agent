// src/tts/sapi_speaker.hpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace voice_agent {

// ========== 系统语音朗读（Windows SAPI） ==========
// 零模型依赖的最简 TTS：调用 Windows 自带语音，把 UTF-8 文本按到达顺序实时念出。
// 设计目标：去掉开源 TTS（Kokoro/sherpa-onnx）的模型加载与整句合成延迟——
// SAPI 在收到文本后几乎立刻出声，天然支持"喂一块念一块"的流式朗读。
//
// 线程模型：ISpVoice 及 COM 只在内部工作线程持有/使用（线程隔离，规避跨线程 COM 风险）。
// speak()/stop() 线程安全：只向命令队列追加，由工作线程串行交给 SAPI。
class SapiSpeaker {
public:
    SapiSpeaker();
    ~SapiSpeaker();

    SapiSpeaker(const SapiSpeaker&) = delete;
    SapiSpeaker& operator=(const SapiSpeaker&) = delete;

    // 创建 SAPI 语音（优先中文语音）。返回是否就绪。
    bool initialize();

    bool available() const { return ok_.load(); }

    // 实时朗读一段 UTF-8 文本。立即返回，朗读由 SAPI 异步进行，并按调用顺序连读。
    void speak(const std::string& utf8_text);

    // 中断当前朗读并清空 SAPI 队列。
    void stop();

    // 是否正在朗读（队列非空或 SAPI 仍有活动读本）。
    bool is_speaking() const;

    // 当队列清空且 SAPI 播完时，在工作线程回调一次（用于驱动"播报结束"状态）。
    void set_done_callback(std::function<void()> cb);

    std::string voice_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> ok_{false};
};

}  // namespace voice_agent