# Voice Agent — 本地全双工语音助手

一款**本地优先、支持 GPU 加速**的桌面语音 Agent。C++20 / Qt6 编写，采用全双工流式语音链路：
说话 → VAD 端点检测 → ASR 转写 → LLM 思考（可调工具）→ TTS 合成 → 播放，**边生成边播报**，并把文字回复实时显示为对话气泡。

界面采用 ChatGPT 式三栏布局，主对话区简洁高效，所有技术细节（模型状态 / 日志 / 耗时）收纳进可折叠的右侧日志栏。

---

## 目录

- [核心特性](#核心特性)
- [界面布局](#界面布局)
- [架构](#架构)
- [技术栈](#技术栈)
- [目录结构](#目录结构)
- [环境依赖](#环境依赖)
- [构建](#构建)
- [运行](#运行)
- [配置](#配置)
- [模型](#模型)
- [语音交互](#语音交互)
- [工具与记忆](#工具与记忆)
- [测试](#测试)
- [部署](#部署)
- [性能与反馈](#性能与反馈)
- [已知问题](#已知问题)
- [路线图](#路线图)

---

## 核心特性

| 能力 | 说明 |
|------|------|
| 🎙️ 全双工语音链路 | 采集 → VAD → ASR → LLM/工具 → TTS → 播放，五阶段串联 |
| ⚡ 流式播报 | LLM 逐 token 生成，按句子切分交给 TTS 异步合成，首句即播放，不等整段生成完 |
| ⌨️ 按住说话（PTT） | 空格键按住录音、松开结束转写（可关 VAD 切换为“按下说、松开转写”） |
| 🧠 持久化记忆 | SQLite + FTS5，支持去重合并、中英文召回，`/memory` 命令行 |
| 🛠 工具调用 | get_time / web_search / memory_save / memory_query，贴合意图门控触发 |
| 🔍 搜索路由 | 本地 SearXNG 优先，Tavily / Brave 在线桩回退 |
| 🧩 模型管理 | 内建主流小模型下载清单，图形化选择 / 下载 / 切换 |
| 🚀 GPU 加速 | LLM→Vulkan；VAD/ASR→DirectML；TTS→CPU（见[已知问题](#已知问题)） |
| 🖥 简洁界面 | ChatGPT 式三栏：左对话/记忆、中主对话、右折叠日志 |

---

## 界面布局

```
┌──────────────┬──────────────────────────────┬──────────────────┐
│  左栏(侧栏)   │       主对话区(默认页)          │ 右栏(可折叠日志栏)   │
│              │                              │                  │
│ Voice Agent  │  当前对话标题  ……… ⊟收起日志    │  模型状态          │
│ ＋ 新建对话   │  ◇ 处理流程条(采集→…→播放)      │   VAD/ASR/TTS/LLM │
│              │                              ├──────────────────┤
│ ▾ 对话        │  ┌───── 对话气泡流 ──────┐    │  工具/记忆/运行日志 │
│   ● 对话 1    │  │ ● 我   您说的话         │    │                  │
│              │  │ ◆ 助手  助手回复         │    ├──────────────────┤
│ ▾ 共享记忆    │  └──────────────────────┘    │  轮次耗时          │
│  记忆条目…     │  [流式回答气泡]               │  阶段 / 耗时       │
│              │  [🎤开始语音][🔊朗读][语音播报][VAD]│                  │
│ ⚙ 设置       │  [输入…                      ][发送] │                │
└──────────────┴──────────────────────────────┴──────────────────┘
```

- **左栏**：`＋ 新建对话` 新建并切换对话（当前为单会话上下文，结构已预留多会话扩展）；`▾ 共享记忆` 分组实时列出记忆库条目，双击查看全文；底部 `⚙ 设置` 进入模型管理等设置页。
- **主对话区**：默认页。置于中央，只展示对话气泡流 + 处理流程条 + 输入行，最简洁直观。
- **右栏（日志）**：默认展开，可点主区右上角 `⊟ 收起日志` / `⊞ 显示日志` 一键折叠。收纳了三类技术信息——模型状态、工具/记忆/运行日志、轮次耗时时间轴，与主界面完全分离。

---

## 架构

```
                    ┌───────────────────────────────┐
┌──────────┐        │        src/gui/ (Qt 线程)        │
│ 用户交互  │───────▶│                                  │
│ 语音/文本 │        │  MainWindow(三栏界面)             │
└──────────┘        │       ↕ 信号槽跨线程                │
                    │  AgentController(工作线程)         │
                    └───────────────┬───────────────────┘
                                    │ 装配
    ┌──────────────┬───────────────┼────────────────┬───────────────┐
    ▼              ▼               ▼                ▼               ▼
 src/orchestrator  src/agent       src/memory       src/search      src/audio
 Orchestrator      AgentLoop       MemoryManager    SearchRouter    AudioPipeline
 全双工状态机      工具循环/意图门控  SQLite+FTS5      SearXNG/Tavily/ 采集+播放
 (Idle/Listening/  -LLM生成          /memory 命令      Brave           +AEC
  EouPending/      -工具执行         -召回注入prompt                       ▼
  Thinking/        -回填再生成                                             ▼
  Speaking/）      ToolRegistry     src/llm          src/vad        src/tts
  ~句级切分~      get_time,web_    llama.cpp       Silero(ONNX     Kokoro
  流式播报调度      search,memory_   (Vulkan GPU)     DirectML)      (sherpa CPU)
                   save/query
```

- 各推理模块（`LLM / VAD / ASR / TTS`）对上层提供统一接口（pimpl），实现可插拔：真实后端就绪即推理，缺失时自动回退 **Mock 占位**保证界面可用。
- `AgentController` 在**独立工作线程**内装配核心，通过 Qt 信号槽把 `stateChanged / llmToken / toolCalled / turnTimeline` 等事件安全桥接到 UI 线程，**界面永不阻塞**。
- 核心逻辑抽离为静态库 `voice_agent_core`，供 GUI / 测试 / CLI 复用。

---

## 技术栈

| 类别 | 技术 |
|------|------|
| 语言 | C++20 |
| GUI | Qt 6.8.3（`msvc2022_64` 套件，Widgets） |
| 构建 | MSVC 2022 + Ninja + CMake |
| LLM | llama.cpp（GGUF），后端 `n_gpu_layers=999` 全量下放 Vulkan GPU |
| VAD | Silero ONNX，ONNX Runtime **DirectML**（GPU） |
| ASR | Paraformer-zh（备用 whisper-tiny），sherpa-onnx + ONNX Runtime **DirectML**（GPU） |
| TTS | Kokoro 多语言（`zf_xiaobei` 中文女声），sherpa-onnx，**CPU** 合成 |
| 记忆 | SQLite（Amalgamation 3.46.1）+ FTS5，WAL / 事务 / 去重合并 |
| 搜索 | 自包含 HTTP GET 客户端；本地 SearXNG / Tavily / Brave |
| 日志 | spdlog（静态链接） |
| EXE 部署 | windeployqt |

---

## 目录结构

```
voice-agent/
├── CMakeLists.txt          # 顶层构建：voice_agent_core 静态库 + voice-agent.exe + tests
├── Justfile                # just 任务配方（configure/build/run/test/deploy…）
├── configs/
│   └── agent.yaml          # 主配置：音频 / VAD / 打断 / EOU / 模型路径 / 搜索 / 日志
│   └── dml_ort_all.conf    # ASR DirectML GraphOptimizationLevel=ALL 配置
├── data/                   # 运行产物：voice_agent.db（记忆库）、测试音频、mel 常量
├── models/                 # 模型（按 asr/llm/tts/vad 分目录）
├── scripts/                # 辅助脚本（如下载音频依赖）
├── src/
│   ├── main.cpp            # 程序入口（QApplication + CRT 断言报告重定向）
│   ├── core/               # 核心共有类型、EventBus、SPSC RingBuffer、CancelToken
│   ├── audio/              # 采集 / 播放管道、AEC 回声消除
│   ├── vad/                # Silero VAD（ONNX Runtime DirectML，动态签名）
│   ├── asr/                # Paraformer-zh / whisper 后端、mel 前端、tokenizer
│   ├── llm/                # llama.cpp 后端（Vulkan GPU）
│   ├── tts/                # Kokoro / sherpa-onnx 后端、流式播报（streaming_speaker）
│   ├── orchestrator/       # 全双工状态机、EOU 检测、智能轮次、音频路由、句级切分
│   ├── agent/              # AgentLoop、工具注册/执行、GBNF grammar、意图门控
│   ├── memory/             # SQLite+FTS5 记忆库、抽取/召回、/memory 命令
│   ├── search/             # 搜索路由与 Provider（SearXNG / Tavily / Brave）
│   ├── util/               # 配置加载、日志、HTTP 客户端、GPU 探测
│   └── gui/                # 三栏 MainWindow / AgentController / 设置 / 模型管理 / 延时调试
└── tests/                  # CTest 测试集（ring_buffer / agent / memory / whisper / kokoro / gpu_probe）
```

---

## 环境依赖

| 依赖 | 备注 |
|------|------|
| Windows 10/11 x64 | 开发环境为 Windows 11 |
| Visual Studio 2022 (MSVC) | **必须 MSVC，不可用 MinGW**（与 Qt6 `msvc2022_64` 套件兼容性问题） |
| Qt 6.8.3 | `C:/Qt/6.8.3/msvc2022_64`，含 Widgets / Network |
| CMake ≥ 3.x + Ninja | 构建生成器 |
| Vulkan SDK | 用于 llama.cpp Vulkan 后端，`just configure` 自动探测 |
| just | 任务运行器（可选，也可直接看 Justfile 命令） |

### 网络可选依赖
- **SearXNG**（`http://localhost:8080`）本地搜索；或配置 `tavily_key` / `brave_key`。

---

## 构建

```powershell
# 1. 配置 + 编译（MSVC 环境 + Ninja + Qt6 + Vulkan，增量）
just configure
just build

# 或一次到位
just build          # 内部也会先 configure
```

构建目标：
- `voice_agent_core` —— 静态库，供 GUI / 测试复用
- `voice-agent.exe` —— 主程序（GUI）
- `tests/*.exe` —— 测试集

> 首次构建会自动通过 CMake FetchContent 拉取并编译 llama.cpp、spdlog、nlohmann/json，并下载解压
> onnxruntime / sherpa-onnx 到 `third_party/`，耗时较长属正常。

---

## 运行

```powershell
# 编译主程序并启动界面（自动配置 Qt DLL 路径）
just run
```

启动后界面默认显示左栏（对话 / 共享记忆）、主对话区、右侧日志栏。
加载模型期间顶部主区会出现“初始化”进度条，完成即隐藏并可交互。

---

## 配置

编辑 `configs/agent.yaml`：

```yaml
# 模型路径（相对于项目根）
vad_model: models/vad/silero_vad.onnx
asr_model: models/asr/paraformer-zh
tts_model: models/tts/kokoro-multi-lang-v1_0
llm_model: models/llm/qwen2.5-1.5b-instruct-q4_k_m.gguf

# 音频 / VAD / 打断 / EOU 等均有默认值，见文件内注释
# 搜索
searxng_url: http://localhost:8080
log_level: info
```

> 在 **设置页** 切换模型后，程序会把所选模型路径自动写回本文件并持久化。

---

## 模型

### 获取方式
- **设置 → 模型下载**：内置可下载小模型清单（Silero VAD / Whisper tiny / Kokoro / Qwen2.5 系列），支持 HTTPS + 断点续传，以 `.part` 临时文件写盘，完成后自动归位。
- **手动放置**：按目录名放到 `models/{asr,llm,tts,vad}/` 下。

### 当前示例模型

| 类型 | 模型 | 后端 | 体积 |
|------|------|------|------|
| LLM | Qwen2.5-1.5B-Instruct Q4_K_M (GGUF) | llama.cpp · **Vulkan GPU** | ~1.1 GB |
| VAD | Silero VAD (ONNX) | ORT · **DirectML GPU** | ~629 KB |
| ASR | Paraformer-zh（备用 whisper-tiny） | sherpa · **DirectML GPU** | 数百 MB |
| TTS | Kokoro 多语言 · `zf_xiaobei` | sherpa · **CPU** | 数百 MB |

模型状态条（右侧日志栏顶部）会实时显示每个模块的**推理后端**（`· Vulkan GPU / · DirectML GPU / · CPU`）与加载状态，悬停可见详细文件、占用、加载耗时。

---

## 语音交互

| 操作 | 说明 |
|------|------|
| 按住空格说话 | 按下开始录音，松开结束并转写（默认 VAD 模式自动切分） |
| 点击“开始语音” | 开关全双工监听 |
| `🔊 朗读` | 朗读最近一次回复 |
| `语音播报` 开关 | 关闭后仅停止合成/播声，文字回复照常 |
| VAD 开关 | 关闭后改为“按住说 / 点击按钮录音，结束即转写”，ASR 直接接管 |

**流式播报**：LLM 边生成 token，按句子（`。！？；`、换行）切分，交给独立线程的 TTS 合成并立即播放，首句出声即可，显著缩短“开口前等待”。
处理流程条（主区中部）以四态展示实时进度：灰=未开始、黄=进行中、绿=完成、虚线灰+`·跳过`=未参与（如文本输入跳过采集）。

---

## 工具与记忆

### 工具调用
内置 `get_time / web_search / memory_save / memory_query`，由 LLM 结构化输出（GBNF grammar）驱动。
**意图门控**：仅当用户明确要求（如“搜一下 / 查天气 / 帮我记住…”）才放行工具，避免无意触发。

### 记忆（持久化，SQLite + FTS5）
| 命令 | 作用 |
|------|------|
| `/memory save <内容>` | 保存一条记忆（自动去重合并） |
| `/memory list` | 列出最近记忆 |
| `/memory query <关键词>` | 召回相关记忆 |
| `/memory forget <id>` | 删除指定记忆 |
| `/memory clear` | 清空记忆库 |
| `/memory count` | 记忆条数 |

- 每次对话会**按需召回**相关记忆并注入 system prompt。
- 记忆写入只在“明确指示”时发生（用户 `/memory save`，或模型在你的要求下调用 `memory_save`），避免高频无意污染长期记忆。
- 左栏「共享记忆」分组会实时展示记忆库内容，双击查看全文。

---

## 测试

```powershell
just build        # 先构建
just test         # 运行全部 CTest
just test-single test_agent   # 运行单个测试
```

覆盖：SPSC RingBuffer、Agent 工具循环（grammar/解析/校验/超时/取消）、记忆（分词/CRUD/召回/有效期/命令/持久化）、Whisper 转写、Kokoro 中文合成、GPU 端到端探针。

---

## 部署

```powershell
just build
just deploy       # windeployqt 将 Qt DLL 部署到可执行目录
```

把 `build-msvc/` 下 `voice-agent.exe` 与其同目录 DLL 一起分发即可运行（另需确保运行时依赖的模型、`third_party` 下 onnxruntime / sherpa DLL 齐全）。

---

## 性能与反馈

- 主区 / 右栏展示**每轮各阶段耗时时间轴**（采集 / ASR / LLM首token / LLM生成 / 工具 / TTS），并给出合计。
- **延时调试面板**（设置页内）轮询各模型推理延时，并在每轮结束后自动给出：
  - **瓶颈判定**：某阶段延时至该轮总时长 ≥30% 且 ≥1500ms 视为瓶颈。
  - **调整建议**（仅给出建议，不自动改参）：LLM 瓶颈→降 `max_tokens`；TTS 瓶颈→降 `sample_rate` / 提 `speed`；非瓶颈 TTS/ASR→提 `num_threads`；VAD→降 `threshold`。
- 每轮输入-输出延时均会记录并显示在调试面板的反馈区。

---

## 已知问题

1. **TTS(Kokoro) 无法在 Intel Arc 上跑 GPU（关键限制）**：Kokoro 模型的 **grouped ConvTranspose** 算子与 Intel Arc 的 DirectML 驱动不兼容，合成时触发 `0xC0000409` 栈溢出崩溃（引擎能创建、运算即崩、不可 try-catch）。已实测排除驱动版本（8243→9030 仍复现）、包完整性、杀软等因素；当前方案为启动时检测 Intel 适配器**自动回退 CPU 合成**（约 3s 合成 4s 语音，对话场景可接受）。如需 TTS 真正上 GPU，需换不含 grouped ConvTranspose 的模型（如 VITS / Matcha / Piper 系 sherpa-onnx 模型）。
2. **Intel Arc 显存 2GB 限制**：LLM 建议用 Qwen2.5-1.5B Q4（可被 2GB 容纳）；大模型需更大显存机型。
3. **ASR 精度**：默认 Paraformer-zh 精度优于 whisper-tiny；whisper-tiny 为最小模型，仅保底。
4. **在线 Provider**：联网搜索需本地 SearXNG 或有效 Tavily/Brave key；离线时搜索自动安全降级。
5. **构建镜像问题**：`CMakeLists` 变化触发 ninja 重跑 cmake 时可能误用 Strawberry/GCC 覆盖编译器，请在 VsDevShell 中显式 `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl` 重配（`just configure` 已处理）。

---

## 路线图

- [ ] M7 — MCP 集成（连接外部 MCP Server）
- [ ] M8 — 延迟调优、显存优化、30 分钟稳定性测试
- [ ] 完整多对话持久化（当前多对话为 UI 结构先行，核心为单会话上下文）
- [ ] TTS 换用 GPU 兼容模型（VITS/Matcha/Piper 系）以启用 GPU 合成
- [ ] 记忆向量化语义召回