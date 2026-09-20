# 本地优先 · 全双工语音 Agent 架构方案（C++ 为主 / GPU 加速 / 开源模型）

> 约束：本地优先、C++ 为主、能用 GPU、全双工（我不抢话 + 你可以打断我）、带 Tools（搜索）与 Memory。
> 下文给出分层选型、全双工判定链路、搜索与记忆的实现路径，以及在线接口的预留方式。

---

## 0. 一句话结论

**编排层自己用 C++ 写（状态机 + 事件总线 + 音频线程），推理层全部走「C++ 原生引擎 + ONNX/GGUF」：**
- 音频前后端：`libwebrtc AEC3`（回声消除，全双工的地基）+ `GTCRN/RNNoise`（降噪）
- VAD：`TEN VAD`（首帧快）或 `Silero VAD v5`
- 轮次判定（别抢话）：`Pipecat Smart Turn v3.2`（Whisper-Tiny+分类头，8M 参数，ONNX，BSD-2）或 `TEN Turn Detection`
- ASR：`sherpa-onnx` + `SenseVoice`（中文短句，快）／流式 `Zipformer/Paraformer`
- TTS：`sherpa-onnx` + `Kokoro-82M`（默认）／`CosyVoice 3`（中文音质）
- LLM/Agent：`llama.cpp`（CUDA/Vulkan，GBNF 结构化输出 + tool calling）
- Memory：`SQLite + FTS5 + sqlite-vec` 自研，需要图谱时以 sidecar 挂 `mem0` / `Graphiti`
- Search：`SearXNG`（本地默认，开 JSON API）+ 可插拔在线 Provider（Tavily/Brave/Exa/Serper），本地 rerank 用 `llama.cpp /reranking`

---

## 1. 总体架构

```
┌─────────────── 音频实时域（单一高优先级线程，10ms 帧，绝不阻塞）───────────────┐
│ PortAudio/miniaudio 采集 ──► AEC3(参考=实际播放PCM) ──► 降噪 ──► 环形缓冲      │
│        ▲                                                    │              │
│        │                                          ┌─────────┴─────────┐     │
│   播放回调(TTS PCM，可即时中断+淡出)                │ VAD(ten-vad)      │     │
└────────┼──────────────────────────────────────────┤ 说话人确认(可选)   │     │
         │                                          └─────────┬─────────┘     │
         │                                                    │ 事件           │
└────────────────────────────────────────────────────────────┼───────────────┘
                                                             ▼
┌──────────────── 会话编排域（C++ 状态机 + 事件总线，actor 模型）────────────────┐
│  StateMachine: IDLE → LISTENING → EOU_PENDING → THINKING → SPEAKING → IDLE    │
│                ↑                                     │                        │
│                └──────────── INTERRUPTED (barge-in) ──┘                        │
│  ├─ ASR Worker（sherpa-onnx）                                                 │
│  ├─ Turn Detector Worker（Smart Turn / 语义端点）                              │
│  ├─ LLM Worker（llama.cpp，tool call + GBNF JSON）                            │
│  ├─ Agent Loop：ToolRegistry（search / memory / shell / MCP client）           │
│  ├─ Memory Worker（抽取 → 判定 → SQLite 写入；检索注入）                        │
│  └─ TTS Worker（sherpa-onnx Kokoro，句子级流式）                               │
└──────────────────────────────────────────────────────────────────────────────┘
                     ▲                                    ▲
        OpenAI 兼容层（本地 llama.cpp server / 远端 API 同构）
                     │                                    │
        SearchProvider（SearXNG ↔ Tavily/Brave/Exa/Serper）
```

---

## 2. 技术选型总表

