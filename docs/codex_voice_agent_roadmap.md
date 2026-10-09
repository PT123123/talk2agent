# talk2agent 本地语音 Agent 路线图（供编码 Agent 分阶段执行）

> 本文是后续实现计划，不代表本轮已实施代码。一次只执行一个阶段；开始前先检查当前分支、工作区、`voice-agent/PROGRESS.md` 和相关实现，保留用户现有改动。每阶段先给出计划和影响文件，完成后运行该阶段明确列出的验证并记录结果；不要重写架构或整批引入 LiveKit。

## 1. 目标和边界

目标是把现有 Windows / Qt / C++20 桌面程序 `voice-agent` 做成可靠的本地优先全双工语音 Agent：用户自然说话，Agent 能及时理解、调用工具、边生成边播报，并允许用户打断；复杂任务可以由后续编码 Agent 按本路线图逐阶段实现。LiveKit Agents 只作为成熟语音交互框架的设计参照。

默认数据路径仍是本机麦克风/扬声器 → VAD/ASR → Orchestrator/LLM/工具 → TTS/AudioRouter。在线模型和搜索是可选能力。本文不要求添加实时网络房间、LiveKit Server/Cloud、电话/SIP、WebRTC SFU、生成式视频或高成本 3D Avatar，也不要求替换 C++/Qt 主架构。语音仍是主要交互方式；低成本本地状态动画与可选 HTML/SVG 视觉展示纳入后续规划，且不得要求每轮调用图像/视频生成模型。端到端语音模型只可作为独立实验分支/适配器，必须与现有级联链路做可回退的实测比较后再决策。

## 2. 当前仓库基线（2026-10-07 检视）

仓库不是空壳：`voice-agent/README.md`、`PROGRESS.md` 和代码显示已具备 Qt GUI、AudioPipeline/AEC、Silero VAD、Paraformer/Whisper ASR、llama.cpp 本地 LLM 与可选远端 OpenAI 兼容 LLM、Kokoro/SAPI TTS、按句流式播报、工具循环、搜索/记忆、取消令牌、EOU/Smart Turn、打断上下文截断、策略/模型路由、JSONL VoiceTrace 和延迟指标。具体入口包括 `src/orchestrator/orchestrator.*`、`src/audio/audio_pipeline.*`、`src/vad/vad.*`、`src/asr/*`、`src/llm/llm.*`、`src/tts/*`、`src/agent/*`、`src/core/task.*`。

项目自己的《本地全双工语音Agent实施计划.md》早期 M0-M8 与现状不完全同步：PROGRESS 中已记录 R1-R9 等后续实现，M7 MCP、M8 优化仍待开始。以代码和 PROGRESS 的近期记录为当前事实，旧计划的模型/硬件假设不可直接照搬。配置默认是 48kHz 音频、VAD 220ms 最短语音/300ms 静音、打断最短 160ms、EOU fast/force 350/900ms；这些是参数初值，不是已经在目标机器上测得的体验保证。

已知约束必须带入后续决策：目标电脑为 Windows/Intel Arc 时显存资源有限；PROGRESS 记录 Kokoro 经 DirectML 在该 Intel Arc 上会崩溃，当前回退 CPU 合成。R8 的 barge-in 延迟测点从 VAD 检出开始，可能低估用户真实开口至静音时间；真实设备长跑、远端连接和真人听感尚未覆盖。不要把文档中计划的 Linux/CUDA/Qwen3 模型或早期延迟目标当成现状。

开始每阶段时核对上述基线，尤其留意用户工作区已经存在的修改。检视时发现的既有未提交项包括 `voice-agent/Justfile`、若干 `src/audio`/`src/gui` 文件和 `voice-agent/scripts` 下两个未跟踪文件；不得覆盖、清理或顺手格式化这些工作。

## 3. LiveKit 对照：借什么、不借什么

