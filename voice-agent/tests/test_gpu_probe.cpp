// tests/test_gpu_probe.cpp
// GPU 探针：隔离验证 VAD/ASR/TTS 的 DirectML 加载与推理，定位卡死点。
// 用法: test_gpu_probe [asr_dir] [vad_model] [tts_dir] [provider]
#include "vad/vad.hpp"
#include "asr/asr.hpp"
#include "tts/tts.hpp"
#include "llm/llm.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace voice_agent;

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

// 读取 16k/16bit/mono int16 WAV（tests/audio/voice_sample.wav）
static bool load_wav16k(const std::string& path, std::vector<int16_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[44];
    if (std::fread(hdr, 1, 44, f) != 44) { std::fclose(f); return false; }
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fclose(f); return false;
    }
    int rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    unsigned short bits = hdr[34] | (hdr[35] << 8);
    unsigned short ch = hdr[22] | (hdr[23] << 8);
    if (rate != 16000 || bits != 16 || ch != 1) { std::fclose(f); return false; }
    uint8_t tmp[8192];
    while (true) {
        size_t n = std::fread(tmp, 1, sizeof(tmp), f);
        if (n == 0) break;
        for (size_t i = 0; i + 1 < n; i += 2)
            out.push_back((int16_t)(tmp[i] | (tmp[i + 1] << 8)));
    }
    std::fclose(f);
    return !out.empty();
}

