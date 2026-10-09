// scripts/probe_zipvoice2.cpp —— 从 UTF-8 文件读参数，绕开 PowerShell 中文传参的编码坑
#include <sherpa-onnx/c-api/c-api.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace std::chrono;

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 读 <key>value</key> 形式
std::string field(const std::string& body, const std::string& key) {
    const std::string open = "<" + key + ">";
    const std::string close = "</" + key + ">";
    const size_t a = body.find(open);
    if (a == std::string::npos) return {};
    const size_t b = body.find(close, a + open.size());
    if (b == std::string::npos) return {};
    return body.substr(a + open.size(), b - a - open.size());
}

bool read_wav_pcm16(const std::string& path, std::vector<float>& out,
                    int& sample_rate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (all.size() < 44 || std::memcmp(all.data(), "RIFF", 4) != 0) return false;
    auto rd32 = [&](size_t o) {
        return static_cast<int>(static_cast<unsigned char>(all[o]) |
                                (static_cast<unsigned char>(all[o+1]) << 8) |
                                (static_cast<unsigned char>(all[o+2]) << 16) |
                                (static_cast<unsigned>(static_cast<unsigned char>(all[o+3])) << 24));
    };
    auto rd16 = [&](size_t o) {
        return static_cast<int>(static_cast<unsigned char>(all[o]) |
                                (static_cast<unsigned char>(all[o+1]) << 8));
    };
    int channels = 1, bits = 16;
    const char* data = nullptr;
    size_t data_size = 0, pos = 12;
    while (pos + 8 <= all.size()) {
        const int csize = rd32(pos + 4);
        const size_t body = pos + 8;
        if (csize < 0 || body + static_cast<size_t>(csize) > all.size()) break;
        if (std::memcmp(all.data() + pos, "fmt ", 4) == 0 && csize >= 16) {
            channels = rd16(body + 2);
            sample_rate = rd32(body + 4);
            bits = rd16(body + 14);
        } else if (std::memcmp(all.data() + pos, "data", 4) == 0) {
            data = all.data() + body;
            data_size = static_cast<size_t>(csize);
        }
        pos = body + static_cast<size_t>(csize) + (csize & 1);
    }
    if (!data || bits != 16 || channels < 1) return false;
    const size_t n = data_size / 2;
    out.resize(n / static_cast<size_t>(channels));
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char* s =
            reinterpret_cast<const unsigned char*>(data) + i * 2 * channels;
        out[i] = static_cast<float>(
            static_cast<int16_t>(static_cast<uint16_t>(s[0] | (s[1] << 8)))) /
            32768.0f;
    }
    return !out.empty();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: probe2 <model_dir> <vocoder> <case.txt>\n");
        return 2;
    }
    const std::string model_dir = argv[1];
    const std::string vocoder = argv[2];

    std::ifstream cf(argv[3], std::ios::binary);
    if (!cf) { std::fprintf(stderr, "cannot open case file\n"); return 1; }
    const std::string body((std::istreambuf_iterator<char>(cf)),
                           std::istreambuf_iterator<char>());
    const std::string ref_txt = trim(field(body, "reftxt"));
    const std::string text = trim(field(body, "text"));
    const std::string ref_name = trim(field(body, "ref"));
    std::printf("reftxt=%s\ntext=%s\n", ref_txt.c_str(), text.c_str());
    if (ref_txt.empty() || text.empty()) {
        std::fprintf(stderr, "FAIL: case file missing reftxt/text\n");
        return 1;
    }

    std::vector<float> ref;
    int ref_sr = 0;
    const std::string ref_path = model_dir + "/test_wavs/" + ref_name;
    if (!read_wav_pcm16(ref_path, ref, ref_sr)) {
        std::fprintf(stderr, "FAIL: cannot read ref %s\n", ref_path.c_str());
        return 1;
    }
    std::printf("ref: %s, %.2f s @ %d Hz\n", ref_name.c_str(),
                ref.size() / static_cast<double>(ref_sr), ref_sr);

    SherpaOnnxOfflineTtsConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.model.num_threads = 2;
    cfg.model.debug = 0;
    cfg.model.provider = "cpu";
    const std::string enc = model_dir + "/encoder.int8.onnx";
    const std::string dec = model_dir + "/decoder.int8.onnx";
    const std::string tok = model_dir + "/tokens.txt";
    const std::string lex = model_dir + "/lexicon.txt";
    const std::string edir = model_dir + "/espeak-ng-data";
    cfg.model.zipvoice.encoder = enc.c_str();
    cfg.model.zipvoice.decoder = dec.c_str();
    cfg.model.zipvoice.vocoder = vocoder.c_str();
    cfg.model.zipvoice.tokens = tok.c_str();
    cfg.model.zipvoice.lexicon = lex.c_str();
    cfg.model.zipvoice.data_dir = edir.c_str();
    cfg.model.zipvoice.feat_scale = 1.0f;
    cfg.model.zipvoice.t_shift = 0.4f;
    cfg.model.zipvoice.target_rms = 0.03f;
    cfg.model.zipvoice.guidance_scale = 1.5f;

    const SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&cfg);
    if (!tts) { std::fprintf(stderr, "FAIL: create returned null\n"); return 1; }

    // 不传 extra：让 min_char_in_sentence 用官方默认 30。
    // 之前传 2 是错的 —— 它是"把短句合并到这个长度"的下限，
    // 调小会让切分碎片化，反而更慢且音质更差。
    for (int steps : {4, 2, 6}) {
        SherpaOnnxGenerationConfig g;
        std::memset(&g, 0, sizeof(g));
        g.speed = 1.0f;
        g.silence_scale = 0.2f;
        g.num_steps = steps;
        g.reference_audio = ref.data();
        g.reference_audio_len = static_cast<int32_t>(ref.size());
        g.reference_sample_rate = ref_sr;
        g.reference_text = ref_txt.c_str();

        const auto t0 = steady_clock::now();
        const SherpaOnnxGeneratedAudio* a =
            SherpaOnnxOfflineTtsGenerateWithConfig(tts, text.c_str(), &g, nullptr, nullptr);
        const double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        if (!a || a->n <= 0) {
            std::printf("num_steps=%d -> FAILED\n", steps);
            if (a) SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
            continue;
        }
        float peak = 0.0f;
        for (int32_t i = 0; i < a->n; ++i) {
            const float p = std::fabs(a->samples[i]);
            if (p > peak) peak = p;
        }
        const double sec = a->n / static_cast<double>(a->sample_rate);
        std::printf("num_steps=%d -> %.0f ms, %d samples (%.2f s), RTF=%.3f, peak=%.3f%s\n",
                    steps, ms, a->n, sec, (ms / 1000.0) / sec, peak,
                    peak > 0.01f ? "" : "  [静音!]");
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
    }

    SherpaOnnxDestroyOfflineTts(tts);
    std::printf("PROBE2 DONE\n");
    return 0;
}