| 层 | 首选开源项目 | 许可 | C++ 集成方式 | 备注 |
|---|---|---|---|---|
| 音频 I/O | `miniaudio`（单头文件）或 `PortAudio`/`RtAudio` | MIT/public domain | 直接源码嵌入 | miniaudio 的回调式播放 = 打断可即时生效 |
| 回声消除 AEC | `libwebrtc` 的 `AEC3`（AudioProcessingModule） | BSD | 抽出 `modules/audio_processing/*` 静态链接 | 全双工刚需，见 §4 |
| 备选 AEC | `speexdsp` | BSD | 直接链接 | 轻量但弱于 AEC3 |
| 降噪 | `GTCRN` / `DPDFNet`（sherpa-onnx 内置）、RNNoise | 开源 | ONNX Runtime | 风扇/键盘噪声 |
| VAD | `TEN VAD`（16kHz，hop 160/256） | 开源 | 提供 C 动态库 + ONNX | 触发延迟约 12ms，比 Silero 更快、体积 306KB |
| VAD 备选 | `Silero VAD v5` | MIT | ONNX Runtime | 生态最成熟，<1ms/帧 |
| 轮次判定 | `Pipecat Smart Turn v3.2`（8M，ONNX） | BSD-2（权重+训练代码全开源） | ONNX Runtime，8s 窗口@16kHz | 判「说完了 vs 在想」，支持 23 语言 |
| 轮次判定备选 | `TEN Turn Detection`（Qwen2.5-7B 文本型） | 开源 | HTTP/gRPC sidecar | 文本型，需先有 ASR 转写 |
| 流式 ASR | `sherpa-onnx` + `Paraformer/Zipformer` streaming | Apache-2.0 | 官方 C++ API | 边说边出字，利于打断时的语义判定 |
| 短句 ASR | `sherpa-onnx` + `SenseVoice`（中/英/日/韩/粤） | Apache-2.0 | 官方 C++ API | 非流式但极快，带情感/事件标签 |
| ASR 兜底 | `whisper.cpp` | MIT | C++ 原生 / GGUF CUDA | large-v3-turbo 精度兜底 |
| TTS 默认 | `Kokoro-82M`（sherpa-onnx 内置） | Apache-2.0 | sherpa-onnx OfflineTts | 82M、~30–341MB、CPU 也能跑，音质好 |
| TTS 中文优选 | `CosyVoice 3 (0.5B)` | Apache-2.0 | ONNX/Python sidecar（HTTP） | 流式 ~150ms，9 语言 18+ 方言 |
| TTS 极轻量 | `Piper` | **注意：Rhasspy 原版 MIT，现维护版 GPLv3** | sherpa-onnx | 边缘设备/树莓派 |
| TTS 克隆 | `Fish Speech S2` / `GPT-SoVITS` | **Research License，商用需审查** | sidecar | 音色克隆 |
| LLM 推理 | `llama.cpp`（CUDA/Vulkan/ROCm） | MIT/Apache | 直接链接 `libllama` 或起 `llama-server`（OpenAI 兼容） | 支持 GBNF 约束、tool calling、`/reranking`、`/v1/embeddings` |
| LLM 模型 | Qwen3 系列（4B/8B/30B-A3B）、gpt-oss、Llama | 各自许可 | GGUF | 中文 + tool call 能力优先选 Qwen3 |
| 结构化输出 | llama.cpp GBNF（b10934 起 `common_schema` IR） | MIT | `json_schema_to_grammar` | 强制合法 JSON，工具调用/Memory 判定全靠它 |
| Embedding / Rerank | llama.cpp `/v1/embeddings`、`/reranking` | MIT | HTTP | Qwen3-Embedding / Qwen3-Reranker 的 GGUF |
| 向量/全文存储 | `SQLite` + `FTS5` + `sqlite-vec`（或 `usearch`、`hnswlib`） | public domain / MIT | 源码嵌入 | 纯本地、零服务，最契合本地优先 |
| Memory 高级方案 | `mem0`（Apache-2.0）、`Graphiti`（时序知识图谱）、`Letta`、`Cognee` | 开源 | Python sidecar + gRPC | 需要图谱/时间推理时再上 |
| 搜索（本地） | `SearXNG`（Docker，`formats: [html, json]`） | AGPL-3.0（自托管自用无碍） | HTTP JSON | 免 key、无追踪、聚合 70+ 引擎 |
| 搜索（在线） | Tavily / Brave Search / Exa / Serper / SerpAPI / Bing CSE | 商业 API | HTTP | 统一抽象成 `SearchProvider` |
| 正文抽取 | Jina Reader（r.jina.ai）、本地 `gumbo`+自研抽取、或 trafilatura sidecar | — | HTTP / C++ | 抓正文再进上下文 |
| 工具协议 | MCP（`mcpp` C++20 SDK / `gopher-mcp` C++ SDK） | MIT 等 | 直接链接 | 第三方工具生态的标准化入口 |
| 并发/日志 | `folly` 或自研 SPSC ring；`spdlog`；OpenTelemetry C++ | 开源 | 链接 | 音频线程与推理线程必须隔离 |
| 传输（可选远端） | `libdatachannel`（WebRTC） / WebSocket(++ / uWebSockets) | 开源 | 链接 | 若要手机/浏览器远程接入 |

