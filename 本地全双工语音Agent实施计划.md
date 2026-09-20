# 本地优先 · 全双工语音 Agent —— 完整实施计划（给执行 Agent）

> **本文件是给编码 Agent 的执行规格书。** 请严格按 M0→M8 顺序推进，每个阶段必须通过「验收标准」才能进入下一阶段。
> 未经确认不得更换第 2 节的选型、不得修改第 4 节的目录结构、不得违反第 13 节的禁令。

---

## 0. 执行规则（Agent 必读）

1. **按阶段提交**：每完成一个 M 阶段，`git tag mN-done` 并在 `PROGRESS.md` 追加一行（阶段 / 完成内容 / 验收结果 / 遗留问题）。
2. **先验收后推进**：验收不通过必须修复，禁止带着已知失败进入下一阶段。
3. **不要一次性写完整个系统**：一次只做一个阶段的改动，保证可编译、可运行、可回退。
4. **接口先行**：第 5 节的接口在任何实现之前落地为头文件，实现可以先用 stub，但签名不得改。
5. **日志**：每个模块用 `spdlog`，音频线程日志必须异步 (`spdlog::async_logger`)，禁止在音频回调里做同步 IO。
6. **测试数据**：在 `tests/audio/` 放至少 6 段真实录音（安静说话、嘈杂说话、思考停顿、附和词"嗯/对"、打断、电视背景人声）。

---

## 1. 目标与硬约束

### 1.1 功能目标

| 编号 | 目标 |
|---|---|
| G1 | 本地优先：断网可用，全部推理在本地 GPU/CPU，在线为可选降级 |
| G2 | 全双工：Agent 说话时用户可随时打断；用户说话时 Agent 不抢话 |
| G3 | 语音闭环：ASR（用户语音→文字）、TTS（Agent 文字→语音） |
| G4 | Agent 能力：工具调用（含联网搜索）、长期记忆（自动判定是否存储） |
| G5 | 在线接口预留：LLM/ASR/TTS/Search 均可切换本地或远端，接口同构 |

### 1.2 非功能指标（验收硬指标）

| 指标 | 目标值 |
|---|---|
| 端到端延迟（无工具，从说完到出声） | < 1200ms（P50），< 1800ms（P90） |
| 打断响应（用户开口 → Agent 音量降到 0） | < 250ms |
| 打断误触发（Agent 说话时，用户不说话被误判打断） | 0 次 / 10 分钟 |
| 抢话率（用户思考停顿时 Agent 抢答） | < 1 次 / 20 轮 |
| 附和词不打断（"嗯/对/好/ok"） | 100% 不打断 |
| 长稳 | 连续对话 30 分钟无内存/显存增长、无线程泄漏 |

### 1.3 运行环境

- OS：Linux (Ubuntu 22.04+) 优先；Windows/macOS 次之
- GPU：NVIDIA ≥12GB 显存（CUDA 12.x）；无 GPU 时 CPU 可跑但延迟不达标
- 语言标准：**C++20**；构建：CMake ≥3.20；编译器 GCC ≥11 / Clang ≥14 / MSVC 19.3x

---

## 2. 选型锁定（不得擅自更换）

