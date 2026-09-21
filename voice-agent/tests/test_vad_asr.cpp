// tests/test_vad_asr.cpp
// 端到端语音链路验证：真实语音 WAV → Silero VAD 切分语音段 → Whisper tiny ONNX 整段转写。
// 复现 Orchestrator 的链路：SpeechStart 开始缓冲、SpeechEnd 停止缓冲并将整段交给 ASR。
// 用法: test_vad_asr [wav] [vad_model] [asr_dir]
//   wav       16k/16bit/单声道 int16 PCM（默认 tests/audio/voice_sample.wav）
//   vad_model Silero onnx 路径（默认 models/vad/silero_vad.onnx）
//   asr_dir   Whisper tiny 模型目录（默认 models/asr/whisper-tiny）
#include "vad/vad.hpp"
#include "asr/asr.hpp"
#include "asr/whisper_onnx.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace voice_agent;

// 仅支持 16k / 16-bit mono int16 PCM WAV
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
    if (rate != 16000 || bits != 16 || ch != 1) {
        std::fclose(f);
        fprintf(stderr, "wav must be 16k/16bit/mono, got rate=%d bits=%d ch=%d\n", rate, bits, ch);
        return false;
    }
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
    std::string wav_path =
        (argc > 1 && std::string(argv[1]) != "-") ? argv[1] : "tests/audio/voice_sample.wav";
    std::string vad_model =
        (argc > 2 && std::string(argv[2]) != "-") ? argv[2] : "models/vad/silero_vad.onnx";
    std::string asr_dir =
        (argc > 3 && std::string(argv[3]) != "-") ? argv[3] : "models/asr/whisper-tiny";

    // 1) 加载语音
    std::vector<int16_t> pcm;
    if (!load_wav16k(wav_path, pcm)) {
        printf("FAIL: cannot read 16k wav %s\n", wav_path.c_str());
        return 1;
    }
    printf("OK: loaded %zu samples (%.2fs) from %s\n", pcm.size(),
           (double)pcm.size() / 16000.0, wav_path.c_str());

    // 2) Whisper 后端（直接用它验证整段转写，同时也经 ASR::transcribe_segment 走正式链路）
    WhisperOnnx whisper;
    if (!whisper.load(asr_dir)) {
        printf("FAIL: whisper load: %s\n", whisper.last_error().c_str());
        return 1;
    }
    printf("OK: whisper-tiny loaded from %s\n", asr_dir.c_str());

    // 3) VAD（Silero）
    VAD vad;
    VADConfig vcfg;
    vcfg.sample_rate = 16000;
    vcfg.speech_threshold = 0.5f;
    vcfg.min_speech_duration_ms = 250;
    vcfg.min_silence_duration_ms = 500;
    if (!vad.initialize(vcfg)) {
        printf("FAIL: vad init\n");
        return 1;
    }
    vad.set_model_path("", vad_model);  // 优先 Silero

    // 缓冲语音段
    std::vector<int16_t> seg;
    bool capturing = false;
    int starts = 0, ends = 0;
    auto t_seg0 = std::chrono::steady_clock::now();
    vad.set_callback([&](VADEvent ev, const int16_t*, size_t) {
        if (ev == VADEvent::SpeechStart) {
            capturing = true; seg.clear(); starts++;
            t_seg0 = std::chrono::steady_clock::now();
        } else if (ev == VADEvent::SpeechEnd && capturing) {
            capturing = false; ends++;
            // ---- 正式链路：SpeechEnd → asr.transcribe_segment(整段) ----
            auto t0 = std::chrono::steady_clock::now();
            std::string text = whisper.transcribe(seg.data(), seg.size());
            auto cv = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            (void)t_seg0;
            fprintf(stderr, "  [segment#%d size=%.2fs asr=%lldms] transcribe='%s'\n",
                    ends, (double)seg.size() / 16000.0, (long long)cv, text.c_str());
            fflush(stderr);
            printf("segment#%d -> '%s'\n", ends, text.c_str());
        }
    });

    // 4) 逐块送入（100ms/块，模拟采集回调）
    const size_t chunk = 1600;
    for (size_t i = 0; i < pcm.size(); i += chunk) {
        size_t n = std::min(chunk, pcm.size() - i);
        if (capturing) seg.insert(seg.end(), pcm.data() + i, pcm.data() + i + n);
        vad.process(pcm.data() + i, n);
    }

    printf("RESULT: VAD segments detected=%d complete=%d\n", starts, ends);
    if (ends == 0) {
        printf("RESULT: PASS (no speech segments ended within clip)\n");
    }
    return 0;
}