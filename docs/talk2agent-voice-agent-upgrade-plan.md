# talk2agent：全天候自然语音 Agent 升级计划

> **用途：直接发给负责 `talk2agent` 的 Coding Agent。**
>
> 目标不是简单增加一个 TTS，而是把现有的全双工 Voice Agent 升级成一个可以**长期、自然、低延迟地和用户连续交流，同时在后台调用 Memory / Search / Local Agent / Strong LLM 完成工作的 Voice Agent Runtime**。
>
> 仓库：<https://github.com/PT123123/talk2agent>
>
> 本文基于当前仓库已有的架构/实施计划继续往上设计，不要求推倒重来。当前仓库已经有 C++20 编排层、Audio I/O、AEC、VAD、Turn Detection、streaming ASR、LLM/Tool Calling、Memory、SearchProvider、MCP、本地/远程接口抽象，以及 M0-M8 的实施路线；后续重点应该从“把语音链路跑通”进一步转向“把对话行为做自然”。

---

## 1. 先理解现在的项目定位

当前 `talk2agent` 的设计已经不是普通的“语音输入 → LLM → TTS”。仓库当前方案明确采用：

- 本地优先
- C++20 主编排层
- GPU/CPU 本地推理
- 全双工
- AEC / VAD / Smart Turn
- streaming ASR
- LLM + Tool Calling
- Memory
- Search
- MCP
- 本地与在线模型接口同构

架构和实施计划里已经有：

```text
Audio I/O
    ↓
AEC / NS / VAD
    ↓
ASR
    ↓
Turn Detection
    ↓
LLM
    ↓
Tool / Memory / Search
    ↓
TTS
    ↓
Playback
```

以及：

```text
IDLE
 → LISTENING
 → EOU_PENDING
 → THINKING
 → SPEAKING
 → INTERRUPTED
 → LISTENING
```

这些基础设计不要推倒重来。

真正缺的，是位于这些模块之上的一层：

> **Conversation Orchestrator / Conversation Runtime**

它负责决定：

- 现在是不是应该回答
- 什么时候回答
- 回答多少
- 是否先给一个很短的回应
- 是否应该后台调用 Agent
- 是否需要 Memory
- 是否需要 Search
- 是否需要强模型
- 当前任务是否应该取消/挂起/继续
- 用户突然打断后，后台任务怎么办
- TTS 应该以什么节奏说出来
- 是否应该保持安静

这才是最终“像人在交流”的核心。

参考当前仓库：

- 架构方案：<https://github.com/PT123123/talk2agent/blob/master/全双工语音Agent架构方案.md>
- 实施计划：<https://github.com/PT123123/talk2agent/blob/master/本地全双工语音Agent实施计划.md>

---

# 2. 最终产品目标

最终希望达到的体验不是：

> “我说一句 → 等 AI 想完 → AI 回答一句。”

而是：

> “我可以和它持续讲话；它知道我什么时候说完了；我可以随时打断；它可以一边和我聊，一边在后台查资料、翻我的记忆、操作本地 Agent；简单问题快速回答，复杂问题自己升级到更强模型；真正需要长时间工作的东西不阻塞前台对话。”

理想状态类似：

```text
User Voice
    ↓
Streaming ASR
    ↓
Conversation Runtime
    ├── Quick Response
    ├── Context / Memory
    ├── Search
    ├── Local Agent
    ├── Strong Online LLM
    └── Deep Reasoning
    ↓
Response Planner
    ↓
Prosody / Timing Planner
    ↓
Streaming TTS
    ↓
Speaker
```

其中最重要的不是 TTS 本身，而是：

**Timing + Turn Taking + Interruption + Context + Agent orchestration + Latency**。

---

# 3. 第一优先级：增加真正的 Conversation Orchestrator

现在已有状态机，但不要再把它理解成一个简单的：

```text
LISTENING → THINKING → SPEAKING
```

建议升级成：

```text
                    ┌──────────────┐
                    │   LISTENING  │
                    └──────┬───────┘
                           ↓
                  PROCESSING_INPUT
                           ↓
                 ┌─────────┴─────────┐
                 │  Response Policy  │
                 └─────────┬─────────┘
                           │
        ┌──────────────────┼──────────────────┐
        ↓                  ↓                  ↓
   QUICK_REPLY        NEED_CONTEXT       NEED_ACTION
        │                  │                  │
        │                  ↓                  ↓
        │             Memory/Search      Tool/Agent
        │                  │                  │
        └──────────────────┼──────────────────┘
                           ↓
                   RESPONSE_PLANNING
                           ↓
                       SPEAKING
                           ↓
                  ┌────────┴────────┐
                  ↓                 ↓
             COMPLETED         INTERRUPTED
                                    ↓
                                LISTENING
```