| 层 | 项目 | 版本/模型 | 许可 | 集成方式 |
|---|---|---|---|---|
| 音频 I/O | `miniaudio`（单头文件） | 最新 master | public domain | 源码嵌入 `third_party/` |
| 回声消除 | `libwebrtc` AEC3（AudioProcessingModule） | 需自抽子模块 | BSD | 静态链接，仅编译 `modules/audio_processing/` |
| 降噪 | APM 内置 NS + 可选 `GTCRN`(ONNX) | — | 开源 | ONNX Runtime |
| VAD | `TEN-framework/ten-vad` | 16kHz, hop 160 | 开源 | C 动态库 + ONNX |
| VAD 复核 | `Silero VAD v5` (silero_vad.onnx) | — | MIT | ONNX Runtime |
| 轮次判定 | Pipecat `Smart Turn v3.2`（ONNX，8M 参数） | v3.2 | BSD-2（权重+训练代码开源） | ONNX Runtime，8s 窗口 |
| ASR（主） | `sherpa-onnx` + `SenseVoice`（中/英/日/韩/粤） | sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17 | Apache-2.0 | 官方 C++ API，CUDA EP |
| ASR（流式） | `sherpa-onnx` + 流式 `Paraformer` | streaming-paraformer-bilingual-zh-en | Apache-2.0 | 同上 |
| ASR（兜底） | `whisper.cpp` | large-v3-turbo GGUF | MIT | 链接 `libwhisper` |
| TTS（默认） | `sherpa-onnx` + `Kokoro-82M` | v1.0（含中文音色） | Apache-2.0 | `OfflineTts` |
| TTS（中文备选） | `sherpa-onnx` + `vits-melo-tts-zh_en` | — | MIT | 同上 |
| TTS（高音质，可选） | `CosyVoice 3 (0.5B)` | — | Apache-2.0 | **Python sidecar + HTTP** |
| LLM | `llama.cpp`（GGML_CUDA=ON） | 最新 b 版本 | MIT/Apache | 链接 `libllama`，或 `llama-server` OpenAI 兼容端点 |
| LLM 模型 | Qwen3-8B / Qwen3-4B-Instruct GGUF Q4_K_M | — | 各自许可 | GGUF |
| 结构化输出 | llama.cpp GBNF（`common_schema` IR） | b≥10934 | MIT | `llama_grammar` / `json_schema_to_grammar` |
| Embedding/Rerank | llama.cpp `/v1/embeddings`、`/reranking` | Qwen3-Embedding-0.6B / Qwen3-Reranker | MIT | HTTP |
| 存储 | `SQLite3` + `FTS5` + `sqlite-vec` | — | public domain | 源码嵌入 |
| 搜索（本地） | `SearXNG`（Docker，开 JSON API） | latest | AGPL-3.0（自托管自用） | HTTP JSON |
| 搜索（在线） | Tavily / Brave / Exa / Serper | — | 商业 API | HTTP，统一接口 |
| 工具协议 | MCP：`mcpp-project/mcpp`（C++20） | — | MIT | 链接 |
| 并发/工具 | 自研 SPSC ring、`spdlog`、`nlohmann/json`、`cpr` 或 `libcurl` | — | 开源 | 链接 |
| 远端传输（可选） | `libdatachannel` / `uWebSockets` | — | 开源 | 链接 |
| 重采样 | `speexdsp` resampler 或 `libsamplerate` | — | BSD / LGPL | 链接 |

**端到端语音模型（备选路线，不在主线）**：`Qwen3-Omni-30B-A3B`（Apache-2.0，原生全双工，首包 ~211ms）或 Kyutai `Moshi`/`Unmute`。需 Python + vLLM，仅在 A/B 验证阶段以 sidecar 接入，**不得并入 C++ 主线**。

---

## 3. 架构与线程模型

### 3.1 采样率与信号流

```
麦克风(设备原生,优先48k) ─► AEC3(48k, 参考=实际播放PCM) ─► NS ─┬─► downsample 16k ─► VAD / SmartTurn / ASR
                                                              │
TTS PCM(24k/48k) ─► resample到设备率 ─► 播放回调 ─► tap一份作为AEC参考(带硬件时间戳)
```

- **AEC 必须在设备原生采样率上做**，不要先降采样再 AEC。
- **AEC 参考信号必须是播放回调里实际写入的样本**，不是待播放队列里的样本。
- 参考与采集用同一时钟对齐；发生音频路由变化（插拔耳机/切换输出设备）必须重置 AEC。

### 3.2 线程划分（严格）

| 线程 | 职责 | 禁止 |
|---|---|---|
| `audio_thread`（`SCHED_FIFO`，最高优先级） | 采集 + AEC + NS + 播放 + tap 参考信号；10ms 帧 | **禁止**任何模型推理、文件 IO、内存分配、加锁、同步日志 |
| `vad_thread` | 消费 16k PCM → ten-vad → 事件 | 禁止阻塞等待 LLM |
| `asr_thread` | 流式/短句识别 | — |
| `turn_thread` | Smart Turn 语义端点判定 | — |
| `llm_thread` | llama.cpp 生成 + 工具调用 + 取消 | — |
| `tts_thread` | 合成 PCM 分块入播放队列 | 必须支持 cancel |
| `tool_thread`(池) | 搜索/HTTP/MCP 工具执行 | 必须支持超时与 cancel |
| `mem_thread` | 记忆抽取与写入 | 用独立小模型，避免抢主模型 |

线程间：PCM 用 **SPSC lock-free ring**；事件用 **EventBus（无锁队列 + 条件变量）**；禁止跨线程共享 `std::vector` 裸指针。

### 3.3 会话状态机

```
        ┌────────────────── barge_in 成立 ───────────────┐
        ▼                                                │
      IDLE ──vad_start──► LISTENING ──vad_stop──► EOU_PENDING
                              ▲                          │
                              │ "还在想/附和词"           │ turn=Complete 或 超时
                              └──────────────────────────┤
                                                         ▼
      INTERRUPTING ◄───── barge_in ──────  SPEAKING ◄── THINKING
            │                                  ▲            │
            └────────► LISTENING               └──── 首句TTS就绪
```

`SPEAKING` 期间 VAD/ASR 必须继续全速运行——这是全双工与"半双工+打断"的分界线。

---

## 4. 目录结构（不得擅自改动）

