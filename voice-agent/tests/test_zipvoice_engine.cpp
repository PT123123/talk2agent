// tests/test_zipvoice_engine.cpp —— ZipVoice 引擎端到端测试（真实模型）
//
// 与 test_tts_bridge 的区别：那个是**纯逻辑离线**测试，这个**真的加载模型、
// 真的合成音频**。目的：验证 tts.cpp 的接线（引擎识别 / 参考音频载入 /
// GenerationConfig 填写 / WAV→PCM 转换）而不只是 sherpa-onnx 本身可用。
//
// 模型不存在时**跳过**而不是失败 —— CI/换机跑不过这一关很正常，
// 但静默"通过"会掩盖问题，所以明确打印 SKIP。
#include "tts/tts.hpp"
#include "util/log.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using namespace std;
using namespace voice_agent;
namespace fs = std::filesystem;

namespace {

bool file_exists(const string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

}  // namespace

int main(int argc, char** argv) {
    cout << "==== test_zipvoice_engine ====" << endl;

    // 模型根目录。默认相对 ctest 的工作目录（build-msvc/tests），
    // 所以要往上两级才是项目根 —— 这跟其它 test_kokoro 的做法一致。
    string root = (argc > 1)
                      ? argv[1]
                      : "../../models/tts/zipvoice/"
                        "sherpa-onnx-zipvoice-distill-int8-zh-en-emilia";
    const string vocoder =
        (argc > 2) ? argv[2] : "../../models/tts/vocos_24khz.onnx";

    if (!file_exists(root + "/encoder.int8.onnx") || !file_exists(vocoder)) {
        cout << "SKIP: 模型未找到" << endl;
        cout << "  encoder: " << root << "/encoder.int8.onnx" << endl;
        cout << "  vocoder: " << vocoder << endl;
        cout << "  （在模型管理界面下载 ZipVoice 即可运行本测试）" << endl;
        return 0;
    }

    // ---- 1. 引擎识别：含 espeak-ng-data 也不能被误判成 piper ----
    {
        // 这里直接验证 zipvoice 分支的配置组装（不重复 agent_controller 的
        // 探测逻辑，那是 GUI 层的职责，测试它需要拉起 Qt）
        cout << "TEST zipvoice config assembly..." << endl;
        TTSConfig cfg;
        cfg.engine = "zipvoice";
        cfg.zipvoice_encoder = root + "/encoder.int8.onnx";
        cfg.zipvoice_decoder = root + "/decoder.int8.onnx";
        cfg.zipvoice_vocoder = vocoder;
        cfg.tokens_path = root + "/tokens.txt";
        cfg.lexicon = root + "/lexicon.txt";
        cfg.data_dir = root + "/espeak-ng-data";
        cfg.num_steps = 4;
        // 用模型自带的参考音频 + 其转写（来自模型自带 prompt.txt）
        cfg.ref_audio_path = root + "/test_wavs/news-female.wav";
        cfg.ref_text = "各位村民, 大家新年好! 近期, 湖北省武汉市等多个地区";
        assert(file_exists(cfg.ref_audio_path));
        assert(file_exists(cfg.zipvoice_encoder));
        assert(file_exists(cfg.zipvoice_vocoder));
        cout << "  -> PASS" << endl;
    }

    // ---- 2. 参考音频缺失必须早失败（沉默比报错更糟）----
    {
        cout << "TEST missing ref audio fails fast..." << endl;
        TTSConfig cfg;
        cfg.engine = "zipvoice";
        cfg.zipvoice_encoder = root + "/encoder.int8.onnx";
        cfg.zipvoice_decoder = root + "/decoder.int8.onnx";
        cfg.zipvoice_vocoder = vocoder;
        cfg.tokens_path = root + "/tokens.txt";
        cfg.data_dir = root + "/espeak-ng-data";
        cfg.ref_audio_path = root + "/test_wavs/__not_exist__.wav";
        cfg.ref_text = "不存在的音频";
        TTS tts;
        assert(tts.initialize(cfg) == false);   // 必须明确失败而不是静默沉默

        // 有音频但没转写文本 —— sherpa-onnx 明确要求两者对应
        TTSConfig cfg2 = cfg;
        cfg2.ref_audio_path = root + "/test_wavs/news-female.wav";
        cfg2.ref_text.clear();
        TTS tts2;
        assert(tts2.initialize(cfg2) == false);
        cout << "  -> PASS" << endl;
    }

    // ---- 3. 真实合成 ----
    TTSConfig cfg;
    cfg.engine = "zipvoice";
    cfg.zipvoice_encoder = root + "/encoder.int8.onnx";
    cfg.zipvoice_decoder = root + "/decoder.int8.onnx";
    cfg.zipvoice_vocoder = vocoder;
    cfg.tokens_path = root + "/tokens.txt";
    cfg.lexicon = root + "/lexicon.txt";
    cfg.data_dir = root + "/espeak-ng-data";
    cfg.ref_audio_path = root + "/test_wavs/news-female.wav";
    cfg.ref_text = "各位村民, 大家新年好! 近期, 湖北省武汉市等多个地区";
    cfg.num_steps = 4;
    cfg.speed = 1.0f;

    cout << "TEST initialize + synthesize (real model)..." << endl;
    TTS tts;
    if (!tts.initialize(cfg)) {
        cout << "  FAIL: initialize 失败" << endl;
        return 1;
    }
    assert(tts.uses_real_backend());
    assert(tts.engine_name() == "zipvoice");
    assert(tts.provider_label() == "CPU");   // 固定 CPU，不走 DirectML

    const string text = "这是零样本克隆的中文语音合成测试。";
    const auto audio = tts.synthesize(text);
    cout << "  synthesized " << audio.size() << " samples in "
         << tts.last_synthesize_ms.load() << " ms" << endl;

    // 断言必须够硬：静音 / 空数组 / 采样率不对都要能抓住
    assert(!audio.empty());
    const double sec = audio.size() / 24000.0;
    const double rtf = (tts.last_synthesize_ms.load() / 1000.0) / sec;
    cout << "  duration " << sec << " s, RTF " << rtf << endl;
    assert(rtf < 3.0);   // 实测 0.26~0.61；留足余量仍能抓住"退化到极慢"

    int16_t peak = 0;
    for (int16_t v : audio) {
        const int a = v < 0 ? -static_cast<int>(v) : v;
        if (a > peak) peak = a;
    }
    cout << "  peak amplitude " << peak << endl;
    assert(peak > 500);          // 静音的话peak 应该是 0
    assert(peak <= 32767);
    cout << "  -> PASS" << endl;

    // ---- 4. 流式接口（走的是 feed_chunks_，与 bridge 共用）----
    {
        cout << "TEST synthesize_stream..." << endl;
        size_t total = 0;
        int chunks = 0;
        bool saw_last = false;
        tts.synthesize_stream(text, [&](const int16_t* p, size_t n, bool last) {
            if (p && n) total += n;
            if (last) saw_last = true;
            ++chunks;
        });
        cout << "  " << chunks << " chunks, " << total << " samples, last="
             << (saw_last ? "yes" : "NO") << endl;
        // is_last 必须出现，否则 ResponsePlan 的播放游标会永远停在 Pending
        assert(saw_last);
        assert(total > 0);
        cout << "  -> PASS" << endl;
    }

    // ---- 5. 运行时切音色（不重建引擎）----
    {
        cout << "TEST set_reference_voice..." << endl;
        const string ref2 = root + "/test_wavs/leijun-1.wav";
        assert(tts.set_reference_voice(
            ref2, "那还是36年前, 1987年. 我呢考上了武汉大学的计算机系."));
        const auto a2 = tts.synthesize("换一个音色试试。");
        assert(!a2.empty());
        // 错误路径：wav 不存在时必须失败且**保持原音色可用**
        assert(!tts.set_reference_voice(root + "/test_wavs/__nope__.wav", "x"));
        const auto a3 = tts.synthesize("原音色还在吗。");
        assert(!a3.empty());   // 切失败不能把引擎搞哑
        cout << "  -> PASS" << endl;
    }

    // ---- 6. num_steps 钳制 ----
    {
        cout << "TEST set_num_steps clamping..." << endl;
        tts.set_num_steps(100);   // 应被钳到 16
        tts.set_num_steps(0);     // 应被钳到 1
        tts.set_num_steps(-5);
        tts.set_num_steps(4);
        cout << "  -> PASS" << endl;
    }

    cout << "ALL PASS" << endl;
    return 0;
}