**端到端语音模型（备选路线 B）**：`Qwen3-Omni-30B-A3B`（Apache-2.0，原生端到端，首包 ~211–234ms，支持 function call）天然全双工，但需 vLLM/transformers，属于 Python 侧；
`Kyutai Moshi/Helium`（Mimi codec）同理。可与 C++ 主程序以 WebSocket/gRPC 组成 sidecar，作为「路线 B」并行验证。

---

## 3. 全双工的核心：状态机 + 两条判定链路

### 3.1 状态机

```
        ┌──────────── barge-in 成立 ────────────┐
        ▼                                       │
     IDLE ──VAD起──► LISTENING ──VAD止──► EOU_PENDING ──"说完了"──► THINKING
                        ▲                       │"还在想/附和词"           │
                        └───────────────────────┘                         ▼
                                                                     SPEAKING
                                                                          │ 播完/被中断
                                                                          ▼
                                                                        IDLE
```

- `SPEAKING` 期间**麦克风与 VAD 必须继续全速运行**——这是「全双工」与「半双工+打断」的分界线。
- `INTERRUPTED` 是一个瞬时事件而非稳定状态：触发后立刻执行级联取消，再回 `LISTENING`。

### 3.2 链路 A：你可以打断我（barge-in）

判定必须**分层**，每层买来不同的可靠性：

1. **AEC 后仍有语音**：AEC3 输出（已扣除 TTS 回声）→ ten-vad 概率 > 阈值
2. **持续时长门限**：连续 150–250ms 为真（过滤咳嗽、键盘、单音节）
3. **不是残余回声**：看 AEC 的 ERLE / 参考-误差相干性；相干性高说明还是回声，判定为「未打断」
4. **不是附和词（backchannel）**：把这段音频快速 ASR，命中 `嗯/对/好/是的/继续/ok/uh-huh` 且时长 <600ms → **不停**
5. **说话人确认（可选但推荐）**：speaker embedding 与注册声纹比对，避免电视/旁人触发

满足后执行**级联取消**（全部在 <100ms 内完成）：
- `llama.cpp` 取消生成（stop generation）→ 丢弃已生成但未播报的文本
- TTS 合成线程取消 → 清空 PCM 队列
- 播放端 30–50ms 淡出后停（防爆音），**只清播放队列，不清已写入声卡的 buffer**（硬件延迟无法回收，需靠淡出掩盖）
- 正在跑的工具：标记为 `background`，结果进上下文但**不播报**，等下一次用户说话再自然带出

### 3.3 链路 B：我不抢话（不误判 EOU）

用户停顿 ≠ 说完。参数要给保守值，再用语义模型兜底：

| 参数 | 建议值 | 说明 |
|---|---|---|
| `min_speech_ms` | 200–250 | 起始去抖 |
| `min_silence_ms`（快速判定） | 300–400 | 触发语义判定，不直接发言 |
| `min_silence_ms`（保守兜底） | 800–1000 | 超时强制进入响应 |
| `max_utterance_ms` | 15000–30000 | 防长篇 |
| 语义端点 | Smart Turn v3.2，输出 `Complete / Incomplete` | 判「说完了 vs 在想」，10–100ms 级 |
| 附和词过滤 | 转写命中列表且 <600ms | 视为仍在听 |

补充策略：
- **抢话抑制窗口**：Agent 生成完毕但用户又开始说话 → 丢弃本轮回复，重新生成（避免"抢"）。
- **think-then-speak**：`THINKING` 阶段若用户继续补充，合并进同一轮请求（把新转写 append 到 prompt）。
- **最小响应间隔**：避免 ASR 抖动导致的连续触发。

---

## 4. AEC：全双工最容易被低估的坑