但注意：**后台任务不能跟 Conversation 状态绑定成单线程阻塞关系。**

应该允许一个 Session 同时存在：

```text
Foreground response
Background search
Background memory retrieval
Background agent task
Speculative task
```

---

# 4. 引入 Event / Task 两套概念

推荐把整个 Runtime 进一步事件化。

## 4.1 Event

事件例如：

```text
AUDIO_STARTED
AUDIO_STOPPED
ASR_PARTIAL
ASR_FINAL
TURN_COMPLETE
TURN_CONTINUING
BACKCHANNEL
USER_INTERRUPT
MODEL_STARTED
MODEL_FIRST_TOKEN
MODEL_COMPLETED
TOOL_STARTED
TOOL_PROGRESS
TOOL_COMPLETED
TOOL_FAILED
TTS_STARTED
TTS_FIRST_AUDIO
TTS_CANCELLED
PLAYBACK_STARTED
PLAYBACK_ENDED
TOPIC_CHANGED
```

所有模块通过 EventBus 通信。

## 4.2 Task

后台工作不能只写成 `await agent()`。

至少需要：

```cpp
enum class TaskPriority {
    Foreground,
    Background,
    Speculative
};

enum class TaskState {
    Queued,
    Running,
    Paused,
    Completed,
    Cancelled,
    Superseded,
    Failed
};
```

这样 Voice Runtime 才能同时管理多个工作。

---

# 5. 增加 Fast Response Layer

这是最值得优先增加的一层。

核心思想：

> **复杂任务可以花时间，但前台对话不能因为后台工作而长时间“死寂”。**

例如用户说：

> “我最近真的有好多事情。”

不应该一定等深度模型 3 秒后才第一次说话。

可能先：

```text
“嗯。”
```

然后后台：

```text
Memory
Recent conversation
Current projects
Current tasks
```

最后再生成完整回应。

但注意：

### Fast Response 不是“每句话都先嗯一下”

它必须允许：

```text
SILENCE
```

安静也是一种合法回应策略。

例如：

- 简单问题 → 直接答
- 有明显延迟的任务 → 先简短 acknowledgement
- 情绪/闲聊 → 快速回应
- 用户还在组织语言 → 不说
- 后台工作非常快 → 不需要说任何 filler

---

# 6. Response Policy：独立决定“要不要说、做多深”

LLM 不应该每次都直接决定完整答案。

增加一个轻量级 `ResponsePolicy`。

建议输入：

```json
{
  "user_text": "...",
  "partial_text": "...",
  "conversation_state": "...",
  "working_context": {},
  "recent_context": {},
  "available_tools": [],
  "latency_budget_ms": 1500
}
```

输出：

```json
{
  "action": "silence | backchannel | quick_reply | answer | search | agent | deep_reasoning",
  "needs_memory": true,
  "needs_search": false,
  "needs_agent": true,
  "depth": "low | medium | high",
  "allow_background": true,
  "latency_budget_ms": 2000
}
```

这个模块不需要大模型。

第一版可以：

```text
规则 + 小模型
```

以后再升级。

不要浪费最强的 LLM token 去做“我要不要先说一句”的判断。

---

# 7. Backchannel / 附和词必须独立建模

目前实施计划已经考虑了：

```text
嗯 / 对 / 好 / 是的 / OK / uh-huh / 继续 / 行
```

这些应该正式定义为一个 User Speech Intent，而不是散落在各个 if 里面。

建议：

```cpp
enum class UserSpeechIntent {
    Content,
    Backchannel,
    Continuation,
    Correction,
    Interruption,
    TopicChange
};
```

例如：

```text
Agent 正在讲话
User：“嗯。”
```

应该：

```text
保持 SPEAKING
```

而：

```text
User：“等一下。”
```

应该：

```text
立即 barge-in
```

---

# 8. 增加“边听边预取”的机制

真正自然的 Voice Agent 不应该等用户完全说完才开始所有工作。