int main(int argc, char** argv) {
    std::string asr_dir = argc > 1 ? argv[1] : "models/asr/paraformer-zh";
    std::string vad_model = argc > 2 ? argv[2] : "models/vad/silero_vad.onnx";
    std::string tts_dir = argc > 3 ? argv[3] : "models/tts/kokoro-multi-lang-v1_0";
    std::string provider = argc > 4 ? argv[4] : "dml";
    std::string wav_path = argc > 5 ? argv[5] : "tests/audio/voice_sample.wav";
    std::string llm_path = argc > 6 ? argv[6] : "models/llm/qwen2.5-1.5b-instruct-q4_k_m.gguf";
    // force=1 → 强制 TTS 走 DirectML（绕过 Intel 回退），验证当前版本是否还崩溃
    if (argc > 7 && std::string(argv[7]) == "force") {
        printf("[C] VA_FORCE_TTS_DML=1 (force DirectML for Kokoro)\n");
        fflush(stdout);
        _putenv_s("VA_FORCE_TTS_DML", "1");
    }

    std::vector<int16_t> wav;
    if (!load_wav16k(wav_path, wav)) {
        printf("FAIL: cannot load WAV %s (need 16k/16bit/mono)\n", wav_path.c_str());
        return 1;
    }
    printf("WAV loaded: %zu samples (%.1f s)\n", wav.size(),
           wav.size() / 16000.0);
    fflush(stdout);

    // Case A: ASR dml 加载 + 整段转写
    {
        auto t0 = std::chrono::steady_clock::now();
        printf("[A] ASR(%s) provider=dml loading...\n", asr_dir.c_str());
        fflush(stdout);
        ASR asr;
        ASRConfig ac;
        ac.model_path = asr_dir;
        ac.provider = provider;
        bool ok = asr.initialize(ac);
        printf("[A] load ok=%d real=%d in %.0f ms\n", ok, asr.uses_real_backend(),
               ms_since(t0));
        fflush(stdout);
        if (!ok) return 1;
        auto t1 = std::chrono::steady_clock::now();
        std::string text = asr.transcribe_segment(wav.data(), wav.size());
        printf("[A] transcribe = \"%s\" (%.0f ms)\n", text.c_str(), ms_since(t1));
        fflush(stdout);
    }

    // Case B: VAD 先加载并推理，再 ASR dml（复现应用初始化顺序）
    {
        auto t0 = std::chrono::steady_clock::now();
        printf("[B] VAD(%s) loading...\n", vad_model.c_str());
        fflush(stdout);
        VAD vad;
        VADConfig vcfg;
        vcfg.sample_rate = 16000;
        vcfg.speech_threshold = 0.5f;
        vad.initialize(vcfg);
        vad.set_model_path("", vad_model);
        printf("[B] VAD loaded in %.0f ms\n", ms_since(t0));
        fflush(stdout);
        auto t1 = std::chrono::steady_clock::now();
        bool speech = false;
        for (size_t i = 0; i + 512 <= wav.size(); i += 512)
            speech = vad.process(wav.data() + i, 512) || speech;
        printf("[B] VAD inference done speech=%d in %.0f ms\n", speech, ms_since(t1));
        fflush(stdout);
        printf("[B] ASR(%s) provider=dml after VAD loading...\n", asr_dir.c_str());
        fflush(stdout);
        ASR asr;
        ASRConfig ac;
        ac.model_path = asr_dir;
        ac.provider = provider;
        bool ok = asr.initialize(ac);
        printf("[B] load ok=%d real=%d in %.0f ms\n", ok, asr.uses_real_backend(),
               ms_since(t0));
        fflush(stdout);
        if (!ok) return 1;
        auto t2 = std::chrono::steady_clock::now();
        std::string text = asr.transcribe_segment(wav.data(), wav.size());
        printf("[B] transcribe = \"%s\" (%.0f ms)\n", text.c_str(), ms_since(t2));
        fflush(stdout);
    }

    // Case C: TTS(kokoro) dml 加载 + 中文合成
    {
        auto t0 = std::chrono::steady_clock::now();
        printf("[C] TTS(kokoro) provider=dml loading...\n");
        fflush(stdout);
        TTS tts;
        TTSConfig tc;
        tc.model_path = tts_dir + "/model.onnx";
        tc.voice_path = tts_dir + "/voices.bin";
        tc.tokens_path = tts_dir + "/tokens.txt";
        tc.data_dir = tts_dir + "/espeak-ng-data";
        tc.lexicon = tts_dir + "/lexicon-zh.txt";
        tc.speaker_id = 45;
        tc.provider = provider;
        bool ok = tts.initialize(tc);
        printf("[C] load ok=%d real=%d in %.0f ms\n", ok, tts.uses_real_backend(),
               ms_since(t0));
        fflush(stdout);
        if (!ok) return 1;
        auto t1 = std::chrono::steady_clock::now();
        std::vector<int16_t> audio = tts.synthesize("你好，世界，GPU 加速测试");
        printf("[C] synthesize ok=%zu samples (%.1f s) in %.0f ms\n", audio.size(),
               audio.size() / 24000.0, ms_since(t1));
        fflush(stdout);
    }

    // Case D: LLM Vulkan GPU 全量下放 + 后端标签
    {
        auto t0 = std::chrono::steady_clock::now();
        printf("[D] LLM(%s) n_gpu_layers=999 loading...\n", llm_path.c_str());
        fflush(stdout);
        LLM llm;
        LLMConfig lc;
        lc.model_path = llm_path;
        lc.n_gpu_layers = 999;
        lc.n_ctx = 2048;
        lc.n_threads = 4;
        lc.max_tokens = 256;
        bool ok = llm.initialize(lc);
        printf("[D] load ok=%d real=%d provider_label='%s' in %.0f ms\n",
               ok, llm.real_backend(), llm.provider_label().c_str(), ms_since(t0));
        fflush(stdout);
        if (!ok || !llm.real_backend()) return 1;
        auto t1 = std::chrono::steady_clock::now();
        std::string out = llm.generate("用一句话回答：1+1等于几？");
        printf("[D] generate[%zu c] in %.0f ms: %s\n", out.size(), ms_since(t1),
               out.empty() ? "(empty)" : out.substr(0, 120).c_str());
        fflush(stdout);
    }

    printf("ALL DONE\n");
    fflush(stdout);
    return 0;
}
