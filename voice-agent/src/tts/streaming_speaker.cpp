// src/tts/streaming_speaker.cpp
#include "tts/streaming_speaker.hpp"
#include "tts/tts.hpp"
#include "audio/audio_device.hpp"
#include "util/log.hpp"

#include <cstring>
#include <cctype>

namespace voice_agent {

namespace {

// 判断一个多字节 UTF-8 序列是否完整（字节流在 token 间可能被切断，尾部保留待拼）。
// 返回需要"保留在累积缓冲"的最小字节数；0 表示当前整段都是完整字符。
size_t utf8_hold_back(const std::string& s) {
    if (s.empty()) return 0;
    size_t i = s.size();
    int cont = 0;
    size_t need = 0;
    for (; i > 0; --i) {
        unsigned char c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0xC0) == 0x80) { ++cont; continue; }
        if ((c & 0x80) == 0) return 0;            // 尾部是 ASCII，全完整
        if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else need = 1;
        break;
    }
    // 距前导字节长度
    size_t have = s.size() - (i - 1);
    return (have >= need) ? 0 : need - have;
}

// 句内是否含实际内容（排除空白/纯标点片段）
bool has_speech(const std::string& s) {
    for (unsigned char c : s) {
        if (c > 0x7F) return true;
        if (std::isspace(c)) continue;
        if (std::strchr("，。！？；：、,.!?;:()[]{}《》“”‘’\"'-\n", c) == nullptr)
            return true;
    }
    return false;
}

// 找句子边界（。！？…、换行）；返回位置（含标点），找不到返回 npos。
size_t find_bound(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '。' || c == '！' || c == '？' || c == '；' ||
            c == '!' || c == '?' || c == ';' || c == '\n' || c == '…')
            return i;
    }
    return std::string::npos;
}

}  // namespace

// ========== 实现 ==========
struct StreamingSpeaker::Impl {
    TTS* tts;

    std::mutex queue_mtx;
    std::condition_variable queue_cv;
    std::deque<std::string> pending;   // 待合成的完整句子
    std::string buffer_;               // 跨 token 累积的句内文本
    bool stopped_{false};              // stop() 已调用，合成线程应退出
    int inflight_{0};                  // 正在合成的句子数（含已弹出尚未入队音频的）

    // 合成线程
    std::thread worker;

    // 播放侧
    std::mutex play_mtx;
    std::vector<int16_t> samples_;     // 已合成待播采样
    std::atomic<size_t> cursor_{0};
    std::atomic<bool> playing_{false};
    std::shared_ptr<AudioDevice> out_;
    bool device_started_{false};

    explicit Impl(TTS* t) : tts(t) {}

    // 播放回调：把 samples_ 从 cursor_ 起填满输出；不足补静音
    size_t onPlayback(int16_t* output, size_t frames) {
        if (!playing_.load()) {
            std::memset(output, 0, frames * sizeof(int16_t));
            return frames;
        }
        std::lock_guard<std::mutex> lk(play_mtx);
        size_t pos = cursor_.load();
        size_t w = 0;
        while (w < frames && pos < samples_.size()) output[w++] = samples_[pos++];
        for (; w < frames; ++w) output[w] = 0;
        cursor_.store(pos);
        return frames;
    }

    // 把一段已合成音频追加到播放缓冲；必要时惰性打开/启动输出设备
    void enqueue_audio(std::vector<int16_t> audio) {
        if (audio.empty()) return;
        {
            std::lock_guard<std::mutex> lk(play_mtx);
            samples_.insert(samples_.end(), audio.begin(), audio.end());
        }
        if (!device_started_) {
            device_started_ = true;
            AudioConfig cfg;
            cfg.sample_rate = tts->config().sample_rate ? tts->config().sample_rate : 24000;
            cfg.channels = 1;
            cfg.frames_per_buffer = 480;   // 20ms @24kHz
            out_ = std::make_shared<AudioDevice>();
            if (out_->init_as_output(cfg)) {
                out_->set_playback_callback(
                    [this](int16_t* o, size_t f) { return onPlayback(o, f); });
                cursor_.store(0);
                playing_.store(true);
                if (!out_->start()) playing_.store(false);
            } else {
                out_.reset();
                device_started_ = true;  // 设备打不开则不再重试
                playing_.store(false);
            }
        }
    }

    void stop_playback() {
        playing_.store(false);
        if (out_) {
            out_->stop();
            out_.reset();
        }
        {
            std::lock_guard<std::mutex> lk(play_mtx);
            samples_.clear();
        }
        cursor_.store(0);
        device_started_ = false;   // 释放设备，下轮首个音频到达时需重新开播
    }

    // 合成线程主循环：弹出句子 → 整句合成 → 追加播放
    void run() {
        for (;;) {
            std::string sentence;
            {
                std::unique_lock<std::mutex> lk(queue_mtx);
                queue_cv.wait(lk, [this] { return stopped_ || !pending.empty(); });
                if (stopped_ && pending.empty()) break;
                sentence = std::move(pending.front());
                pending.pop_front();
                ++inflight_;
            }
            if (sentence.empty()) {
                --inflight_;
                continue;
            }
            std::vector<int16_t> audio;
            if (tts) audio = tts->synthesize(sentence);
            if (!audio.empty()) enqueue_audio(std::move(audio));
            {
                std::lock_guard<std::mutex> lk(queue_mtx);
                --inflight_;
            }
            queue_cv.notify_all();   // wait_done 可能正等 inflight_ 清零
        }
    }