例如：

```text
User:
“我想问一下我之前那个 Agent 项目……”
```

Streaming ASR 已经得到：

```text
“我想问一下我之前那个 Agent 项目”
```

此时可以 speculative 地做：

```text
Memory retrieval
Recent project retrieval
Relevant files prefetch
```

但是：

> **绝对不要在用户还没说完时提交最终答案。**

因此需要区分：

```text
Speculative Work
```

和：

```text
Committed Work
```

如果用户最后说：

> “……不过我其实想问另一个项目。”

前面的 speculative 结果应该直接丢弃或进入 cache。

---

# 9. Foreground / Background Agent

这是升级的核心。

举例：

```text
User:
“帮我查一下 Qwen TTS 最新的情况。”
```

系统：

```text
Foreground:
“我查一下。”

Background:
Search
    ↓
Fetch
    ↓
Rerank
    ↓
Summarize
```

结果出来：

```text
“有，我看到了几个最近的重要变化……”
```

如果中途用户说：

> “算了，我突然想问另一个问题。”

必须做到：

```text
立即停止旧答案的播放
新问题成为 foreground
旧 Search 不一定 kill
旧 Search 可继续 background
结果回来后进入 context/cache
但不要主动抢话播报旧结果
```

---

# 10. 必须支持 Supersede，而不只是 Cancel

`cancel` 不能解决所有问题。

例如任务 A：

```text
Search Phone X
```

用户换话题：

```text
“算了，帮我看一下另一个东西。”
```

此时 A 可以标记：

```text
SUPERSEDED
```

而不是强制杀掉。

它完成以后：

```text
result = cache/context
speak = false
```

这对个人 Agent 非常有用。

---

# 11. Cancellation 必须贯穿整条链路

用户打断 Agent：

```text
USER_INTERRUPT
    ↓
CancelToken.cancel()
    ↓
LLM.stop()
TTS.cancel()
Playback.stop()
```

同时：

```text
Background Search
Background Memory
Background Agent
```

可以根据 Task Policy 决定：

```text
cancel
pause
continue
supersede
```

不要把所有任务都一刀切 cancel。

---

# 12. Tool Call 不允许阻塞整个 Voice Loop

当前仓库已有 ToolRegistry / Search / Memory / MCP 的思路，继续保持。

但是 Runtime 应该变成：

```text
LLM
 ↓
Tool Request
 ↓
Tool Worker
 ↓
EventBus
 ↓
Conversation Orchestrator
```

而不是：

```text
LLM
 ↓
blocking Tool Call
 ↓
Voice Runtime 卡死
```

Tool Worker 至少要发：

```text
TOOL_STARTED
TOOL_PROGRESS
TOOL_COMPLETED
TOOL_FAILED
TOOL_CANCELLED
```

而 Orchestrator 决定：

- 是否通知用户
- 什么时候通知
- 是否保持安静
- 是否继续生成答案

---

# 13. Model Router

最终不应该：

```text
每个问题都调用同一个模型
```

建议支持：

```text
FAST
NORMAL
DEEP
AGENT
SEARCH
BACKGROUND
```

一种典型策略：

### FAST

适合：

- 简单聊天
- acknowledgement
- 简短说明
- 当前时间
- 简单计算

→ 小型本地模型 / rule

### NORMAL

适合：

- 普通问题
- 一般分析
- 常规对话

→ 本地中型模型 / 普通在线模型

### DEEP

适合：

- 复杂推理
- 代码设计
- 长文分析
- 多步骤规划

→ 强在线模型

### AGENT

适合：

- 操作电脑
- 读取文件
- 代码执行
- 本地项目操作
- 复杂 tool chain

→ 现有 Local Agent

### SEARCH

适合：

- 最新新闻
- 最新模型
- 产品实时价格
- 当前事实

→ SearchProvider + LLM

---

# 14. Online Strong LLM + Local Agent 不要二选一

这个方向尤其重要。

最终建议架构：

```text
                 Conversation Orchestrator
                          │
               ┌──────────┴──────────┐
               ↓                     ↓
         Strong Online LLM       Local Fast Model
               │
               ↓
            Tool Router
               │
       ┌───────┼────────┐
       ↓       ↓        ↓
    Memory   Search   Local Agent
```

也就是：

> **在线强模型负责“想”，本地 Agent 负责“做”。**

