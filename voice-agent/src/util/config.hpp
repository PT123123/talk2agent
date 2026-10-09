// src/util/config.hpp
#pragma once
#include "../core/types.hpp"
#include "log.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ========== 轻量 YAML 解析 ==========
// 配置文件是 agent.yaml（YAML），而 nlohmann::json 只能读 JSON。
// 这里提供一个针对本项目 YAML 子集的极简解析器，仅支持：
//   - 注释（# 开头）
//   - 顶层 key: value 与两级分组（key: 后换行，子项缩进两个空格）
//   - 标量：数字 / 布尔 / 字符串（含 URL 等含冒号的值，取第一个 ': ' 前为键）
namespace yaml_light {

// 去掉行首/行尾空白
inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// 统计行首都缩进空格数
inline size_t indent_of(const std::string& s) {
    size_t n = 0;
    while (n < s.size() && s[n] == ' ') ++n;
    return n;
}

// 把去除注释后的行去掉行尾注释
inline std::string strip_comment(const std::string& s) {
    std::string t = s;
    size_t pos = t.find('#');
    if (pos != std::string::npos) t = t.substr(0, pos);
    return t;
}

// 把 YAML 标量转成 nlohmann::json 值
inline json to_value(const std::string& raw) {
    std::string v = trim(raw);
    if (v.empty()) return json(nullptr);
    // 去掉可选引号
    if (v.size() >= 2 &&
        ((v.front() == '"' && v.back() == '"') ||
         (v.front() == '\'' && v.back() == '\''))) {
        v = v.substr(1, v.size() - 2);
    }
    if (v == "true") return json(true);
    if (v == "false") return json(false);
    if (v == "null" || v == "~") return json(nullptr);
    // 尝试数字（整数/浮点），失败按字符串
    try {
        size_t sz = 0;
        if (v.find_first_not_of("+-0123456789.") == std::string::npos &&
            !v.empty()) {
            long double d = std::stold(v, &sz);
            if (sz == v.size()) {
                // 整数优先
                if (v.find('.') == std::string::npos) {
                    try { return json(std::stoll(v)); } catch (...) {}
                }
                return json(double(d));
            }
        }
    } catch (...) {}
    return json(v);
}

// 递归解析：lines 中从 idx 开始、缩进 >= indent 的行，构建一个对象
inline json parse_block(const std::vector<std::string>& lines, size_t& idx,
                        size_t indent) {
    json obj = json::object();
    while (idx < lines.size()) {
        const std::string& line = lines[idx];
        size_t ind = indent_of(line);
        if (line.empty()) { ++idx; continue; }
        if (ind < indent) break;                      // 上溯到父级
        std::string body = trim(strip_comment(line));
        if (body.empty()) { ++idx; continue; }

        // 键值分割：找 "key: " 或行尾 "key:"（分组）
        size_t colon = std::string::npos;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] == ':') {
                size_t next = i + 1;
                if (next >= body.size() || body[next] == ' ') { colon = i; break; }
            }
        }
        if (colon == std::string::npos) { ++idx; continue; }   // 非 key:value 行，跳过
        std::string key = trim(body.substr(0, colon));
        std::string rest = trim(body.substr(colon + 1));

        if (rest.empty()) {
            // 分组：子项缩进更大。解析子块。
            size_t child_indent = std::string::npos;
            for (size_t k = idx + 1; k < lines.size(); ++k) {
                size_t ci = indent_of(lines[k]);
                if (ci > ind) { child_indent = ci; break; }
            }
            size_t old = idx;
            ++idx;   // 跳过分组头
            if (child_indent != std::string::npos) {
                obj[key] = parse_block(lines, idx, child_indent);
            } else {
                obj[key] = json(nullptr);
            }
            (void)old;
        } else {
            obj[key] = to_value(rest);
            ++idx;
        }
    }
    return obj;
}

inline json parse(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    size_t idx = 0;
    return parse_block(lines, idx, 0);
}
}  // namespace yaml_light

// ========== 配置管理 ==========