```
voice-agent/
├── CMakeLists.txt
├── PROGRESS.md                  # 每阶段进度记录
├── configs/
│   ├── agent.yaml               # 主配置（模型路径、端口、阈值）
│   └── policy.yaml              # 路由策略（local/remote/auto、隐私等级）
├── third_party/                 # miniaudio, webrtc-apm, sqlite, spdlog, json, mcpp...
├── models/                      # onnx / gguf（.gitignore）
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── types.hpp            # 全局类型：AudioFrame, TextChunk, Event
│   │   ├── event_bus.hpp/.cpp   # 无锁事件总线
│   │   ├── ring_buffer.hpp      # SPSC 环形缓冲
│   │   ├── orchestrator.hpp/.cpp# 会话状态机（核心）
│   │   ├── turn_policy.hpp      # 阈值与判定参数集中处
│   │   └── cancel_token.hpp     # 级联取消令牌
│   ├── audio/
│   │   ├── device.hpp/.cpp      # miniaudio 采集/播放
│   │   ├── aec.hpp/.cpp         # webrtc AEC3 封装
│   │   ├── resampler.hpp/.cpp
│   │   └── tap.hpp              # 播放参考信号 tap + 时间戳
│   ├── vad/
│   │   ├── vad_engine.hpp/.cpp  # ten-vad 主 + silero 复核
│   │   └── turn_detector.hpp/.cpp # Smart Turn ONNX
│   ├── asr/
│   │   ├── iasr.hpp
│   │   ├── asr_sherpa.hpp/.cpp
│   │   └── asr_whisper.cpp      # 可选
│   ├── tts/
│   │   ├── itts.hpp
│   │   ├── tts_sherpa.hpp/.cpp
│   │   └── tts_remote.hpp/.cpp
│   ├── llm/
│   │   ├── illm.hpp             # OpenAI 兼容抽象
│   │   ├── llm_llamacpp.hpp/.cpp
│   │   ├── llm_remote.hpp/.cpp
│   │   └── grammar.hpp          # GBNF / JSON schema
│   ├── agent/
│   │   ├── tool_registry.hpp/.cpp
│   │   ├── tools/               # search.cpp, memory.cpp, time.cpp, shell.cpp
│   │   ├── mcp_client.hpp/.cpp
│   │   └── agent_loop.hpp/.cpp  # 生成→工具→再生成循环，带轮次上限
│   ├── search/
│   │   ├── isearch.hpp
│   │   ├── searxng_provider.cpp
│   │   ├── tavily_provider.cpp
│   │   ├── brave_provider.cpp
│   │   ├── search_router.cpp    # 本地优先 + 回退 + 缓存
│   │   ├── rerank.cpp           # llama.cpp /reranking
│   │   └── fetcher.cpp          # 正文抽取 + SSRF 防护
│   ├── memory/
│   │   ├── schema.sql
│   │   ├── store.hpp/.cpp       # SQLite + FTS5 + vec
│   │   ├── extractor.hpp/.cpp   # 小模型结构化抽取（GBNF）
│   │   └── retriever.hpp/.cpp   # BM25 + 向量 + 时间衰减 + RRF
│   └── util/{config.hpp,log.hpp,http.hpp,hash.hpp}
├── tests/
│   ├── audio/                   # 6 段真实录音（必须齐）
│   ├── test_aec.cpp
│   ├── test_vad_bargein.cpp
│   ├── test_eou.cpp
│   ├── test_tools.cpp
│   └── bench_latency.cpp
└── scripts/
    ├── download_models.sh
    ├── run_searxng.sh
    └── run_llama_server.sh
```

---

## 5. 关键接口（先落地头文件）

