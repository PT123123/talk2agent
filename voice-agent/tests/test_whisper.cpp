// tests/test_whisper.cpp
// 冒烟测试：加载真实 whisper-tiny ONNX 模型并跑一次完整 encoder+decoder 推理。
// 工作目录需为项目根（models/asr/whisper-tiny 相对路径）。可选参数：一个 16k 单声道 int16 WAV 做真实转写。
#include "asr/whisper_onnx.hpp"
#include "util/log.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

using namespace voice_agent;

static bool load_wav(const std::string& path, std::vector<int16_t>& out, int& rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[44];
    if (std::fread(hdr, 1, 44, f) != 44) { std::fclose(f); return false; }
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fclose(f); return false;
    }
    rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    unsigned short bits = hdr[34] | (hdr[35] << 8);
    unsigned short ch = hdr[22] | (hdr[23] << 8);
    uint8_t tmp[4096];
    while (true) {
        size_t n = std::fread(tmp, 1, sizeof(tmp), f);
        if (n == 0) break;
        for (size_t i = 0; i + 1 < n; i += 2) {
            int16_t s = (int16_t)(tmp[i] | (tmp[i + 1] << 8));
            if (ch == 1) out.push_back(s);
            else if (i + 3 < n) { int32_t l = tmp[i] | (tmp[i+1] << 8); int32_t r = tmp[i+2] | (tmp[i+3] << 8); out.push_back((int16_t)((l + r) / 2)); i += 2; }
        }
    }
    std::fclose(f);
    return !out.empty();
}

int main(int argc, char** argv) {
    const std::string dir = (argc > 1 && std::string(argv[1]) != "-")
                                ? argv[1] : std::string("models/asr/whisper-tiny");

    WhisperOnnx asr;
    if (!asr.load(dir)) {
        printf("FAIL: model load failed: %s\n", asr.last_error().c_str());
        return 1;
    }
    printf("OK: whisper model loaded from %s\n", dir.c_str());

    std::vector<int16_t> pcm;
    int rate = 16000;
    if (argc > 2) {
        if (!load_wav(argv[2], pcm, rate)) {
            printf("FAIL: cannot read wav %s\n", argv[2]);
            return 1;
        }
        printf("loaded wav: %zu frames @ %d Hz\n", pcm.size(), rate);
        if (rate != 16000) {
            printf("SKIP: wav not 16k (got %d), skipping real transcribe\n", rate);
            return 0;
        }
    } else {
        // 合成 2s 440Hz 音频，验证推理运行不崩溃（合成音无语义，结果可能为空）
        constexpr int sr = 16000;
        pcm.resize((size_t)(sr * 2));
        const float pi = 3.14159265f;
        for (int i = 0; i < sr * 2; ++i) {
            float env = 0.5f + 0.5f * std::sin(2.0f * pi * 2.0f * i / sr);  // 慢包络
            pcm[(size_t)i] = (int16_t)(3000 * env * std::sin(2.0f * pi * 440.0f * i / sr));
        }
        printf("synthesized 2s tone for smoke-run\n");
    }

    auto t0 = std::chrono::steady_clock::now();
    std::string text = asr.transcribe(pcm.data(), pcm.size());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "inference done in %lld ms, text(%zu)='%s'\n",
           (long long)ms, text.size(), text.c_str());
    fflush(stderr);

    // 断言：模型加载 + 一次完整推理通过。合成音无语义故不要求非空。
    printf("RESULT: PASS\n");
    return 0;
}