LiveKit Agents 是以 Python/Node.js 为主的实时 Agent 框架，面向 LiveKit room 中的服务器参与者；它提供 STT-LLM-TTS 与 realtime-model 两类管线、 turn handling、interruptions、pipeline 节点/生命周期钩子、异步工具、MCP、多 Agent handoff、测试与可观测性。talk2agent 的目标是本地桌面 C++ 运行时，已经有不少相同问题的本地实现，因此应借鉴其**职责划分和行为约束**，而不是复制 SDK、房间模型或云服务依赖。

| LiveKit 机制/能力 | talk2agent 的对应点 | 采用方式 |
|---|---|---|
| 轮次检测：VAD + endpointing + 语义 turn detector；检测模式可按延迟/语言/模型能力选择 | `EOUDetector`、`SmartTurn`、`SemanticTurnDetector` | 对照其动态等待与不同模式的边界；保持可配置的本地规则默认值，先修正阈值生命周期和中英文验证，再评估小模型；不直接拉入云推理检测器 |
| 自适应打断过滤附和词；误打断恢复；打断时保留实际已播内容 | `AudioPipeline`、AEC、`TurnAdmission`、`AudioRouter`、`InterruptionTruncation` | 延续真实播放游标截断和取消链；用数据验证误报/漏报。LiveKit 的自适应门控使用其 Cloud 模型，不能视为本地可直接使用的依赖 |
| STT、LLM、TTS 节点和 lifecycle hooks | 现有各模块接口与 Orchestrator callback | 明确模块输入输出、取消、阶段事件和替换边界；新能力优先以 adapter/接口扩展，避免把 GUI、模型和轮次状态绑在一起 |
| Preemptive generation（最终轮次确认前投机启动） | `Orchestrator`、partial ASR、取消令牌 | 可在稳定 partial-transcript 之后作为实验优化；必须校验最终转写变化会取消旧生成/旧 TTS，比较节省的首音频延迟与浪费算力，不作为前置必做 |
| 异步工具与进度更新 | `AgentLoop`、`TaskManager`、`FastResponseLayer` | 让慢工具有超时、取消、去重/supersede 和用户可感知状态；优先复用现有任务与快速回应机制，不重复造任务池 |
| MCP 工具与 Agent handoff | `ToolRegistry`/AgentLoop；PROGRESS 中 MCP 为待做 | 先完成受控 MCP client 接入；多 Agent 暂作接口/规划，不因示例而拆成多个 Agent。需要复杂任务角色切换时再设计 handoff 与共享对话上下文 |
| 模拟会话测试、事件断言、评测与 traces | 当前 C++ tests、VoiceTrace、LatencyMetrics | 把用户可观察行为变成离线确定性用例和真实音频评估集；自动测工具/状态/截断，真人听感和硬件延迟单列人工/设备验收 |
| WebRTC、房间调度、SIP、云噪声取消、LiveKit Inference | 本地 miniaudio/Qt 桌面管线 | 暂不采用；仅当产品边界转为远端多端实时通话时重新评估。AEC/NS 则按本地设备实测选择本地组件 |

## 4. 分阶段路线图

阶段应顺序推进。每一阶段均需保留旧行为可回退；若真实用户体验数据与文档数值冲突，应先记录基线和原因，再调目标，不为达成指标而牺牲中文轮次完整性。

### 阶段 A：基线收敛与现状核实（先做）

**工作项**
- 梳理 M0-M8 与 R1-R9/实际代码的差异，补齐具体模块的真值表：可用后端、模型路径、mock/fallback 条件、设备采样率、当前测试覆盖、明确遗留。
- 复核从采集时间戳、VAD、ASR partial/final 到 EOU、LLM、TTS 分句、实际播放、打断截断的事件链；列明回调线程和锁边界。
- 检查 `SemanticTurnDetector` 对 force_ms 的临时修改在新轮/超时/连续不完整话语等所有结束路径是否恢复；检查关闭 TTS、重复打断、旧任务回调和路由变化路径。
- 建立真实体验基线：用户开口至播放静音、EOU 时长、partial/final ASR 差异、首 token/首音频、CPU/GPU/内存、打断后已听/未听内容同步。指标名称和测点先统一。