    // 从给定文本中切出所有完整句子（静态辅助）
    static void split_sentences(const std::string& text, std::vector<std::string>& out) {
        size_t b = 0;
        size_t pos;
        while (b < text.size() && (pos = find_bound(text.substr(b))) != std::string::npos) {
            std::string sent = text.substr(b, pos + 1);
            b += pos + 1;
            out.push_back(sent);
        }
    }
};

StreamingSpeaker::StreamingSpeaker(TTS* tts) : impl_(std::make_unique<Impl>(tts)) {
    impl_->worker = std::thread([this] { impl_->run(); });
}

StreamingSpeaker::~StreamingSpeaker() {
    stop();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void StreamingSpeaker::push_text(const std::string& chunk) {
    if (chunk.empty()) return;

    // 简单引擎（系统语音）：文本一到就直接朗读，实现"出一个字念一个字"的实时流式。
    // 仅做 UTF-8 完整字节保护，不做句子缓冲（系统语音无整句合成延迟）。
    if (impl_->tts && impl_->tts->simple_engine()) {
        std::string ready;
        {
            std::lock_guard<std::mutex> lk(impl_->queue_mtx);
            impl_->buffer_ += chunk;
            size_t hold = utf8_hold_back(impl_->buffer_);
            if (hold == impl_->buffer_.size()) return;   // 尾部仍是未完成字符，等下一块
            ready = impl_->buffer_.substr(0, impl_->buffer_.size() - hold);
            impl_->buffer_ = impl_->buffer_.substr(impl_->buffer_.size() - hold);
        }
        if (!ready.empty()) impl_->tts->synthesize(ready);   // 即时，无合成延迟，直接入 SAPI 队列
        return;
    }

    static constexpr size_t kMaxChunk = 40;   // 无标点时最长攒多少字符就强制切，防止等你等到天荒地老
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mtx);
        impl_->buffer_ += chunk;
        // 保留尾部未完整 UTF-8 字节，其余参与切句
        size_t hold = utf8_hold_back(impl_->buffer_);
        if (hold == impl_->buffer_.size()) return;   // 全是未完成字符，等下一块
        std::string ready = impl_->buffer_.substr(0, impl_->buffer_.size() - hold);
        impl_->buffer_ = impl_->buffer_.substr(impl_->buffer_.size() - hold);
        // 无句法边界时：按字符数兜底强制切分（优先在 ASCII 空白处断开）
        while (ready.size() > kMaxChunk && find_bound(ready) == std::string::npos) {
            size_t cut = kMaxChunk;
            size_t sp = ready.find_last_of(" \t\n", kMaxChunk);
            if (sp != std::string::npos && sp > 0) cut = sp + 1;
            std::string piece = ready.substr(0, cut);
            if (has_speech(piece)) impl_->pending.push_back(piece);
            ready.erase(0, cut);
        }
        std::vector<std::string> sents;
        Impl::split_sentences(ready, sents);
        for (auto& s : sents)
            if (has_speech(s)) impl_->pending.push_back(std::move(s));
    }
    impl_->queue_cv.notify_one();
}

void StreamingSpeaker::flush() {
    // 简单引擎：把残余（可能未到分出条件的完整字符）也交给系统语音，随后无需内存播放
    if (impl_->tts && impl_->tts->simple_engine()) {
        std::string tail;
        {
            std::lock_guard<std::mutex> lk(impl_->queue_mtx);
            tail = std::move(impl_->buffer_);
        }
        if (!tail.empty()) impl_->tts->synthesize(tail);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mtx);
        if (!impl_->buffer_.empty() && has_speech(impl_->buffer_))
            impl_->pending.push_back(std::move(impl_->buffer_));
        impl_->buffer_.clear();
    }
    impl_->queue_cv.notify_all();
}

void StreamingSpeaker::stop() {
    impl_->stop_playback();
    if (impl_->tts && impl_->tts->simple_engine()) impl_->tts->stop();   // 同时清空系统语音队列
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mtx);
        impl_->pending.clear();
        impl_->stopped_ = true;
    }
    impl_->queue_cv.notify_all();
}

void StreamingSpeaker::wait_done() {
    // 等待合成线程把队列全部合成入队（含正在合成长句的 in-flight）
    {
        std::unique_lock<std::mutex> lk(impl_->queue_mtx);
        impl_->queue_cv.wait(lk, [this] { return impl_->inflight_ == 0 && impl_->pending.empty(); });
    }
    // 等待播放缓冲排空
    for (;;) {
        size_t n;
        {
            std::lock_guard<std::mutex> lk(impl_->play_mtx);
            n = impl_->samples_.size() - impl_->cursor_.load();
        }
        if (n == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // 尾音
    impl_->stop_playback();
}

}  // namespace voice_agent