inline AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open config: " + path);
    }

    std::stringstream ss;
    ss << f.rdbuf();
    json j = yaml_light::parse(ss.str());

    // 音频配置
    if (j.contains("audio")) {
        auto& a = j["audio"];
        cfg.audio_device = a.value("device", 0);
        cfg.audio_sample_rate = a.value("sample_rate", 48000);
        cfg.audio_channels = a.value("channels", 1);
        cfg.audio_buffer_ms = a.value("buffer_ms", 10);
    }

    // VAD 参数
    if (j.contains("vad")) {
        auto& v = j["vad"];
        cfg.vad_threshold = v.value("threshold", 0.5f);
        cfg.vad_min_speech_ms = v.value("min_speech_ms", 220);
        cfg.vad_min_silence_ms = v.value("min_silence_ms", 300);
    }

    // 打断参数
    if (j.contains("barge_in")) {
        auto& b = j["barge_in"];
        cfg.barge_in_min_ms = b.value("min_ms", 160);
        cfg.backchannel_max_ms = b.value("backchannel_max_ms", 600);
        cfg.coherence_max = b.value("coherence_max", 0.6f);
        cfg.fade_out_ms = b.value("fade_out_ms", 40);
        cfg.interrupt_duck_volume = b.value("duck_volume", 0.35f);
        cfg.barge_in_require_threshold = b.value("require_threshold", true);
    }

    // EOU 参数
    if (j.contains("eou")) {
        auto& e = j["eou"];
        cfg.eou_fast_ms = e.value("fast_ms", 350);
        cfg.eou_force_ms = e.value("force_ms", 900);
        cfg.max_utterance_ms = e.value("max_utterance_ms", 20000);
    }

    // 模型路径
    cfg.vad_model = j.value("vad_model", "models/vad/silero_vad.onnx");
    cfg.asr_model = j.value("asr_model", "models/asr/whisper-tiny");
    cfg.tts_model = j.value("tts_model", "models/tts/kokoro-multi-lang-v1_0");
    cfg.llm_model = j.value("llm_model", "models/qwen3-4b-q4_k_m.gguf");
    cfg.tts_engine = j.value("tts_engine", "simple");
    cfg.tts_speed = j.value("tts_speed", 1.0f);
    cfg.tts_pitch = j.value("tts_pitch", 1.0f);
    cfg.tts_speaker_id = j.value("tts_speaker_id", 45);
    cfg.tts_prosody_adapter = j.value("tts_prosody_adapter", "auto");
    cfg.tts_bridge_endpoint = j.value("tts_bridge_endpoint", "http://127.0.0.1:8770");
    cfg.tts_bridge_timeout_ms = j.value("tts_bridge_timeout_ms", 30000);
    cfg.tts_bridge_health_timeout_ms = j.value("tts_bridge_health_timeout_ms", 1500);
    // 语音输入方式：ptt=按住说话（默认）；vad=自动切分。
    // 只认这两个值，其它（含文件里没写、以及历史遗留的脏值）一律当 ptt。
    {
        std::string m = j.value("input_mode", std::string("ptt"));
        cfg.input_mode = (m == "vad") ? "vad" : "ptt";
    }

    // Conversation Runtime 开关
    cfg.enable_interrupt_truncation = j.value("enable_interrupt_truncation", true);
    cfg.enable_semantic_turn        = j.value("enable_semantic_turn", true);
    cfg.eagerness                   = j.value("eagerness", std::string("low"));
    cfg.trace_path                  = j.value("trace_path", std::string(""));

    // ZipVoice（零样本克隆）
    if (j.contains("zipvoice")) {
        auto& z = j["zipvoice"];
        cfg.tts_zipvoice_vocoder = z.value("vocoder", cfg.tts_zipvoice_vocoder);
        cfg.tts_ref_audio = z.value("ref_audio", cfg.tts_ref_audio);
        cfg.tts_ref_text = z.value("ref_text", cfg.tts_ref_text);
        cfg.tts_num_steps = z.value("num_steps", cfg.tts_num_steps);
        cfg.tts_guidance_scale = z.value("guidance_scale", cfg.tts_guidance_scale);
    }

    // 搜索
    if (j.contains("search")) {
        auto& s = j["search"];
        cfg.searxng_url = s.value("searxng_url", "http://localhost:8080");
        cfg.tavily_key = s.value("tavily_key", "");
        cfg.brave_key = s.value("brave_key", "");
    }

    // R7：在线强模型（OpenAI 兼容）。
    // 注意：api_key **不从 yaml 读** —— 配置文件可能进 git。
    // 密钥从环境变量 VOICE_AGENT_REMOTE_API_KEY 取。
    if (j.contains("remote_llm")) {
        auto& r = j["remote_llm"];
        cfg.remote_base_url = r.value("base_url", "");
        cfg.remote_model = r.value("model", "");
        cfg.remote_timeout_ms = r.value("timeout_ms", 30000);
        cfg.remote_for_deep = r.value("for_deep", true);
        cfg.remote_for_agent = r.value("for_agent", true);
        cfg.remote_for_search = r.value("for_search", true);
    }
    if (const char* key = std::getenv("VOICE_AGENT_REMOTE_API_KEY")) {
        cfg.remote_api_key_env = key;
    }

    // 日志
    cfg.log_level = j.value("log_level", "info");
    cfg.log_file = j.value("log_file", "");

    return cfg;
}