**验收**：新增一份可复核的基线记录或在本路线图 PR 中附上表格；每项指标写清楚测点、采集方式、硬件/模型。状态恢复路径有代码级测试或明确缺口；不把现有 PROGRESS 中的单元测试数字误写成硬件实测。

### 阶段 B：轮次与打断可靠性（高优先）

**工作项**
- 把轮次判定调参集中到配置/策略对象，明确 fast endpoint、force endpoint、最大话轮、partial transcript 稳定度和语言/模式的关系；让每轮/每次重置恢复基准阈值，避免放大后残留。
- 分开记录“检测到声学活动”“确认是有效打断”“播放停止”“当前句上下文已截断”时间点。AEC 参考采用实际输出样本；设备/路由变化应重置回声处理状态。
- 核查打断取消链覆盖 LLM、TTS 合成、排队音频和工具结果；旧一轮迟到 callback 不得污染新一轮；重复打断应幂等。
- 针对中文停顿、句中犹豫、短附和词、连续附和后补充内容、电视/背景声、Agent 自身回声建立离线用例；误打断不能简单用“短文本白名单”一项解决。
- 继续以实际已播放音频而不是已生成/已入队文本截断会话历史。若 ASR/TTS 可提供时间戳，规划更精确对齐；字符/句级估算的误差必须可见。

**验收**：新增/更新确定性用例覆盖正常结束、不完整延长后完成、最大时长兜底、新一轮恢复默认、打断发生于 TTS 合成/排队/播放阶段、连续打断和迟到回调；离线音频回放报告有效打断 precision/recall、附和词误打断数及完整延迟分布。硬件验收需以实际开口为起点，明确设备、阈值、样本数；对比现有 250ms 目标，不得用 VAD 时间冒充真实响应时间。

### 阶段 C：ASR/TTS 适配与中文流式体验

**工作项**
- 不预设增加模型就是优化。比较 Paraformer 与 Whisper 等当前 ASR 后端在项目自己的中英文短句、长句、停顿/修正、专名语料上的实时率、字错/词错、partial 抖动、尾段延迟与硬件占用；只有量化优势明显才加新后端。
- 确认 ASR partial 只供界面/投机意图使用，final 才作为事实写入对话和工具参数；final 修订要能撤销对应投机生成。
- 将 TTS 合成首块时间、音频时钟/采样率转换、按句队列和播放游标放进同一链路契约。对于 Intel Arc Kokoro DirectML 已知崩溃，继续 CPU 回退；新候选（如适配 sherpa-onnx 的 VITS/Matcha/Piper）先做兼容性、音质、首块延迟、长句稳定性对比，不能直接宣称 GPU 可用。
- 检查 `pause_before_ms` 等响应分段字段是否真正被播放侧消费；为关播报/切换设备/取消提供一致生命周期。

**验收**：模型可选配且缺失时有清晰降级；golden 音频集保留人工转写与场景标签；记录每个 ASR/TTS 配置的精度/实时率/首块与整句耗时、资源占用；分段拼接无重复/丢字、播放顺序稳定，取消后无旧音频续播。音质必须由听感评估补充，不能只用编译或接口测试替代。

### 阶段 D：低延迟、异步工具和 Agent 对话协作

**工作项**
- 以 VoiceTrace/LatencyMetrics 分布定位瓶颈后再优化。可研究 partial-ASR 下的 preemptive generation，但必须在 final 文本改变、用户继续说话、工具需求变化时取消或重试，禁止播放依据不完整输入生成的陈旧答案。
- 将慢工具统一到 TaskManager 生命周期：deadline/超时、取消、重复调用保护、topic supersede、结构化进度事件、结果回填。搜索等耗时任务可发自然简短进度，但不可编造完成状态；用户打断时按工具语义决定取消或仅保留后台结果。
- 保持 AgentLoop 对工具调用轮数、JSON 参数校验、错误返回和最大总耗时的限制；危险/有外部副作用的工具需显式确认策略。网络搜索明确区分搜索引擎、平台 API 与站内结果来源。
- 补齐必要 MCP 支持：先定启动/stdio 与 HTTP 传输边界、服务器 allowlist、超时/取消、工具 schema 映射、错误隔离和 GUI 可见性。不要把任意 shell 或任意 MCP server 默认开放给语音模型。
- 仅在出现真实复杂分工需求后，设计 Agent handoff：handoff 触发条件、上下文摘要、工具权限、状态回传/恢复；优先单 Orchestrator + 多工具，避免多个 LLM 无必要串行增加延迟。