```cpp
// ---------- core/types.hpp ----------
using Sample = int16_t;
struct AudioFrame { const Sample* data; size_t frames; int sample_rate; uint64_t hw_pts_us; };
using PcmSink = std::function<void(const AudioFrame&)>;

// ---------- core/cancel_token.hpp ----------
class CancelToken { // 级联取消：LLM / TTS / 工具 共用
public:
  bool cancelled() const;
  void cancel();                      // 传播给所有已注册 child
  void add_child(std::shared_ptr<CancelToken>);
  void on_cancel(std::function<void()> cb); // 清队列、stop generation、淡出
};

// ---------- audio/aec.hpp ----------
class AecProcessor {
public:
  void init(int sample_rate, int channels);
  void process_capture(Sample* buf, size_t frames);          // 就地消除回声
  void on_playback_tap(const Sample* buf, size_t frames, uint64_t hw_pts_us); // 参考信号
  void reset();                                              // 路由变化时调用
  double erle_db() const;                                    // 回声抑制量，用于残余回声判定
  double coherence() const;                                  // 相干性，高=仍是回声
};

// ---------- vad/vad_engine.hpp ----------
struct VadResult { bool speech; float prob; };
class VadEngine {
public:
  void push(const int16_t* pcm16k, size_t n);  // 必须喂 AEC 之后的音频
  VadResult last() const;
};

// ---------- vad/turn_detector.hpp ----------
enum class TurnState { Complete, Incomplete };
class TurnDetector {
public:
  void reset();
  void push(const int16_t* pcm16k, size_t n);  // 最多 8s 窗口
  std::optional<TurnState> evaluate();          // 10~100ms 内返回
};

// ---------- asr/iasr.hpp ----------
class IAsr {
public:
  virtual void accept(const AudioFrame&) = 0;
  virtual std::string partial() const = 0;
  virtual std::string finalize() = 0;   // 短句模型：整段识别
};

// ---------- tts/itts.hpp ----------
class ITts {
public:
  virtual void synthesize_stream(std::string text,
                                 std::function<void(std::vector<Sample>)> chunk_cb,
                                 std::shared_ptr<CancelToken> ct) = 0; // 必须支持取消
};

// ---------- llm/illm.hpp ----------
struct ToolDef { std::string name, description, json_schema; };
struct ToolCall { std::string id, name, arguments_json; };
class ILlm {
public:
  virtual void chat_stream(const std::vector<Message>&, const std::vector<ToolDef>&,
                           std::function<void(Token)>, std::shared_ptr<CancelToken>) = 0;
  virtual void stop() = 0;  // 必须真正停止生成并丢弃 KV
};

// ---------- search/isearch.hpp ----------
struct SearchHit { std::string title, url, snippet, content, published_at; double score; };
class ISearchProvider {
public:
  virtual std::string name() const = 0;
  virtual bool is_online() const = 0;
  virtual std::vector<SearchHit> search(const std::string& q, int topk,
                                        std::chrono::milliseconds timeout) = 0;
};
class SearchRouter { // 本地优先，失败回退在线；结果合并去重 + rerank + 缓存
public:
  std::vector<SearchHit> query(const std::string& q, SearchPolicy);
};

// ---------- memory/store.hpp ----------
struct MemoryItem { int64_t id; std::string type, subject, content;
                    double salience; int sensitivity; int64_t valid_from, valid_to;
                    int64_t superseded_by; };
class MemoryStore {
public:
  int64_t upsert(MemoryItem);                              // 新事实不删旧事实，标 superseded
  std::vector<MemoryItem> hybrid_search(const std::string& q, int k);
  void decay_and_archive();                                // 定期衰减 salience
};
```

**工具函数的 JSON Schema（GBNF 强制）**：`web_search(query, topk?, time_range?)`、`memory_save(...)`、`memory_query(q)`、`get_time()`。

---

## 6. 阶段计划

### M0 — 工程骨架（0.5 天）

**任务**
1. CMake 工程：C++20，引入 `third_party/`（miniaudio、spdlog、nlohmann/json、SQLite3、sqlite-vec）。
2. 实现 `core/types.hpp`、`ring_buffer.hpp`、`event_bus.hpp`、`cancel_token.hpp`、`util/log.hpp`、`util/config.hpp`。
3. `main.cpp` 打印配置 + 空转退出。
4. `scripts/download_models.sh`：下载 ten-vad、silero_vad.onnx、SenseVoice、Kokoro、Qwen3 GGUF。

**验收**：`cmake -B build && cmake --build build -j && ./build/voice-agent --config configs/agent.yaml` 正常退出 0；单测 `ring_buffer` 在 10 万帧单生产者单消费者下无丢帧无越界（ASan 干净）。

---

### M1 — 音频闭环 + AEC（最关键，1~2 天）

**任务**
1. `audio/device.cpp`：miniaudio 采集 + 播放，回调式，帧长 10ms。
2. `audio/tap.cpp`：在播放回调里把**实际写入**的样本复制给 AEC（带 `hw_pts_us`）。
3. `audio/aec.cpp`：封装 webrtc APM（`echo_cancellation` + `noise_suppression` + `high_pass`），采集侧 `ProcessReverseStream`(参考) → `ProcessStream`(就地)。
4. 可选：耳机路由检测 → `aec.reset()`。
5. `tests/test_aec.cpp`：播放粉噪/语音时，计算 ERLE。

**关键参数**
- 播放缓冲尽量小（目标 <60ms 的设备缓冲），否则打断后声音收不回。
- 若设备不支持 48k，用 resampler 统一到 APM 支持速率（16k/32k/48k）。

**验收**
- 播放 TTS 音频时，AEC 输出残余能量比输入低 ≥20dB（ERLE）。
- 只播放、人不说话：VAD（临时挂 silero）**不得**触发（0 次 / 5 分钟）。
- 播放同时人说话：AEC 输出中人声可闻，波形能量明显高于纯回声。
- ASan/TSan 干净。