// ========== 配置回写 ==========
// GUI 改设置后要把新值写回 agent.yaml。这里的定位规则比"全局搜第一个
// key:"严格：短key（min_ms / key / id）全局搜很容易命中别的分组，
// 所以必须限定在目标分组体内。
//
// 分组体边界：从 "group:" 之后到下一个**顶层**key（行首无缩进且含 ':'）。
//   barge_in:← 目标分组
//     min_ms: 160← 组内
//     fade_out_ms: 40              ← 组内（虽然名字里也有 min_ms）
// eou:            ← 顶层，下一段开始
namespace yaml_edit {

// 在 group 分组内把 key 设成 val（不含 key: 与空格）。
// 返回 true 表示文本有改动。分组不存在时返回 false 且不动文本。
inline bool set_in_group(std::string& text, const std::string& group,
                         const std::string& key, const std::string& val) {
    const std::string gpat = group + ":";
    const auto gpos = text.find(gpat);
    if (gpos == std::string::npos) return false;

    // 找分组体结束位置
    size_t gend = text.size();
    for (size_t scan = gpos + gpat.size(); scan < text.size();) {
        const size_t lineEnd = text.find('\n', scan);
        const size_t end = (lineEnd == std::string::npos) ? text.size() : lineEnd;
        const std::string line = text.substr(scan, end - scan);
        const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        if (!blank && line[0] != ' ' && line[0] != '\t' &&
            line.find(':') != std::string::npos) {
            gend = scan;   // 下一个顶层 key，本段到此为止
            break;
        }
        scan = end + 1;
    }

    const std::string kpat = key + ":";
    const auto kpos = text.find(kpat, gpos);
    if (kpos != std::string::npos && kpos < gend) {
        auto lineEnd = text.find('\n', kpos);
        if (lineEnd == std::string::npos || lineEnd > gend) lineEnd = gend;
        const std::string newLine = kpat + " " + val;
        if (text.compare(kpos, lineEnd - kpos, newLine) == 0) return false;
        text.replace(kpos, lineEnd - kpos, newLine);
        return true;
    }

    // 组内没有这个 key：在分组末尾插一行（保持两空格缩进）
    size_t insertPos = gend;
    if (insertPos > gpos) {
        const size_t prevEnd = text.rfind('\n', insertPos - 1);
        if (prevEnd != std::string::npos && prevEnd + 1 < insertPos)
            insertPos = prevEnd + 1;
    }
    text.insert(insertPos, "  " + kpat + " " + val + "\n");
    return true;
}

}  // namespace yaml_edit

// 打印配置（用于调试）
inline void print_config(const AppConfig& cfg) {
    LOG_INFO("=== Configuration ===");
    LOG_INFO("Audio: device={}, rate={}, channels={}, buffer={}ms",
             cfg.audio_device, cfg.audio_sample_rate, cfg.audio_channels, cfg.audio_buffer_ms);
    LOG_INFO("VAD: threshold={}, min_speech={}ms, min_silence={}ms",
             cfg.vad_threshold, cfg.vad_min_speech_ms, cfg.vad_min_silence_ms);
    LOG_INFO("Barge-in: min={}ms, backchannel_max={}ms, fade_out={}ms, duck={:.2f}, threshold={}",
             cfg.barge_in_min_ms, cfg.backchannel_max_ms, cfg.fade_out_ms,
             cfg.interrupt_duck_volume,
             cfg.barge_in_require_threshold ? "on" : "off");
    LOG_INFO("Input mode: {}", cfg.input_mode);
    LOG_INFO("EOU: fast={}ms, force={}ms, max_utterance={}ms",
             cfg.eou_fast_ms, cfg.eou_force_ms, cfg.max_utterance_ms);
    LOG_INFO("Models: VAD={}, ASR={}, TTS={}, LLM={}",
             cfg.vad_model, cfg.asr_model, cfg.tts_model, cfg.llm_model);
    LOG_INFO("Search: searxng={}", cfg.searxng_url);
}
