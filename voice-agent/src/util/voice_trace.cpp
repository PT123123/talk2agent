// src/util/voice_trace.cpp
#include "util/voice_trace.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace voice_agent {

using json = nlohmann::json;

// ========== TraceEvent ==========

json TraceEvent::to_json() const {
    json j;
    j["t_us"] = t_us;
    j["type"] = type;
    if (!detail.empty()) j["detail"] = detail;
    if (ms > 0.0) j["ms"] = ms;
    return j;
}

// ========== TurnRecord ==========

json TurnRecord::to_json() const {
    json j;
    j["session_id"] = session_id;
    j["turn_id"] = turn_id;
    j["user_text"] = user_text;
    j["user_intent"] = user_intent;
    j["policy_action"] = policy_action;
    j["policy_reason"] = policy_reason;
    j["route_tier"] = route_tier;
    j["route_engine"] = route_engine;
    j["spoke_ack"] = spoke_ack;
    if (!ack_text.empty()) j["ack_text"] = ack_text;

    // 延迟指标统一放在 latency 子对象下，便于离线按字段聚合
    json lat;
    auto put = [&lat](const char* k, double v) {
        if (v >= 0.0) lat[k] = v;
    };
    put("speech_to_asr_ms", speech_to_asr_ms);
    put("asr_to_first_audio_ms", asr_to_first_audio_ms);
    put("llm_ttft_ms", llm_ttft_ms);
    put("tts_first_audio_ms", tts_first_audio_ms);
    put("total_ms", total_ms);
    if (!lat.empty()) j["latency"] = std::move(lat);

    j["interrupted"] = interrupted;
    j["discard_count"] = discard_count;

    if (!events.empty()) {
        json evs = json::array();
        for (const auto& e : events) evs.push_back(e.to_json());
        j["events"] = std::move(evs);
    }
    return j;
}

// ========== VoiceTraceWriter ==========

VoiceTraceWriter::VoiceTraceWriter(std::string path, bool enabled)
    : path_(std::move(path)), enabled_(enabled) {
    if (!enabled_ || path_.empty()) return;
    try {
        // 父目录不存在就建
        const std::filesystem::path p(path_);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        file_.open(path_, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            LOG_WARN("Voice trace: cannot open '{}'", path_);
            enabled_ = false;
        }
    } catch (const std::exception& e) {
        LOG_WARN("Voice trace: open failed: {}", e.what());
        enabled_ = false;
    }
}

VoiceTraceWriter::~VoiceTraceWriter() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void VoiceTraceWriter::write_turn(const TurnRecord& rec) {
    if (!enabled_ || !file_.is_open()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    file_ << rec.to_json().dump() << "\n";
    // 每轮立即 flush：长跑崩溃时已完成的轮次不能丢
    file_.flush();
    ++turn_counter_;
}

void VoiceTraceWriter::write_event(const TraceEvent& ev, const std::string& session_id) {
    if (!enabled_ || !file_.is_open()) return;
    json j;
    j["kind"] = "event";
    j["session_id"] = session_id;
    j["event"] = ev.to_json();
    std::lock_guard<std::mutex> lock(mutex_);
    file_ << j.dump() << "\n";
    file_.flush();
}

void VoiceTraceWriter::write_session_header(const json& info) {
    if (!enabled_ || !file_.is_open()) return;
    json j;
    j["kind"] = "header";
    j["info"] = info;
    std::lock_guard<std::mutex> lock(mutex_);
    file_ << j.dump() << "\n";
    file_.flush();
}

// ========== VoiceTraceReader ==========

VoiceTraceReader::VoiceTraceReader(const std::string& path) : path_(path) {
    file_.open(path_, std::ios::in);
    if (!file_.good()) {
        LOG_WARN("Voice trace: cannot read '{}'", path);
    }
}

std::vector<json> VoiceTraceReader::turns() const {
    std::vector<json> out;
    std::ifstream f(path_);
    if (!f.good()) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded()) continue;
        // 轮次行有 turn_id；header/event 行用 kind 标记
        if (j.contains("turn_id")) out.push_back(std::move(j));
    }
    return out;
}

VoiceTraceReader::LatencyStats VoiceTraceReader::compute_latency(
    const std::vector<json>& turns, const std::string& field) {
    LatencyStats s;
    std::vector<double> v;
    v.reserve(turns.size());
    for (const auto& t : turns) {
        auto lat = t.find("latency");
        if (lat == t.end()) continue;
        auto it = lat->find(field);
        if (it == lat->end()) continue;
        if (!it->is_number()) continue;
        v.push_back(it->get<double>());
    }
    if (v.empty()) return s;

    std::sort(v.begin(), v.end());
    auto q = [&v](double p) {
        // 最近秩法：小样本下比线性插值更稳，且不会插出没测到的值
        if (v.empty()) return 0.0;
        size_t idx = static_cast<size_t>(p * (v.size() - 1) + 0.5);
        if (idx >= v.size()) idx = v.size() - 1;
        return v[idx];
    };
    s.count = v.size();
    s.p50 = q(0.50);
    s.p90 = q(0.90);
    s.p99 = q(0.99);
    s.min = v.front();
    s.max = v.back();
    double sum = 0.0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    return s;
}

VoiceTraceReader::Summary VoiceTraceReader::summarize(const std::vector<json>& turns) {
    Summary s;
    s.turns = turns.size();
    for (const auto& t : turns) {
        if (t.value("interrupted", false)) ++s.interrupted;
        if (t.value("spoke_ack", false)) ++s.acked;
        if (t.value("discard_count", 0) > 0) ++s.superseded;
        if (t.value("failed", false)) ++s.failed;
    }
    return s;
}

}  // namespace voice_agent
