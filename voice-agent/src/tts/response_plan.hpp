// src/tts/response_plan.hpp
#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "tts/prosody.hpp"

namespace voice_agent {

// ========== 播放游标 ==========
// 关键概念：**已播出的不可改，未播出的可丢弃**。
// 这是 Response Revision 的全部原理 —— 模型意识到自己说错时，
// 已经播出的话收不回来，但后面还没念的内容可以丢掉重生成。
enum class SegmentState {
    Pending,     // 已规划，未送 TTS
    Synthesized, // 已合成音频，未播
    Playing,     // 正在播
    Played,      // 播完（不可修订）
    Discarded    // 被修订丢弃
};

// ========== ResponsePlan ==========
// 管理一条回答的分段生命周期。它是 LLM（说什么）与 TTS（怎么说）之间的
// 缓冲层，也是"响应修订"能落地的地方。
//
// 典型用法（每轮对话）：
//   plan.append(planner.plan(llm_chunk1));   // 句 1
//   if (auto s = plan.next_to_synthesize()) tts->synthesize(s->text, ...);
//   ...
//   // 模型改了主意：
//   plan.discard_unplayed_from(1);            // 丢弃句 2 之后未播的内容
//   plan.append(planner.plan(corrected));     // 重新规划
class ResponsePlan {
public:
    ResponsePlan() = default;

    // 追加一批段（来自 LLM 的新输出）
    void append(const std::vector<SpeechSegment>& segs);

    // 取出下一个该合成的段（自动跳过已丢弃的）
    std::optional<SpeechSegment> next_to_synthesize();

    // 标记某段已合成（按段 id，段 id 即 append 时的下标）
    void mark_synthesized(size_t index);
    void mark_playing(size_t index);
    void mark_played(size_t index);

    // ---- Response Revision ----
    // 丢弃从 from_index 起、尚未播出的所有段。已播出的保留。
    // 返回实际丢弃的段数。
    size_t discard_unplayed_from(size_t from_index);

    // 丢弃所有未播段（整条回答作废）
    size_t discard_all_unplayed();

    // 还有多少段没播（Pending / Synthesized）
    size_t pending_count() const;

    // 已播完且没有待处理段
    bool finished() const;

    // 当前播放下标；无则返回 -1
    long long playing_index() const;

    // 已播段数
    size_t played_count() const;

    // ---- 观测 ----
    struct Stats {
        size_t appended{0};
        size_t played{0};
        size_t discarded{0};
        size_t revised_rounds{0};   // 修订过几轮
    };
    Stats stats() const;

    void clear();

private:
    struct Slot {
        SpeechSegment seg;
        SegmentState state{SegmentState::Pending};
    };

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    size_t cursor_{0};              // 下一个待合成下标
    long long playing_idx_{-1};
    Stats stats_;
};

}  // namespace voice_agent
