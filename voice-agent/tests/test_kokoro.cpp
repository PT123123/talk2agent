// tests/test_kokoro.cpp
// 冒烟测试：加载真实 Kokoro 多语言 ONNX 模型并合成一段中文语音，
// 验证 sherpa-onnx C API 集成正确（引擎创建 + 生成 + 输出采样率/时长合理）。
// 工作目录需为项目根（models/tts/kokoro-multi-lang-v1_0 相对路径）。
#include "tts/tts.hpp"
#include "util/log.hpp"
#include <chrono>
#include <cstdio>
#include <string>

using namespace voice_agent;

int main(int argc, char** argv) {
    const std::string dir = (argc > 1 && std::string(argv[1]) != "-")
                                ? argv[1]
                                : std::string("models/tts/kokoro-multi-lang-v1_0");

    TTSConfig tc;
    tc.model_path = dir + "/model.onnx";
    tc.voice_path = dir + "/voices.bin";
    tc.tokens_path = dir + "/tokens.txt";
    tc.data_dir = dir + "/espeak-ng-data";
    tc.lexicon = dir + "/lexicon-zh.txt";
    tc.speaker_id = 45;  // zf_xiaobei 中文女声
    tc.lang = "zh";

    TTS tts;
    if (!tts.initialize(tc)) {
        printf("FAIL: TTS initialize failed\n");
        return 1;
    }
    printf("OK: Kokoro engine ready (real backend: %s)\n",
           tts.uses_real_backend() ? "yes" : "no");
    if (!tts.uses_real_backend()) {
        printf("FAIL: expected real backend\n");
        return 1;
    }

    const std::string text = "你好，我是本地语音助手，很高兴为你服务。";
    auto audio = tts.synthesize(text);
    if (audio.empty()) {
        printf("FAIL: synthesis returned empty audio\n");
        return 1;
    }

    const double secs = static_cast<double>(audio.size()) / tc.sample_rate;
    printf("OK: synthesized %zu samples (%.2f s @ %d Hz), last_synthesize_ms=%.1f\n",
           audio.size(), secs, tc.sample_rate, tts.last_synthesize_ms.load());
    if (secs < 0.5) {
        printf("FAIL: audio too short (%.2f s)\n", secs);
        return 1;
    }
    // 抽样检查非静音（应有语音能量）
    int nonzero = 0;
    for (size_t i = 0; i < audio.size(); i += 480) {
        if (audio[i] > 200 || audio[i] < -200) ++nonzero;
    }
    printf("INFO: %d/%zu sampled frames have speech energy\n", nonzero,
           audio.size() / 480 + 1);

    printf("All Kokoro TTS tests PASSED\n");
    return 0;
}
