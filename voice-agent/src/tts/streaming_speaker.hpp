// src/tts/streaming_speaker.hpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace voice_agent {

class TTS;

// ========== 流式增量播报 ==========
// 把 LLM 流式输出的增量文本按句切分，交给独立合成线程逐句合成，并连播到音频输出，
// 实现"边生成边开口"，把首响应从"整段合成完再播"压缩到首句完成即可出声。
//
// 用法（任意线程安全）：
//   push_text(token) —— 向累积缓冲追加增量 token；跨 UTF-8 字符安全，到句子边界即切句入队
//   flush()          —— 流结束：把未到边界的残留文本也收进来一起播
//   stop()           —— 清空队列并停止播放
//   wait_done()      —— 阻塞直到所有已入队句子合成并播完（含 flush 尾部）
// 支持 tts_enabled=false 时"只收文本不播"（由调用方决定是否调用）。
class StreamingSpeaker {
public:
    explicit StreamingSpeaker(TTS* tts);
    ~StreamingSpeaker();

    StreamingSpeaker(const StreamingSpeaker&) = delete;
    StreamingSpeaker& operator=(const StreamingSpeaker&) = delete;

    void push_text(const std::string& chunk);
    void flush();
    void stop();
    void wait_done();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voice_agent