---

### M2 — VAD + 打断探测（1 天）

**任务**
1. `vad/vad_engine.cpp`：ten-vad 主判定（16k）+ silero 复核（双通道）。
2. 打断判定链路（`orchestrator` 内 `check_barge_in()`），分层：
   - L1：AEC 后 VAD prob > 阈值
   - L2：连续 ≥ `barge_in_min_ms`(160ms) 为真
   - L3：`aec.coherence()` 低于阈值（排除残余回声）
   - L4：快速 ASR 文本不在附和词表 `{"嗯","对","好","是的","ok","uh-huh","继续","行"}` 或时长 >600ms
   - L5（可选）：声纹 embedding 匹配注册用户
3. 触发后：`CancelToken.cancel()` → LLM `stop()` + TTS 取消 + 播放 30~50ms 淡出 + 清空播放队列。
4. `tests/test_vad_bargein.cpp` 用 `tests/audio/` 六段录音离线回放验证。

**参数初值**（集中在 `turn_policy.hpp`）

| 参数 | 初值 |
|---|---|
| `vad_threshold` | 0.5（嘈杂 0.6~0.7） |
| `barge_in_min_ms` | 160 |
| `backchannel_max_ms` | 600 |
| `coherence_max` | 0.6（超过视为残余回声，不判定打断） |
| `fade_out_ms` | 40 |

**验收**
- 打断响应（从 `tests/audio/interrupt.wav` 人声起点到播放能量降至 -40dB）**< 250ms**。
- 10 分钟纯播放 + 环境噪声：误触发 **0 次**。
- 附和词录音 5 段：**0 次打断**。
- 电视背景人声录音：不触发（无声纹时允许降级为"需 L4 通过"）。

---

### M3 — 单轮链路 ASR→LLM→TTS（1~2 天）

**任务**
1. `asr/asr_sherpa.cpp`：SenseVoice 短句识别（优先），Paraformer 流式用于实时字幕/附和词判定。
2. `llm/llm_llamacpp.cpp`：链接 `libllama`，CUDA，流式 token 回调；实现 `stop()`。
3. `tts/tts_sherpa.cpp`：Kokoro 合成，**按句子/标点切分**流式入队；`CancelToken` 取消。
4. `main.cpp` 串成单轮：VAD 结束 → ASR → LLM → TTS → 播放。
5. `tests/bench_latency.cpp` 打点：vad_stop → asr_done → llm_ttft → tts_first → play_first。

**验收**：端到端 P50 < 1200ms（8B Q4 + 12GB GPU）；打点日志可见每一段耗时。

---

### M4 — 全双工状态机（1~2 天）

**任务**
1. `orchestrator.cpp` 完整状态机（第 3.3 节），事件驱动。
2. `turn_detector.cpp`：Smart Turn ONNX，输入最近 ≤8s 的 16k PCM，输出 Complete/Incomplete。
3. EOU 双阈值策略：
   - 静音 300~400ms → 触发 Smart Turn 判定
   - `Complete` → 进入 THINKING；`Incomplete` → 继续等
   - 静音 800~1000ms → **强制**进入 THINKING（防卡死）
   - 单轮最长 15~30s 强制结束
4. 抢话抑制：THINKING 完成但用户重新开口 → 丢弃本轮回复重新生成。
5. 打断时在跑的工具标 `background`，结果入上下文但**不播报**。

**参数初值**

| 参数 | 初值 |
|---|---|
| `min_speech_ms` | 220 |
| `eou_fast_ms` | 350（触发语义判定） |
| `eou_force_ms` | 900（强制兜底） |
| `max_utterance_ms` | 20000 |
| `smartturn_window_ms` | 8000 |

**验收**
- `tests/audio/thinking_pause.wav`（含 1.2s 思考停顿）：不抢话，正确在说完才响应。
- 20 轮真人对话：抢话 ≤1 次。
- 打断后重新提问：回复内容基于新问题，无旧回复残留。

---

### M5 — Agent：结构化输出 + 工具循环（1~2 天）

**任务**
1. `llm/grammar.hpp`：把工具 JSON Schema 转 GBNF，约束解码。
2. `agent/tool_registry.cpp`：注册 `web_search`、`memory_save`、`memory_query`、`get_time`；支持并行调用。
3. `agent/agent_loop.cpp`：生成 → 解析 tool_calls → 执行（带超时）→ 结果回填 → 再生成；**轮次上限 3**。
4. `agent/mcp_client.cpp`（可选，基于 `mcpp`）：连接外部 MCP server，把 tools 并入 registry。
5. 工具执行必须响应 `CancelToken`。