1. **参考信号必须是「实际播放出去的 PCM」**，不是「准备播放的 PCM」。在播放回调里把真正写入的样本复制一份进 ring buffer 作为 AEC 参考，并记录硬件时间戳。
2. **延迟对齐**：播放缓冲 → DAC → 空气路径 → ADC → 采集缓冲，整体可能 20–200ms；AEC3 自带 delay estimation，但**换音频设备/插耳机时会失锁**，需要检测 route change 并重置。
3. **双讲（double-talk）**：正是打断发生的那一刻。AEC3 用相干性检测在双讲时冻结滤波器自适应，否则滤波器会把人声当回声学坏——表现为「打断时前 0.5 秒识别最差」。
4. **耳机场景**：回声路径近乎为零，可动态关闭 AEC 以降低损伤。
5. **验证方法**：必须专门测「双方同时说话」，只测礼貌轮流对话会漏掉所有真问题。

---

## 5. 延迟预算（目标值）

| 阶段 | 目标 | 手段 |
|---|---|---|
| VAD 首帧触发 | ~12–30ms | ten-vad |
| 语义端点判定 | 50–150ms（与 ASR 并行） | Smart Turn |
| ASR（短句） | 100–300ms | SenseVoice / 流式增量 |
| 工具判断（是否搜索） | 0–200ms | 小模型分类或规则 |
| LLM TTFT | 200–500ms | 7B Q4 + CUDA，小 KV、prompt 缓存 |
| 搜索（命中缓存） | <50ms | SQLite 缓存 |
| 搜索（未命中） | 800–2500ms | 并行子查询 + 抓取 + rerank（**先说"我查一下"占位**） |
| TTS 首包 | 100–300ms | Kokoro / CosyVoice 流式 |
| **端到端（无工具）** | **700–1200ms** | |
| **打断响应** | **<250ms** | AEC+VAD 直通，绕过 LLM |

工程抓手：TTS 按**句子/子句切分流式**（第一句先播），LLM 首句优先，搜索时先播报占位句（"让我查一下"）——体感延迟比绝对延迟更重要。

---

## 6. Search 功能怎么实现（重点）

### 6.1 抽象接口（预留在线能力的关键）

```cpp
struct SearchHit { std::string title, url, snippet, published_at; double score; };
struct SearchQuery { std::string q; int topk=8; std::string lang="zh-CN";
                     std::optional<std::string> time_range, site; };

class ISearchProvider {
public:
  virtual std::string name() const = 0;
  virtual bool online() const = 0;            // 是否需要外网
  virtual std::vector<SearchHit> search(const SearchQuery&,
                                        std::chrono::milliseconds timeout) = 0;
};
// 实现：SearxngProvider（本地默认）/ TavilyProvider / BraveProvider /
//       ExaProvider / SerperProvider / BingCseProvider / LocalIndexProvider(RAG)
```

注册进 `SearchRouter`：**本地优先，失败/不足才回退在线**，并按隐私等级决定是否允许外发。

### 6.2 本地默认：SearXNG

```bash
docker run -d --name searxng -p 8080:8080 searxng/searxng
# settings.yml 必须开：search.formats: [html, json]   ← 漏了会 403
curl -s "http://localhost:8080/search?q=test&format=json" | jq '.results[:8]'
```
- 引擎选择：general（Bing/DDG/Brave/Startpage）+ `science`/`it` 分类按需；本地自用可关 limiter，但不要暴露公网。
- 优点：免 API key、无追踪、无限流；缺点：结果质量受上游波动影响，需自己做 rerank。

### 6.3 在线 Provider（预留）

| Provider | 特点 |
|---|---|
| Tavily | 为 Agent 设计，直接返回**已抽取正文 + 摘要**，省抓取环节 |
| Brave Search | 独立索引、性价比高、有 freshness 参数 |
| Exa | 语义/神经检索，适合「找类似的东西」 |
| Serper / SerpAPI | Google 结果 |
| Jina Search / Reader | 搜索 + 正文抽取一体 |

统一返回同一 `SearchHit` 结构，`SearchRouter` 做：并发 fan-out → 超时丢弃 → 合并去重 → rerank。

### 6.4 后处理流水线（决定搜索质量的关键，别省）