本地 Agent 可以提供统一工具：

```text
memory_query
memory_save
search
file_search
shell
computer
project_agent
```

在线模型只需要通过标准 tool calling 使用这些工具。

这样：

- 在线模型可以随时更换
- 本地 Agent 继续保持成熟能力
- Memory 不需要塞进超大 prompt
- Agent runtime 不被某个 LLM 绑定

---

# 15. Context Manager

不要只做：

```text
last N messages
```

建议专门建立：

```text
ContextManager
```

动态拼装：

```text
P0 当前用户输入
P1 当前正在讨论的主题
P2 当前任务状态
P3 最近几轮对话
P4 Working Context
P5 Relevant Memory
P6 Tool Results
P7 更旧历史
```

这样可以大幅降低长期对话中的 context 膨胀。

---

# 16. Working Context 与 Long-term Memory 必须分开

例如用户说：

> “我现在正在做 SlideTrace。”

这不一定应该成为长期 Memory。

它应该先成为：

```text
working_context.active_project = SlideTrace
```

之后用户说：

> “那个时间轴的问题……”

Agent 应该优先从 Working Context 解析“那个”。

只有长期有效的信息才进入长期 Memory。

建议明确区分：

```text
Turn Context
Conversation Context
Working Context
Long-Term Memory
```

---

# 17. Conversation Session

长期使用时需要：

```text
Turn
Conversation
Session
Memory
```

### Turn

用户一次输入 + Agent 一次回应。

### Conversation

持续讨论一个主题。

### Session

例如今天连续使用 5 小时。

### Memory

跨 session 仍值得保留的信息。

不要把 5 小时全文都当 prompt。

---

# 18. Search 必须与 Voice Timing 联动

当前项目已经设计 SearchRouter / SearXNG / 在线 Provider。

继续保持，但为 Voice 场景增加预算：

```json
{
  "max_latency_ms": 2500,
  "max_queries": 3,
  "max_results": 8,
  "freshness_required": true,
  "allow_background": true
}
```

例如：

```text
User:
“现在最新的 Qwen3-TTS 怎么样？”

↓

Response Policy:
needs_search = true

↓

Quick Response:
“我看一下最新的。”

↓

Search background

↓

Result

↓

Strong LLM summarize

↓

TTS streaming
```

不要让用户在 5 秒以上的静默里等 Search。

---

# 19. Progress Utterance Policy

长任务需要一种可控的短反馈，但绝不能固定成：

> “让我查一下。”

可以根据任务生成：

```text
“我看一下。”
“我确认一下。”
“这个我得查一下最新的信息。”
“我翻一下之前的记录。”
“这个需要多算一步。”
```

也可以：

```text
不说话
```

因此建议增加：

```text
ProgressUtterancePolicy
```

输入：

```text
task type
expected latency
user mood / urgency
recent speech
```

输出：

```text
silent
acknowledge
progress
```

---

# 20. Streaming Response Planner

TTS 不应该等完整 LLM answer。

应该：

```text
LLM token stream
       ↓
Response Planner
       ↓
Sentence / Clause Segmenter
       ↓
Prosody Planner
       ↓
TTS stream
       ↓
Audio queue
```

例如模型输出：

```text
“我觉得这里有两个问题。第一……”
```

应该在：

```text
“我觉得这里有两个问题。”
```

完成后立即生成 TTS。

而不是等整个回答结束。

---

# 21. TTS 抽象必须升级

当前方案默认 Kokoro 是合理的轻量路线，但不要永久锁死 TTS。

建议保持：

```cpp
class ITts {
public:
    virtual StreamHandle synthesize_stream(
        const SpeechPlan& plan,
        CancelToken token) = 0;

    virtual void cancel() = 0;
};
```

同时增加：

```cpp
struct SpeechPlan {
    std::string text;

    float rate = 1.0f;
    float pitch = 0.0f;
    float energy = 0.5f;
    float warmth = 0.5f;

    int pause_before_ms = 0;
    int pause_after_ms = 0;

    std::vector<std::string> tags;
};
```

这里的重点是：

> LLM 负责“说什么”，而 Speech/Prosody 层负责“怎么说”。

---

# 22. Prosody Planner

最终自然度不要只依赖 TTS 模型。

建议增加：

```text
ProsodyPlanner
```

例如：