**验收**
- 模型输出 100% 可被解析为合法 JSON（100 次采样测试）。
- 工具轮次不超过上限；超时（>5s）自动返回"工具超时"文案继续对话，不卡死。
- 打断时正在跑的 HTTP 请求被取消且不播报旧结果。

---

### M6 — Search（1~2 天）

**任务**
1. `scripts/run_searxng.sh`：Docker 起 SearXNG，**必须** `search.formats: [html, json]`，`secret_key` 随机；仅监听 127.0.0.1。
2. `searxng_provider.cpp`：GET `/search?q=&format=json&language=zh-CN&pageno=1`，解析 `results[]`（title/url/content/score）。
3. 在线 Provider：Tavily / Brave / Exa / Serper，各自实现 `ISearchProvider`，默认禁用，靠 `policy.yaml` 打开。
4. `search_router.cpp`：本地优先 → 结果不足或失败才回退在线；并发 fan-out（最大并发 8）→ 超时丢弃（单查询 5s）→ URL 规范化去重（去 utm/trailing slash）+ 标题 simhash → `rerank.cpp`（llama.cpp `/reranking`）取 top 6~8。
5. `fetcher.cpp`：并发抓正文（Jina Reader 或本地抽取），单页 ≤64KB、5s 超时；**SSRF 防护**：禁止解析到内网/回环/链路本地地址，重定向后重新校验 IP。
6. 缓存表 `search_cache(query_hash, results_json, created_at)`：事实类 TTL 7 天、新闻 1 小时、实时类 5 分钟。
7. 触发策略：模型自主决定调用 `web_search`；辅以内含"最新/今天/最近/多少钱/查一下"的规则兜底。**禁止每句话都搜**。
8. 搜索时先播占位句（"我查一下"）。

**验收**
- 断网时 SearXNG 本地可用（或明确降级到本地索引，接口不抛异常）。
- 在线 Provider 未配置时不报错，本地链路完整。
- 10 个事实类问题：答案带引用且可点开验证 ≥8/10；缓存命中时响应 <50ms。
- SSRF 用例（指向 127.0.0.1 / 10.x / 169.254.x）全部被拦。

---

### M7 — Memory（1~2 天）

**任务**
1. `memory/schema.sql`：

```sql
CREATE TABLE memories(
  id INTEGER PRIMARY KEY, type TEXT, subject TEXT, content TEXT,
  sensitivity INT DEFAULT 0, salience REAL DEFAULT 1.0,
  valid_from INT, valid_to INT, superseded_by INT,
  created_at INT, access_count INT DEFAULT 0, last_access INT);
CREATE VIRTUAL TABLE memories_fts USING fts5(content, tokenize='unicode61');
CREATE VIRTUAL TABLE memories_vec USING vec0(embedding float[1024]);
```

2. 判定是否存储（两阶段）：
   - **快筛**：规则触发词 `我叫/我喜欢/记住/别忘了/我住在/我是/以后都/不要再`；或与已有记忆 embedding 相似度 >0.92 → 合并/更新。
   - **LLM 抽取**：用小模型（0.6B~4B，别占主模型显存）+ GBNF 强制输出
     `{"worth_saving":bool,"type":"profile|preference|episodic|semantic|procedural","content":str,"confidence":0~1,"sensitivity":0|1,"ttl_days":int,"supersedes":[id]}`
     → 仅 `worth_saving && confidence>0.7` 写入。
3. `retriever.cpp`：BM25(FTS5) + 向量 + 时间衰减 `salience*exp(-Δt/τ)`，RRF 融合 → 本地 rerank → 注入 3~8 条。关键词型查询上调 BM25 权重。
4. 分层注入：常驻 profile（始终在 system prompt）→ 检索命中 → 原始对话滑动窗口。
5. 冲突处理：新事实不覆盖旧事实，设 `valid_to` 与 `superseded_by`，保留可追溯。
6. 遗忘：`decay_and_archive()` 按 `access_count`/`last_access` 衰减 salience，低于阈值归档（移出检索域不删除）；每 N 轮把 episodic 摘要为 semantic。
7. 提供 `/memory` 命令：查看 / 删除 / 修正（**用户可改是刚需**）。
8. `sensitivity>=1` 的记忆：强制禁用一切在线 provider。

**验收**
- 跨会话（重启进程）能正确召回 20 条写入中的 ≥18 条。
- 重复陈述同一事实 3 次：不产生 3 条重复记录。
- "我之前说我在哪工作"类冲突查询：正确返回最新值且可追溯旧值。
- 敏感记忆存在时，发起在线搜索被拦截并提示。

---

### M8 — 在线接口预留 + 降级 + 打磨（1~2 天）