1. **查询改写**：LLM（GBNF 约束）生成 3–5 个子查询 / 关键词变体，并行搜索
2. **去重**：URL 规范化（去 utm/trailing slash）+ 标题 simhash
3. **过滤**：域名黑名单、时效性（新闻类只要 7 天内）、语言
4. **Rerank（本地）**：`llama.cpp /reranking` + Qwen3-Reranker，取 top 6–8
5. **抓取正文**：Jina Reader 或本地抽取器；**限大小（≤64KB）、限超时、限并发**
6. **入上下文**：只放清洗后的正文片段 + `[id]` 编号，要求模型回答带引用
7. **充分性判定**：LLM 判「够不够」→ 不够则改写查询再来一轮（≤2–3 轮，硬上限）

### 6.5 触发策略与工程细节

- **不要每句话都搜**：交给模型自己决定调用 `web_search` 工具；加规则兜底（含"最新/今天/最近/多少钱"等）。
- **缓存**：SQLite 表 `search_cache(query_hash, results_json, created_at)`，事实类 TTL 7 天、新闻类 1 小时、股票/天气 5 分钟。
- **安全**：SSRF 防护（禁止解析到内网 IP / 域名重解析校验）、遵守 robots.txt、抓取超时 5s、最大并发 8。
- **隐私路由**：对话被 Memory 模块标记为 `sensitive` 时，强制只用本地 Provider。
- **离线降级**：SearXNG 不可用时，退化到本地索引（`LocalIndexProvider` 走同一接口接 RAG），保证接口不中断。

---

## 7. Memory：存什么、怎么判定、怎么检索

### 7.1 判定「要不要存」——两阶段

**阶段 1：快筛（零成本）**
- 规则触发词：`我叫 / 我喜欢 / 记住 / 别忘了 / 我住在 / 我是 / 以后都 / 不要再`
- 实体识别：出现人名、地名、时间、偏好词
- 与已有记忆 embedding 相似度 > 0.92 → **合并或更新**，不新增

**阶段 2：LLM 结构化抽取（每轮结束或每 N 轮批量跑）**
用 llama.cpp GBNF 强制输出：

```json
{
  "worth_saving": true,
  "type": "profile|preference|episodic|semantic|procedural",
  "subject": "user",
  "content": "用户偏好在广州，不喜欢被打断",
  "confidence": 0.9,
  "sensitivity": 0,        // ≥1 表示禁止上云
  "ttl_days": 3650,
  "supersedes": [1234]     // 覆盖哪些旧记忆
}
```
只在 `worth_saving=true && confidence>阈值` 时写入；小模型（0.6B–4B）足够做这件事，别用主模型抢 GPU。

### 7.2 存储（本地优先，纯 C 栈）

```sql
CREATE TABLE memories(
  id INTEGER PRIMARY KEY, type TEXT, subject TEXT, content TEXT,
  sensitivity INT DEFAULT 0, salience REAL DEFAULT 1.0,
  valid_from INT, valid_to INT, superseded_by INT,
  created_at INT, access_count INT, last_access INT);
CREATE VIRTUAL TABLE memories_fts USING fts5(content, tokenize='unicode61');
-- 向量：sqlite-vec 的 vec0 虚表（或 usearch 独立索引）
```
- **时间维度**照搬 Graphiti 的思路：`valid_from / valid_to / superseded_by`，新事实不删旧事实，只标失效——这样「我之前是怎么说的」可追溯。
- **分层注入**：常驻 profile（始终在 system prompt）→ 检索命中的语义记忆（top-k）→ 原对话原文（滑动窗口）。

### 7.3 检索

混合检索 = `FTS5 BM25` + `向量相似度` + `时间衰减 salience * exp(-Δt/τ)`，用 **RRF 融合**再过一遍本地 rerank，最终注入 3–8 条。
纯关键词查询（人名/专有名词）BM25 权重上调，语义查询向量权重上调。

### 7.4 遗忘与压缩
- 每 N 轮：把旧的 episodic 摘要为 semantic 条目，原文不保留
- salience 随 `access_count` 与 `last_access` 衰减，低于阈值归档（不删，移出检索域）
- 提供 `/memory` 命令：查看、删除、修正（**用户可改是刚需**）

### 7.5 什么时候上 sidecar
需要时序图谱、多跳关系、跨会话复杂推理时，再以 Python sidecar 挂 `mem0`（生态最大，Apache-2.0）或 `Graphiti`（时序知识图谱，时间是一等公民）。C++ 侧通过 gRPC 调用，保持主程序干净。

---

## 8. 在线接口的预留方式