```json
{
  "segments": [
    {
      "text": "嗯，我明白。",
      "rate": 0.92,
      "energy": 0.25,
      "warmth": 0.65,
      "pause_after_ms": 180
    },
    {
      "text": "这个问题其实挺有意思的。",
      "rate": 0.98,
      "energy": 0.55,
      "warmth": 0.70,
      "emphasis": ["挺有意思"]
    }
  ]
}
```

这样未来换 TTS 不需要修改整个 Agent。

---

# 23. 情绪控制不要只有 happy / sad / angry

这种情绪标签过于粗糙。

更建议把声音表达拆成连续变量：

```text
energy
warmth
certainty
urgency
pace
pause_density
pitch_variance
```

比如：

### 平静讨论

```text
energy ↓
pace ↓
warmth ↑
pause ↑
```

### 兴奋发现

```text
energy ↑
pace ↑
pitch_variance ↑
```

### 用户明显烦躁

不要机械地输出：

```text
emotion = sad
```

更合理的是：

```text
energy ↓
pace ↓
intensity ↓
warmth ↑
```

---

# 24. TTS 模型建议至少做两个 Adapter 实验

不要把当前 Kokoro 直接升级成“最终方案”。

建议先做两个新 backend。

## 24.1 Qwen3-TTS

重点测试：

- 中文自然度
- 中英文混合
- Voice cloning
- Voice design
- streaming
- instruction/control
- 长文本稳定性
- first audio latency
- GPU memory

官方仓库：

<https://github.com/QwenLM/Qwen3-TTS>

## 24.2 Chatterbox

重点测试：

- conversational speech
- pause
- laugh / chuckle
- sigh / paralinguistic expression
- interruption 后重新开始
- streaming latency
- 长时间连续运行

官方仓库：

<https://github.com/resemble-ai/chatterbox>

另外不要只看“单句试听音质”，要看**连续 30 分钟对话的整体体验**。

---

# 25. 为什么不要只把重点放到 TTS

“真人感”主要来自：

```text
1. timing
2. turn-taking
3. interruption
4. response length
5. backchannel
6. context awareness
7. latency
8. prosody
9. voice quality
```

其中前 7 项都不是 TTS 单独解决的。

一个声音非常逼真的模型，如果：

```text
用户停顿 400ms 就抢话
每次都说“让我想想”
用户打断还继续播
不记得前面说过什么
每一个问题都调用大模型
搜索时沉默 8 秒
```

仍然不会像自然交流。

因此：

> **Timing 比 TTS 音质更优先。**

---

# 26. Conversation Timing Engine

建议把 timing 单独抽象。

例如：

```cpp
struct TimingDecision {
    bool respond_now;
    int delay_ms;

    bool use_ack;
    bool use_filler;

    bool can_interrupt;
    bool allow_background;
};
```

可能返回：

```text
RESPOND_NOW
DELAY_200MS
DELAY_500MS
ACK_THEN_WORK
SILENCE_AND_LISTEN
```

注意：

> 不要通过大量随机 sleep 来模拟真人。

每一个 delay 都应该有原因：

- 等候 Turn Detector
- 等待第一批上下文
- 避免打断用户
- 等待更高质量的模型输出
- 等待当前句结束

---

# 27. User Topic Change

必须显式建模“换话题”。

例如：

```text
Agent：
“所以我觉得第一个问题是——”

User：
“等一下，我突然想到另外一个东西……”
```

应该：

```text
TTS stop
Current response superseded
New user turn priority = foreground
旧 background tasks 按策略继续/暂停
```

绝对不要：

```text
把旧答案播完再回答新问题
```

---

# 28. Response Revision

另一个高级能力：

LLM 已经开始输出：

```text
“我认为第一点是……”
```

后面才意识到：

```text
“其实第二个因素更加重要。”
```

TTS 不应该无脑把全文播完。

需要支持：

```text
Response Revision
```

规则可以是：

- 已经播放的音频不能修改
- 未播放的 segment 可以丢弃
- 可以重新生成后续内容
- 必要时当前句结束后立即切换

这会显著提高“像人在思考和修正”的感觉。

---

# 29. Long-running Session

最终目标不是 Demo 运行 5 分钟，而是：

```text
1 hour
4 hours
8 hours
```

必须测试：