**任务**
1. `policy.yaml` 路由：`llm: local|remote|auto`、`search: local-first|local-only|remote`、`privacy: block-sensitive-egress`。
2. 所有远端实现 OpenAI 兼容：`/v1/chat/completions`（含 tools + stream）、`/v1/embeddings`、`/reranking`、`/v1/audio/transcriptions`、`/v1/audio/speech`。
3. 降级**必须对用户可见**（TTS 播报或 UI 提示"本地不可用，已切换云端"）。
4. 打点埋到 `spdlog` + 可选 OpenTelemetry：每一轮的各段耗时、打断次数、工具调用、搜索命中率。
5. 长稳测试：30 分钟对话，记录 RSS / 显存 / 句柄数 / 线程数曲线。
6. 写 `README.md`（快速开始）与 `PROGRESS.md` 汇总。

**验收**
- 断网 30 分钟：本地链路全程可用，无异常退出。
- 手动 kill 本地 llama-server：自动切远端并播报提示；恢复后切回本地。
- 30 分钟长稳：RSS 增长 <10%，无线程泄漏（`cat /proc/<pid>/status | grep Threads` 平稳）。

---

## 7. 全双工实现细则（最有难度，逐条落实）

1. **AEC 参考 = 实际播放样本**：在播放回调里 tap，而不是从待播队列取。
2. **延迟对齐**：参考与采集用硬件时间戳对齐；换设备/插耳机 → `aec.reset()`。
3. **双讲**：打断恰发生在双讲期。AEC3 会在双讲时冻结自适应；**不要**关闭该机制，也不要在双讲期强行拉高抑制强度（会把用户语音削掉）。
4. **判定顺序固定**：`AEC → VAD → 时长门限 → 相干性 → 附和词 → 声纹`，跳过任何一层都会退化。
5. **级联取消**（<100ms 内完成）：`LLM.stop()`（含丢弃 KV）→ TTS 合成取消 → 清空播放队列 → 40ms 淡出。**已写入声卡的 buffer 无法回收，只能靠淡出掩盖**。
6. **后台工具结果不播报**：打断时完成的工具结果写入上下文，等下一轮自然引用。
7. **抢话抑制**：THINKING 结束后若检测到用户重新开口，丢弃回复重新生成。
8. **最小响应间隔**：避免 ASR 抖动导致连续触发（建议 300ms）。

---

## 8. 测试素材清单（`tests/audio/` 必须齐全）

| 文件 | 内容 | 用途 |
|---|---|---|
| `quiet_speech.wav` | 安静环境正常说话 | ASR 基线 |
| `noisy_speech.wav` | 风扇/键盘噪声下说话 | VAD 阈值 |
| `thinking_pause.wav` | 句中 1.2s 思考停顿 | EOU 不抢话 |
| `backchannel.wav` | "嗯 / 对 / 好 / 是的" | 附和词不打断 |
| `interrupt.wav` | Agent 播报时用户插话 | barge-in 延迟 |
| `tv_background.wav` | 电视/旁人说话 | 误触发测试 |

素材必须**真实录制**（含回放 TTS 的真实回声链路），合成叠加的音频不能验证 AEC。

---

## 9. 配置规格

```yaml
# configs/agent.yaml
audio: { device_rate: 48000, proc_rate: 16000, frame_ms: 10, play_buffer_ms: 50 }
aec:   { enabled: true, ns: true, high_pass: true, reset_on_route_change: true }
vad:   { engine: ten_vad, threshold: 0.5,复核: silero }
turn:  { min_speech_ms: 220, eou_fast_ms: 350, eou_force_ms: 900, max_utterance_ms: 20000 }
barge: { min_ms: 160, backchannel_max_ms: 600, coherence_max: 0.6, fade_out_ms: 40 }
asr:   { provider: sherpa_sensevoice, model: models/sense-voice, lang: zh }
llm:   { provider: llamacpp, gguf: models/Qwen3-8B-Q4_K_M.gguf, ctx: 8192, gpu_layers: 99, temp: 0.7 }
tts:   { provider: sherpa_kokoro, voice: zf_xiaobei, speed: 1.0, stream_by_sentence: true }
search:{ default: searxng, searxng_url: "http://127.0.0.1:8080", online: [], timeout_ms: 5000, cache: true }
memory:{ db: data/memory.db, embed_model: models/Qwen3-Embedding-0.6B.gguf, topk: 6 }
policy:{ llm: local, search: local-first, privacy: block-sensitive-egress }
```

---

## 10. 验收矩阵（汇总）

