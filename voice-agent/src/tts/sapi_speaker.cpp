// src/tts/sapi_speaker.cpp
#define WIN32_LEAN_AND_MEAN
#include "tts/sapi_speaker.hpp"
#include "util/log.hpp"

#include <windows.h>
#include <sapi.h>

#include <cstring>
#include <thread>

namespace voice_agent {

namespace {

// UTF-8 → UTF-16（Windows 需要宽字符）。失败返回空。
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

// 获取语音 token 的可读名称（无默认名则留空，调用方回退"系统语音"）
void token_name(ISpObjectToken* tok, std::wstring& out) {
    if (!tok) return;
    WCHAR* s = nullptr;
    if (SUCCEEDED(tok->GetStringValue(L"", &s)) && s && s[0]) {
        out = s;
        CoTaskMemFree(s);
    }
}

// 枚举已安装语音，挑选简体中文（语言 0x804）。选到返回 token（已 AddRef）。
// 未选到返回 nullptr —— 调用方随后回退系统默认语音。
ISpObjectToken* pick_chinese_voice() {
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&cat))))
        return nullptr;
    if (FAILED(cat->SetId(SPCAT_VOICES, FALSE))) { cat->Release(); return nullptr; }

    IEnumSpObjectTokens* tokEnum = nullptr;
    if (FAILED(cat->EnumTokens(nullptr, nullptr, &tokEnum))) { cat->Release(); return nullptr; }

    ISpObjectToken* chosen = nullptr;
    ISpObjectToken* tok = nullptr;
    ULONG fetched = 0;
    while (tokEnum->Next(1, &tok, &fetched) == S_OK) {
        ISpDataKey* attrKey = nullptr;
        if (SUCCEEDED(tok->OpenKey(L"Attributes", &attrKey))) {
            DWORD lang = 0;
            if (SUCCEEDED(attrKey->GetDWORD(L"Language", &lang)) &&
                (lang & 0xFFFF) == 0x804) {
                chosen = tok;
                tok = nullptr;
            }
            attrKey->Release();
        }
        if (tok) { tok->Release(); tok = nullptr; }
        if (chosen) break;
    }
    if (tokEnum) tokEnum->Release();
    if (cat) cat->Release();
    return chosen;
}

}  // namespace

enum class CmdType { Speak, Purge };
struct Cmd {
    CmdType type;
    std::wstring text;
};

struct SapiSpeaker::Impl {
    std::atomic<bool> ok{false};

    std::mutex qmtx;
    std::condition_variable qcv;
    std::deque<Cmd> queue;
    bool quit{false};

    std::thread worker;

    std::mutex mtx;
    std::function<void()> done_cb;
    std::atomic<bool> speaking{false};
    std::wstring wide_voice_name;

    // 工作线程：持有 COM + ISpVoice，串行执行命令
    void thread_main() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        ISpVoice* voice = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ISpVoice, reinterpret_cast<void**>(&voice)))) {
            LOG_ERROR("SapiSpeaker: CoCreateInstance(SpVoice) 失败");
            CoUninitialize();
            return;
        }

        // 尽量切换中文语音，否则用系统默认
        ISpObjectToken* zh = pick_chinese_voice();
        if (zh) {
            if (SUCCEEDED(voice->SetVoice(zh))) {
                std::wstring d;
                token_name(zh, d);
                if (!d.empty()) {
                    std::lock_guard<std::mutex> lk(mtx);
                    wide_voice_name = std::move(d);
                }
            }
            zh->Release();
            zh = nullptr;
        }
        if (wide_voice_name.empty()) {
            ISpObjectToken* cur = nullptr;
            if (SUCCEEDED(voice->GetVoice(&cur)) && cur) {
                std::wstring d;
                token_name(cur, d);
                if (!d.empty()) {
                    std::lock_guard<std::mutex> lk(mtx);
                    wide_voice_name = std::move(d);
                }
                cur->Release();
            }
        }

        ok.store(true);

        for (;;) {
            Cmd cmd;
            {
                std::unique_lock<std::mutex> lk(qmtx);
                qcv.wait(lk, [this] { return quit || !queue.empty(); });
                if (quit && queue.empty()) break;
                cmd = std::move(queue.front());
                queue.pop_front();
            }

            if (cmd.type == CmdType::Purge) {
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                speaking.store(false);
            } else if (!cmd.text.empty()) {
                speaking.store(true);
                voice->Speak(cmd.text.c_str(), SPF_ASYNC, nullptr);
            }

            // 队列清空后：等待 SAPI 把已入队的异步读本播完，再回调"播报结束"
            bool drained = false;
            for (;;) {
                bool empty_q = false;
                {
                    std::lock_guard<std::mutex> g(qmtx);
                    empty_q = queue.empty();
                }
                if (!empty_q) { drained = false; break; }
                if (!speaking.load()) { drained = true; break; }
                if (voice->WaitUntilDone(50) == S_OK) {
                    speaking.store(false);
                    drained = true;
                    break;
                }
            }
            if (drained) {
                std::function<void()> cb;
                {
                    std::lock_guard<std::mutex> g(mtx);
                    cb = done_cb;
                }
                if (cb) cb();
            }
        }

        voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        voice->Release();
        CoUninitialize();
    }
};

SapiSpeaker::SapiSpeaker() : impl_(std::make_unique<Impl>()) {}
SapiSpeaker::~SapiSpeaker() {
    if (impl_->worker.joinable()) {
        {
            std::lock_guard<std::mutex> lk(impl_->qmtx);
            impl_->quit = true;
        }
        impl_->qcv.notify_all();
        impl_->worker.join();
    }
}

bool SapiSpeaker::initialize() {
    impl_->worker = std::thread([this] { impl_->thread_main(); });
    for (int i = 0; i < 200 && !impl_->ok.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return impl_->ok.load();
}

void SapiSpeaker::speak(const std::string& utf8_text) {
    if (utf8_text.empty() || !impl_->ok.load()) return;
    std::wstring w = utf8_to_wide(utf8_text);
    if (w.empty()) return;
    {
        std::lock_guard<std::mutex> lk(impl_->qmtx);
        impl_->queue.push_back(Cmd{CmdType::Speak, std::move(w)});
    }
    impl_->qcv.notify_one();
}

void SapiSpeaker::stop() {
    {
        std::lock_guard<std::mutex> lk(impl_->qmtx);
        impl_->queue.push_back(Cmd{CmdType::Purge, {}});
    }
    impl_->qcv.notify_one();
}

bool SapiSpeaker::is_speaking() const {
    return impl_->speaking.load();
}

void SapiSpeaker::set_done_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->done_cb = std::move(cb);
}

std::string SapiSpeaker::voice_name() const {
    std::wstring w;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        w = impl_->wide_voice_name;
    }
    if (w.empty()) return "系统语音";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "系统语音";
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n,
                        nullptr, nullptr);
    return out;
}

}  // namespace voice_agent
