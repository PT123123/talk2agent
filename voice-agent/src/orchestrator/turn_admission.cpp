// src/orchestrator/turn_admission.cpp
#include "turn_admission.hpp"

#include "core/event_bus.hpp"
#include "util/log.hpp"

namespace voice_agent {

TurnAdmissionResult TurnAdmission::evaluate(const std::string& text) {
    TurnAdmissionResult r;

    // ---- 1. 意图分类 ----
    r.intent = classifier_.classify(text);
    const UserSpeechIntent prev = prev_intent_.load();
    last_intent_.store(r.intent);
    prev_intent_.store(r.intent);

    {
        Event e{EventType::UserIntent, 0, text, {},
                user_speech_intent_to_string(r.intent)};
        e.turn_id = current_turn_id_.load();
        global_event_bus().publish(std::move(e));
    }

    // ---- 2. 换话题 ----
    // 连着说两次"算了…"不算真的换 —— 否则用户改主意两次就误清上下文。
    if (cfg_.enable_topic_supersede &&
        r.intent == UserSpeechIntent::TopicChange &&
        prev != UserSpeechIntent::TopicChange) {
        r.topic_changed = true;
        r.supersede_stale_tasks = true;
        work().on_topic_changed(text);
        Event e{EventType::TopicChanged, 0, text, {}, "user-initiated"};
        e.turn_id = current_turn_id_.load();
        global_event_bus().publish(std::move(e));
        LOG_INFO("Topic changed - transient working context cleared");
    }

    // ---- 3. Backchannel：Agent 继续说，不起轮次也不打断 ----
    if (r.intent == UserSpeechIntent::Backchannel) {
        r.should_start_turn = false;
        r.should_interrupt = false;
        r.decision.action = ResponseAction::Silence;
        r.decision.tier = ModelTier::Fast;
        r.reason = "backchannel: keep current speech, start nothing";
        return r;
    }

    // 续说（"继续""然后呢"）在 Agent 说话时也不该抢话
    if (r.intent == UserSpeechIntent::Continuation && cfg_.agent_speaking) {
        r.should_start_turn = false;
        r.should_interrupt = false;
        r.decision.action = ResponseAction::Silence;
        r.decision.tier = ModelTier::Fast;
        r.reason = "continuation while agent speaking: wait";
        return r;
    }

    // ---- 4. 打断类：让出话轮，但自己不接着说 ----
    if (r.intent == UserSpeechIntent::Interruption) {
        r.should_interrupt = true;
        r.should_start_turn = false;
        r.decision.action = ResponseAction::Silence;
        r.decision.tier = ModelTier::Fast;
        r.reason = "interruption: yield turn, stay quiet";
        return r;
    }

    // ---- 5. ResponsePolicy ----
    if (cfg_.enable_response_policy) {
        ResponsePolicyInput in;
        in.user_text = text;
        in.intent = r.intent;
        in.agent_is_speaking = cfg_.agent_speaking;
        r.decision = policy_.decide(in);
        r.reason = r.decision.reason;

        {
            Event e{EventType::RouteDecision, 0, "", {},
                    model_tier_to_string(r.decision.tier)};
            e.turn_id = current_turn_id_.load();
            global_event_bus().publish(std::move(e));
        }

        if (r.decision.action == ResponseAction::Silence) {
            r.should_start_turn = false;
            // Policy 判静默但意图是"换话题/纠正"时，仍要让出话轮
            r.should_interrupt = (r.intent == UserSpeechIntent::TopicChange ||
                                  r.intent == UserSpeechIntent::Correction) &&
                                 cfg_.agent_speaking;
            return r;
        }
    } else {
        r.decision.action = ResponseAction::Answer;
        r.decision.tier = ModelTier::Normal;
        r.reason = "policy disabled: default answer";
    }

    // ---- 6. 真正起一轮 ----
    r.should_start_turn = true;
    r.should_interrupt = cfg_.agent_speaking;
    current_turn_id_.store(next_turn_id_++);
    return r;
}

void TurnAdmission::reset() {
    last_intent_.store(UserSpeechIntent::Content);
    prev_intent_.store(UserSpeechIntent::Content);
    current_turn_id_.store(0);
    next_turn_id_.store(1);
    policy_.clear_silence_window();
    work().clear();
}

}  // namespace voice_agent