**验收**：慢工具期间状态可见且语音交互仍可中断；取消/超时后无重复副作用和迟到话语；工具错误可恢复且错误有 trace；Agent 循环上限/参数 schema 有边界测试；MCP 的断线、畸形输出和未授权工具均安全失败。投机生成只有在同一硬件/语料的延迟获益成立且错误答案率不升时才默认开启。

### 阶段 E：评估、观测、降级与长稳

**工作项**
- 复用 VoiceTrace/LatencyMetrics，确认隐私边界、默认关闭/启用策略、清理方式和敏感文本是否需要脱敏；为每轮关联配置、模型版本、设备和决定事件，避免只剩平均值。
- 引入分层评估：纯逻辑单测（状态、取消、截断、路由）；固定音频回放（端点、打断、ASR）；端到端真人/设备走查（回声、听感、真实延迟）；长稳与模型故障演练。LLM/工具的非确定输出用事件约束/行为判据，不硬编码整段措辞。
- 为 ASR/LLM/TTS/搜索的模型缺失、异常退出、远端超时、流中断、显存不足定义健康状态和降级回退；断网时验证本地核心交互仍能工作。
- 对常驻模型、队列、SQLite、音频设备重复启动/停止做真实时长测试，并记录内存/显存曲线与恢复结果。

**验收**：在项目实际目标 Windows 设备上完成指定时长的真实音频会话（建议先 30 分钟，再按使用情境扩展）；保留 trace 和配置快照；运行中打断/取消、TTS/LLM故障、断网、设备重启均有可解释行为；显存不足能提示或回退，不崩溃。明确人工主观验收与自动测试各自覆盖边界。

### 阶段 F：低成本视觉交互与 HTML 展示（核心语音稳定后，可选）

目标是让语音交互在需要时有清晰的视觉反馈和可操作内容，同时保持低延迟、低运行成本。动画与图示由本地界面和预制模板绘制；不为每个回答调用图像/视频生成服务。

**工作项**
- 先盘点 GUI 和 Orchestrator 已能提供的会话事件，再定义小而稳定的 `VisualEvent`/视图模型：聆听、思考/工具处理中、说话、用户打断、完成，以及可选的结构化展示内容。视觉状态跟随真实音频/任务生命周期，不让模型直接操纵 GUI 内部状态。
- 最小动态反馈先做本地 2D 状态动画（如呼吸/闪烁、音量波形或嘴部开合）；驱动数据来自现有 VAD、TTS 播放游标或音量包络，不为动画帧调用 LLM。隐藏窗口时降帧或暂停；动画不能阻塞音频、ASR、打断和主界面。
- 需要解释或比较时支持可选 HTML/SVG 展示，例如摘要卡、步骤、时间线、简单图表或可点击的局部展开；优先让模型返回有 schema 的数据块，由本地模板渲染。研究嵌入 WebView 与现有 Qt 原生绘制的包体、内存、冷启动、性能和维护成本后再选技术，不预先强加 WebEngine。
- 所有视觉内容可关闭，文本/语音路径不依赖渲染器；提供减少动态效果/静态模式。状态动画、语音字幕和展示卡在暂停、重播、取消、打断和新一轮开始时保持同步。
- 将外部文本当作不可信输入：模板必须转义用户/模型内容，脚本与资源限定为本地 allowlist，不开放任意文件、shell、外网导航或模型生成的可执行 HTML/JS。若需要富交互，只暴露明确的白名单动作并校验参数。
- 先做交互原型和资源测量，再决定是否加入主程序；静态 HTML 预览可单独验证信息层级，不把生成一份 HTML 文件误认为已经具备实时交互。