- RAM leak
- VRAM leak
- thread leak
- audio device reconnect
- Bluetooth headset reconnect
- network reconnect
- search timeout
- LLM disconnect
- TTS crash
- ASR crash
- tool timeout
- model restart

一个局部模块失败时：

> **不能让整个 Voice Runtime 退出。**

---

# 30. Graceful Degradation

建议明确降级链：

```text
Strong Remote LLM unavailable
        ↓
Local LLM

Search unavailable
        ↓
Cached / Local context

Deep Agent unavailable
        ↓
Normal LLM

Main TTS unavailable
        ↓
Fallback TTS

GPU overloaded
        ↓
Small / CPU model
```

Audio realtime path 应始终拥有最高优先级。

---

# 31. GPU Resource Manager

增加一个统一资源管理层：

```text
ResourceManager
```

监控：

```text
GPU memory
GPU utilization
CPU utilization
Audio realtime load
Concurrent inference jobs
```

特别需要避免：

```text
ASR
+ TTS
+ Reranker
+ LLM
+ embedding
```

突然同时抢 GPU 导致实时音频卡顿。

原则：

> Audio realtime thread 绝对不能因为模型推理阻塞。

---

# 32. Audio Realtime 层继续维持现有原则

当前实施计划已经把这一点写得很明确：

- 10ms frame
- ring buffer
- AEC reference tap
- audio callback 不做同步 IO
- 异步 logging

这些不要削弱。

尤其：

```text
Audio callback
```

里面禁止：

```text
malloc
filesystem IO
blocking network
blocking mutex
LLM inference
```

---

# 33. 评估体系：不要靠“主观感觉不错”

需要建立 Voice Agent Replay。

建议目录：

```text
tests/
├── audio/
├── conversations/
├── interruption/
├── timing/
├── routing/
├── search/
└── memory/
```

至少记录：

```text
ASR partial latency
Turn completion latency
LLM TTFT
TTS first audio latency
End-to-end latency
Barge-in latency
False interruption rate
False response rate
Backchannel interruption rate
Tool latency
Search latency
Memory retrieval latency
```

---

# 34. Conversation Replay

每一轮对话都建议能记录成：

```json
{
  "session_id": "...",
  "turn_id": "...",
  "user_audio": "...",
  "partial_asr": [],
  "final_asr": "...",
  "user_intent": "Content",
  "turn_decision": "Complete",
  "router_decision": "Deep",
  "context_sources": [],
  "tool_calls": [],
  "llm_ttft_ms": 0,
  "tts_first_packet_ms": 0,
  "interruptions": [],
  "final_response": "...",
  "timing_events": []
}
```

这样可以离线比较不同实现。

---

# 35. Voice Trace

每个 Turn 都建议产生 trace：

```text
21:31:02.100 USER_SPEECH_START
21:31:02.350 ASR_PARTIAL
21:31:03.100 ASR_PARTIAL
21:31:03.650 USER_SPEECH_END
21:31:03.700 TURN_COMPLETE
21:31:03.710 ROUTE=DEEP
21:31:03.900 MEMORY_QUERY
21:31:04.100 LLM_START
21:31:04.380 LLM_FIRST_TOKEN
21:31:04.600 TTS_FIRST_PACKET
21:31:04.650 PLAYBACK_START
21:31:06.100 USER_BARGE_IN
21:31:06.130 TTS_CANCEL
21:31:06.150 FOREGROUND_RESPONSE_SUPERSEDED
```

这对调试极其重要。

---

# 36. 建议新增接口

## 36.1 Conversation Orchestrator

```cpp
class IConversationOrchestrator {
public:
    virtual void on_audio(const AudioFrame&) = 0;
    virtual void on_user_text(const std::string&) = 0;
    virtual void on_tool_event(const ToolEvent&) = 0;
    virtual void on_model_event(const ModelEvent&) = 0;
    virtual void on_playback_event(const PlaybackEvent&) = 0;
};
```

## 36.2 Response Policy

```cpp
struct ResponseDecision {
    enum class Action {
        Silence,
        Backchannel,
        QuickReply,
        Answer,
        Search,
        Agent,
        DeepReasoning
    };

    Action action;
    int latency_budget_ms;
    bool allow_background;
    bool requires_memory;
    bool requires_search;
};
```

## 36.3 Speech Plan