原则：**所有外部依赖都收敛到一个 OpenAI 兼容的抽象层**，本地与远端同构，切换只改配置。

| 能力 | 本地实现 | 远端预留 | 接口 |
|---|---|---|---|
| LLM | llama.cpp / llama-server | DeepSeek / Qwen / OpenAI / OpenRouter | OpenAI `/v1/chat/completions`（含 tools + stream） |
| Embedding | llama.cpp `/v1/embeddings` | 同上 | `/v1/embeddings` |
| Rerank | llama.cpp `/reranking` | Cohere / Jina | `/reranking` |
| ASR | sherpa-onnx / whisper.cpp | Groq Whisper / 各家 | 自定义 `IASRProvider` |
| TTS | sherpa-onnx Kokoro | 各家 | `ITTSProvider`（**必须支持流式 + cancel**） |
| Search | SearXNG | Tavily / Brave / Exa / Serper | `ISearchProvider` |
| Tools | 本地函数 + MCP client | 任意 MCP server | MCP（C++ SDK：`mcpp`、`gopher-mcp`） |

路由策略：`policy.yaml` 定义 `llm: local | remote | auto(fallback)`、`search: local-first`、`privacy: block-sensitive-egress`。
**降级必须在 UI 上可见**（本地失败转云端时提示），否则"本地优先"是假的。

---

## 9. 线程与 GPU 资源规划

- **音频线程**：`SCHED_FIFO`/高优先级，10ms 帧，只做采集 + AEC + 播放，**不做任何推理**（一次推理抖动 = 爆音）。
- **推理 worker**：ASR / Turn / LLM / TTS 各一线程（或线程池），通过 SPSC ring buffer 与音频线程交换 PCM，零锁。
- **GPU 争抢**：VAD/小 ASR/TTS 用 ONNX Runtime CUDA EP，LLM 用 llama.cpp CUDA；同卡多流建议开 `CUDA MPS`，或把 VAD + Kokoro 放 CPU（它们 CPU 也很快），把显存留给 LLM。
- **显存预算**（12–16GB 档）：LLM 7B Q4 ≈ 4–5GB，SenseVoice <1GB，Kokoro ~1GB，embedding 0.6B ≈ 0.6GB —— 够用；上 30B MoE 需要 24GB+。

---

## 10. 里程碑

| 阶段 | 交付 | 验收标准 |
|---|---|---|
| M0 | 音频闭环：采集 + AEC + VAD + 播放 | 播放 TTS 时说话，AEC 后 VAD 能检出（无自激误触发） |
| M1 | 单轮：ASR → llama.cpp → TTS | 端到端 <1.5s |
| M2 | **全双工**：打断 + 不抢话（§3 两条链路） | 打断响应 <250ms；"嗯/对"不打断；思考停顿不抢话 |
| M3 | Agent：ToolRegistry + GBNF 结构化 + 并行/取消 | tool call 成功率 >95%；打断能取消工具 |
| M4 | Search：SearXNG + 在线 Provider + rerank + 缓存 | 引用正确、可离线降级 |
| M5 | Memory：抽取判定 + 混合检索 + 冲突管理 + 用户可改 | 跨会话召回正确，不重复存 |
| M6 | 打磨：延迟、日志/追踪、配置化 provider 切换 | 长对话稳定 30min+，无显存/线程泄漏 |

---

## 11. 风险与坑清单（按踩坑概率排序）

1. **AEC 参考信号对齐错** → 打断时识别崩坏（最常见）
2. **播放 buffer 无法即时回收** → 打断后仍漏出 100–300ms 声音，必须用淡出 + 短播放缓冲（<60ms）
3. **把 TTS 自己的声音当打断** → 自激循环；必须先过 AEC 再过 VAD
4. **EOU 太激进** → 用户一思考就被抢话，体验比慢更糟
5. **每句都搜索** → 延迟爆炸；必须模型自主决策 + 占位话术
6. **Memory 无限膨胀** → 相似度去冗余 + TTL + 定期压缩，缺一不可
7. **许可合规**：Piper（GPLv3 vs MIT 版本要分清）、Fish Speech / GPT-SoVITS（Research License 商用需审查）、SearXNG（AGPL，自托管自用无碍）
8. **llama.cpp 取消生成必须真正生效**（stop generation + 丢弃 KV），否则打断后旧回复还会冒出来