**验收**：在无网络、无图像/视频生成 API 的情况下，音频对话、取消和打断仍正常；模拟各 `VisualEvent` 可重复触发正确视图并在真实 TTS 播放时同步；动画隐藏/静态模式可用且无阻塞音频的回归；HTML 内容由 schema 验证并安全渲染，拒绝脚本/外网资源；在目标电脑记录渲染器冷启动、空闲/动画 CPU 与内存占用和帧率，确定清晰的资源上限后再默认开启。若嵌入式 HTML 的成本不合算，保留 Qt 原生状态动画并将复杂视觉卡片作为按需打开的本地 HTML。

## 5. 推荐决策顺序

1. 先确认产品主要语言、实际硬件、默认是否完全离线、是否需要多轮工具进度播报。不要沿用早期 CUDA/12GB 显存假设。
2. 先修正确性与测量（阶段 A/B），再追求端到端延迟。低延迟但抢话、附和词误打断或上下文错乱不是成功。
3. 模型选择由本项目数据集和目标电脑基准决定。优先复用已经集成并稳定的 sherpa-onnx/llama.cpp/ORT 路径；新模型必须有许可、内存、量化、下载、更新和回退方案。
4. 用开关/adapter 做可逆实验；模型切换、语义 turn detector、投机生成、在线服务和新 TTS 都应可关闭，默认行为保持可预测。
5. MCP 与多 Agent handoff 属于能力扩展，不先于音频轮次正确性；handoff 不等同于启动另一个 Codex 编码 Agent。
6. 视觉增强放在语音正确性和长稳之后：语音始终可单独工作；先做本地状态动画，再按需渲染 HTML/SVG 说明；视频/图像生成只在用户明确要求且成本可接受时考虑。

## 6. 暂不实施

- 不将 LiveKit Agents SDK 或 LiveKit Server 作为本地桌面运行时的必需依赖。
- 不迁移到 Python/Node，也不重写 Qt GUI 或当前 C++ Orchestrator。
- 不为了“全双工”接入 WebRTC/SFU、房间调度、SIP、生成式视频、3D Avatar 或 Cloud inference；这不排除本地模板化 2D 动画与 HTML/SVG 视觉层。
- 不未经基准比较就同时接入多个新 ASR/TTS/LLM 模型；不假设 Intel Arc DirectML 支持现有 Kokoro 算子。
- 不默认开启投机生成、云端模型、云端语音处理或自动暴露 MCP/shell 权限。
- 不把情绪/心理分析等与产品目标无关的功能并入语音运行时。

## 7. 实施者每阶段交付格式

每阶段开始先报告：现状核对、最小改动方案、预期文件和风险。完成后报告：改动摘要、实际验证命令/结果、未覆盖场景、PROGRESS 记录。不要为了本路线图本身运行构建或测试；后续编码 Agent 执行代码阶段时，才按该阶段验收要求运行相关验证。不要创建 tag、提交或清理工作区，除非用户另行要求。

## 8. 参考资料

- LiveKit Agents 仓库（Apache-2.0 框架；模型许可需单独核对）：<https://github.com/livekit/agents>
- LiveKit 轮次与打断（检测模式、端点等待、语义 detector、interruption 和音频预处理）：<https://docs.livekit.io/agents/logic/turns/>
- LiveKit 轮次调参（turn detection、endpointing、adaptive interruption、preemptive generation）：<https://docs.livekit.io/agents/logic/turns/tuning/>
- LiveKit pipeline nodes/hooks（STT/LLM/TTS 处理节点、生命周期、打断时分段保留/截断）：<https://docs.livekit.io/agents/logic/nodes/>
- LiveKit 异步工具（进度更新、取消信号、慢工具）：<https://docs.livekit.io/agents/logic/tools/async/>
- LiveKit pipeline 类型（级联、realtime、half-cascade 的延迟/能力取舍）：<https://docs.livekit.io/agents/models/pipelines/>

链接和功能依据于 2026-10-07 检视；LiveKit SDK/API 迭代较快，后续实现时应重新核对当前官方文档。本路线图仅将公开文档中的机制作为设计参考，不声称已在本地验证 LiveKit 的性能数字或云端模型效果。