```cpp
struct SpeechSegment {
    std::string text;

    float rate;
    float pitch;
    float energy;
    float warmth;

    int pause_before_ms;
    int pause_after_ms;

    std::vector<std::string> tags;
};
```

## 36.4 Agent Task

```cpp
struct AgentTask {
    std::string id;

    enum class Priority {
        Foreground,
        Background,
        Speculative
    };

    enum class State {
        Queued,
        Running,
        Paused,
        Completed,
        Cancelled,
        Superseded,
        Failed
    };

    Priority priority;
    State state;
};
```

---

# 37. 第一阶段不要一次性完成全部功能

建议在现在已有 M0-M8 基础上，增加一个“上层 Runtime”路线。

## R0 — Orchestrator 骨架

优先建立：

```text
EventBus
Task
Cancellation
Supersede
Foreground / Background / Speculative
WorkingContext
```

### 验收

可以同时跑：

```text
foreground response
+
background search
```

并且互不阻塞。

---

## R1 — 真正稳定的 Full Duplex

重点：

- AEC
- VAD
- Turn Detection
- Barge-in
- cancellation
- backchannel filtering

验收：

```text
Agent 讲话时用户可以随时打断
Agent 不因为用户短暂停顿抢话
“嗯 / 对 / 好”不导致错误打断
```

当前仓库已经给出 `<250ms` 打断响应以及低误触发的硬指标，这一目标继续保持。citehttps://github.com/PT123123/talk2agent/blob/master/本地全双工语音Agent实施计划.md#L216-L225

---

## R2 — Streaming Pipeline

建立：

```text
Streaming ASR
    ↓
Streaming LLM
    ↓
Sentence/Clause segmentation
    ↓
Prosody Planner
    ↓
Streaming TTS
```

目标：

> 用户说完后尽快听到第一段真正有意义的话。

---

## R3 — Fast Response + Background Agent

加入：

```text
Quick Response
Background Task
Task lifecycle
Supersede
Progress utterance
```

这是从“语音聊天 Demo”升级到“个人 Agent”的关键一步。

---

## R4 — Model Router

加入：

```text
fast
normal
deep
agent
search
background
```

根据任务难度和 latency budget 动态切换。

---

## R5 — Context / Memory

完善：

```text
WorkingContext
ConversationContext
LongTermMemory
TaskContext
```

---

## R6 — Prosody / TTS

加入：

```text
SpeechPlan
ProsodyPlanner
Qwen3-TTS Adapter
Chatterbox Adapter
```

Kokoro 继续作为轻量 fallback。

---

## R7 — Online Strong LLM

增加：

```text
OpenAI-compatible remote provider
Tool calling
Search
Local Agent bridge
```

让在线强模型成为“大脑”，而本地 Agent 成为工具层。

---

## R8 — Replay + Long-running

最终做：

```text
Conversation Replay
Voice Trace
Latency benchmark
Interruption benchmark
1h / 4h / 8h stability test
```

---

# 38. 最终推荐架构

```text
                             USER
                              │
                         microphone
                              │
                              ▼
                 ┌─────────────────────────┐
                 │ Audio Realtime Layer    │
                 │ AEC / NS / VAD          │
                 └────────────┬────────────┘
                              │
                              ▼
                 ┌─────────────────────────┐
                 │ Streaming ASR           │
                 └────────────┬────────────┘
                              │
                              ▼
                 ┌─────────────────────────┐
                 │ Conversation            │
                 │ Orchestrator            │
                 └────────────┬────────────┘
                              │
             ┌────────────────┼──────────────────┐
             │                │                  │
             ▼                ▼                  ▼
        Fast Response    Context Manager     Model Router
             │                │                  │
             │                │        ┌─────────┼──────────┐
             │                │        │         │          │
             │                │      Local    Remote      Deep
             │                │      Model      LLM      Reasoning
             │                │        │         │          │
             └────────────────┴────────┴─────────┴──────────┘
                                      │
                                      ▼
                                Tool Router
                                      │
                     ┌────────────────┼────────────────┐
                     ▼                ▼                ▼
                   Memory           Search         Local Agent
                     │                │                │
                     └────────────────┼────────────────┘
                                      │
                                      ▼
                              Response Planner
                                      │
                                      ▼
                               Prosody Planner
                                      │
                                      ▼
                               Streaming TTS
                                      │
                                      ▼
                                   Speaker
                                      │
                                      └─────► AEC reference
```

---