| 阶段 | 关键指标 | 通过线 |
|---|---|---|
| M0 | 编译 + 环形缓冲 | ASan 干净 |
| M1 | ERLE、误触发 | ≥20dB、0 次/5min |
| M2 | 打断延迟、附和词 | <250ms、0 次 |
| M3 | 端到端 | P50 <1200ms |
| M4 | 抢话率 | ≤1/20 轮 |
| M5 | tool call 解析成功率 | ≥95% |
| M6 | 搜索引用正确率 | ≥8/10；SSRF 全拦 |
| M7 | 记忆召回、去重 | ≥18/20；无重复 |
| M8 | 长稳、降级可见 | RSS 增长 <10% |

---

## 11. 风险与禁令

### 禁令（违反即打回）

1. **禁止在音频回调线程做推理、IO、内存分配、加锁、同步日志。**
2. **禁止把待播放 PCM 当作 AEC 参考信号。**
3. **禁止跳过 AEC 直接把麦克风音频喂 VAD**（必然自激）。
4. **禁止每句话都触发搜索。**
5. **禁止绕过 `ISearchProvider` / `ILlm` / `ITts` 抽象直接写死某家 SDK。**
6. **禁止在没有 CancelToken 的情况下启动 LLM/TTS/工具任务。**
7. **禁止把 Memory 的向量/全文检索做成"全量塞进 prompt"。**
8. **禁止删除旧记忆**（只能标 `superseded_by` + `valid_to`）。

### 风险登记表

| 风险 | 表现 |  mitigation |
|---|---|---|
| AEC 参考对齐错 | 打断时识别崩坏、自激 | M1 专项验收；换设备 reset |
| 播放缓冲过大 | 打断后漏声 100~300ms | 播放缓冲 <60ms + 淡出 |
| EOU 过激进 | 用户一思考就被抢话 | 双阈值 + Smart Turn |
| GPU 争抢 | 音频抖动爆音 | VAD/Kokoro 放 CPU，显留给 LLM；或用 CUDA MPS |
| 搜索拖慢体感 | 用户干等 2s | 占位句 + 并发 + 缓存 + 轮次上限 |
| 记忆膨胀 | 检索质量下降 | 相似度去冗余 + TTL + 定期压缩 |
| 许合规 | Piper 部分版本 GPLv3；Fish Speech/GPT-SoVITS 为 Research License；SearXNG 为 AGPL | 只用第 2 节锁定项；商用前复核 |

---

## 12. 附录

### A. 模型下载（`scripts/download_models.sh`）

```bash
# VAD
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx
git clone https://github.com/TEN-framework/ten-vad.git        # 16kHz, hop 160/256
# ASR（中英日韩粤，短句极快）
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2
# ASR（流式，用于实时字幕/附和词）
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-paraformer-bilingual-zh-en.tar.bz2
# TTS
#   sherpa-onnx 模型页取 Kokoro v1.0（含中文音色）或 vits-melo-tts-zh_en
# LLM
huggingface-cli download Qwen/Qwen3-8B-Instruct-GGUF qwen3-8b-instruct-q4_k_m.gguf --local-dir models
huggingface-cli download Qwen/Qwen3-Embedding-0.6B-GGUF --local-dir models
huggingface-cli download Qwen/Qwen3-Reranker-0.6B-GGUF  --local-dir models
```

### B. 本地服务端启动

```bash
# llama.cpp（OpenAI 兼容端点，供 rerank/embeddings/远端回退共用）
./llama-server -m models/qwen3-8b-instruct-q4_k_m.gguf -c 8192 -ngl 99 -fa on --port 8080 \
               --embedding --reranking
# SearXNG（务必开 json）
docker run -d --name searxng -p 127.0.0.1:8080:8080 searxng/searxng
docker exec searxng sed -i 's/ formats:/ formats:\n  - json/' /etc/searxng/settings.yml && docker restart searxng
curl -s "http://127.0.0.1:8080/search?q=test&format=json" | jq '.results[:3]'
```

### C. 值得抄的开源参考

| 项目 | 抄什么 |
|---|---|
| `TEN-framework/TEN-framework`（C++ 核心） | 实时多模态编排骨架、扩展机制 |
| `kyutai-labs/unmute` | semantic VAD 时序（不打断用户） |
| `kyutai-labs/moshi` | 全双工 multi-stream 设计范式 |
| `huggingface/speech-to-speech` | `--mode local` / Realtime API 双形态；web search 工具实现 |
| `78/xiaozhi-esp32` + `xiaozhi-esp32-server` | 全双工状态机、AEC、MCP 工具、provider 抽象、打断时序 |
| `mudler/LocalAI` | C++ 引擎群（parakeet.cpp / LocalVQE AEC / TTS / 声纹 / 本地向量库） |
| `pipecat-ai/pipecat` | Smart Turn v3.2 ONNX 权重（BSD-2，可直接搬进 C++） |

> 必须自己实现、无现成 C++ 方案的三块：**AEC 参考对齐与双讲处理、打断的级联取消、EOU 语义端点**。这三块决定体验上限，请投入最多时间。