# 39. 关键原则

## 原则 1：不要重新造一个 Local Agent

当前 `talk2agent` 的价值应该是：

> **把成熟的 Agent 能力包装进自然的实时语音交互。**

不要为了 Voice Agent 再重新做一套 Memory / Search / Tool / Coding Agent。

优先通过 adapter / protocol 接入现有能力。

---

## 原则 2：模型不是全部，Runtime 更重要

最终自然度很大一部分来自：

```text
什么时候说
什么时候不说
什么时候打断
什么时候等待
什么时候后台工作
什么时候升级模型
什么时候结束旧任务
什么时候恢复旧任务
```

这些是 Runtime 的职责。

---

## 原则 3：在线模型负责高层推理，本地 Agent 负责执行

推荐最终形成：

```text
Strong Online LLM
        ↓
    Tool Calling
        ↓
Local Agent / Memory / Search / Files
```

这样最灵活。

---

## 原则 4：不要为了“拟人”而随机插入 filler

不要：

```text
每次先说“嗯”
每次都说“让我想想”
随机 sleep
随机笑声
每句话强行情绪化
```

真正自然的方式是：

```text
有必要 → 回应
没必要 → 安静
需要处理 → 后台工作
用户打断 → 立即让路
```

---

## 原则 5：优先优化 Timing，再优化 Voice Quality

优化优先级建议：

```text
1. Turn-taking
2. Barge-in
3. First-response latency
4. Background Agent
5. Context
6. Model routing
7. Prosody
8. TTS voice quality
```

一个音质一般但 timing 正确的 Agent，实际交流体验往往会比音质极佳但总抢话/等待很久的 Agent 更自然。

---

# 40. 最终验收标准

## A. 对话

连续 30 分钟使用：

- 不频繁抢话
- 不频繁重复
- 不把“嗯/对/好”当成新问题
- 能维持当前话题
- 能自然换话题
- 能理解“那个”“刚才那个”等上下文引用
- 能使用 Working Context
- 能调用 Relevant Memory

## B. 打断

- Agent 可随时被用户打断
- 目标 `<250ms` 停止播放
- 不继续播旧答案
- 新问题成为 foreground
- 旧任务按策略 continue / pause / cancel / supersede

## C. 智能路由

- 简单问题不调用重模型
- 复杂问题能够升级
- 需要实时事实时自动 Search
- 需要个人上下文时自动 Memory
- 需要操作时自动 Local Agent

## D. 语音

- Streaming TTS
- Low first-audio latency
- 中文自然
- 支持 pause
- 支持 rate / pitch / energy
- 支持 emotion / prosody
- 至少两个 TTS backend

## E. 长时间稳定性

- 1h 连续运行
- 4h 连续运行
- 无明显 RAM 增长
- 无明显 VRAM 增长
- 无 thread leak
- 设备断连可恢复
- 网络异常可恢复
- 单个工具失败不拖死整个 Voice Runtime

---

# 41. 给 Coding Agent 的直接执行要求

请直接以当前 `talk2agent` 为基础实现，不要推倒重来。

执行时遵守：

```text
1. 先阅读现有 voice-agent/ 与当前 M0-M8 文档
2. 找出已有实现与本文的差距
3. 先设计接口，再实现
4. 每次只做一个小阶段
5. 每阶段都要可编译、可运行、可回退
6. 增加单元测试和 replay test
7. 每阶段记录 latency / interruption / failure metrics
8. 不要把后台 Tool/Agent 调用做成 blocking call
9. 不要把 TTS 和 Conversation Orchestrator 强耦合
10. 不要把所有问题都路由到最大模型
11. 不要用大量 random sleep 模拟真人
12. 用户打断永远优先于旧回答
```

建议每完成一个阶段：

```text
build
→ unit test
→ replay test
→ latency test
→ git commit
→ 更新 PROGRESS.md
```

---

# 42. 一句话版本

最终目标不是：

> “做一个更像真人的 TTS。”

而是：

> **做一个能长时间和用户交流、知道什么时候该说话和什么时候该闭嘴、能够随时被打断、能够边聊天边调用本地 Agent、能够根据问题难度动态切换模型，并且整个过程具有自然 timing / prosody 的 Voice Agent Runtime。**

这应该成为 `talk2agent` 接下来高于单纯“ASR → LLM → TTS”链路的主要升级方向。
