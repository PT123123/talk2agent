# Voice Agent 实施进度

## 概述
- **目标**：本地优先全双工语音 Agent，C++ 为主，支持 GPU
- **GPU**：Intel Arc（Vulkan/OpenVINO）
- **模型**：Qwen3 + SenseVoice + Kokoro

---

## 阶段进度

### M0 - 工程骨架 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | CMake 工程、核心类型、SPSC Ring Buffer、EventBus、CancelToken、日志配置 |
| **验收结果** | ✅ 编译通过，核心组件功能正常 |
| **遗留问题** | 待接入真实模型 |
| **文件** | `CMakeLists.txt`, `src/core/*`, `src/util/*`, `tests/test_ring_buffer.cpp` |

### M1 - 音频闭环 + AEC ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | miniaudio 采集/播放、AEC NLMS 自适应滤波器、AudioDeviceManager、AudioPipeline |
| **验收结果** | ✅ 编译通过，设备枚举正常，音频管道运行正常 |
| **遗留问题** | AEC 待接入 speexdsp 增强算法；需真实 ASR 验证 ERLE |
| **文件** | `src/audio/audio_device.hpp/cpp`, `src/audio/audio_pipeline.hpp/cpp`, `src/audio/echo_cancellation.hpp/cpp` |

### M2 - VAD + 打断探测 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | VAD 模块（能量检测 + ONNX Runtime 接口）、InterruptionDetector（5 层判定）、音频管道重启修复 |
| **验收结果** | ✅ 编译通过，VAD 回调正常工作，音频管道可重启复用 |
| **遗留问题** | 需下载 ten-vad/silero 模型进行真实 VAD 验证；AEC 未收敛需后续调优 |
| **文件** | `src/vad/vad.hpp/cpp` |

### M3 - ASR + LLM + TTS ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | ASR 模块（Mock 实现，支持 SenseVoice/Whisper 接口）、LLM 模块（Mock 实现，支持 llama.cpp 流式接口）、TTS 模块（Mock 实现，支持 Kokoro 接口）、pimpl 架构设计 |
| **验收结果** | ✅ 编译通过，ASR/LLM/TTS 初始化正常，端到端流程测试通过 |
| **遗留问题** | 需下载真实模型：SenseVoice ONNX、Qwen3-1.5B-Q4 GGUF、Kokoro ONNX；需接入 ONNX Runtime |
| **文件** | `src/asr/asr.hpp/cpp`, `src/llm/llm.hpp/cpp`, `src/tts/tts.hpp/cpp` |

### M4 - 全双工状态机 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | Orchestrator 六状态机（Idle/Listening/EouPending/Thinking/Speaking/Interrupting）、EOUDetector 双阈值（350ms 快阈值 / 900ms 强制阈值）、SmartTurn 轮次判定（barge-in/回utterance词分辨）、AudioRouter 音频路由、EOUDetector mutable mutex bug 修复 |
| **验收结果** | ✅ 编译通过，状态机框架完整 |
| **遗留问题** | 需真实音频设备测试状态转移；需模型下载后端到端验证 |
| **文件** | `src/orchestrator/orchestrator.hpp/cpp`, `src/orchestrator/eou_detector.hpp/cpp`, `src/orchestrator/smart_turn.hpp/cpp`, `src/orchestrator/audio_router.hpp/cpp` |

### M5 - Agent + Tools ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | Agent 结构化输出（JSON Schema→GBNF grammar + 工具调用解析/离线校验）、ToolRegistry + ToolExecutor（并行执行、单工具超时、级联取消）、内置工具（get_time/web_search/memory_save/memory_query）、AgentLoop（生成→解析→执行→回填→再生成，轮次上限3，token 流式 sink）、搜索框架（ISearchProvider 抽象、本地 SearXNG Provider、Tavily/Brave 在线桩、SearchRouter 本地优先+回退+URL 去重）、自包含 HTTP GET 客户端、Orchestrator 挂载 `attach_agent()` 使 Thinking 阶段走 AgentLoop |
| **验收结果** | ✅ 编译通过；`tests/test_agent` 8 项断言全部 PASS（grammar/解析/校验/注册/工具执行/超时/搜索离线安全/AgentLoop/取消）；`ctest` 2/2 通过 |
| **遗留问题** | 在线 Provider 需带 TLS 的客户端（cpr/libcurl）方可真正联网；记忆层为进程内 EphemeralMemory（M6 换 SQLite+FTS5+向量）；真实模型接入后需 GBNF 粒度调优 |
| **文件** | `src/agent/{grammar,tool_registry,tools,agent_loop}.hpp/cpp`, `src/search/{isearch,searxng_provider,online_providers}.hpp/cpp`, `src/util/http.hpp/cpp`, `tests/test_agent.cpp` |

### M6 - Memory ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | SQLite 持久化记忆库（AMALGAMATION 3.46.1 本地 third_party，FTS5 编译启用，WAL/事务/去重合并）、双路径召回（FTS5 拉丁 OR + 整句 LIKE 兜底中文短词）+ 语料级打分（unit 覆盖率 + 时间衰减 + salience），MemoryStore / MemoryExtractor（中英文启发式基线）、MemoryRetriever（有效期过滤）、MemoryManager（ingest→recall→command 门面）、/memory 命令（save/list/forget/clear/count）、Orchestrator 挂载 `attach_memory()`（自动拦截命令、召回注入 system prompt、每轮 Agent 完成后自动 ingest）、memory_* 工具接入持久化 MemoryStore |
| **验收结果** | ✅ 编译通过；`tests/test_memory` 8 组断言全部 PASS（分词/CRUD/中文短词召回/有效期过滤/抽取/命令/持久化跨实例）；`ctest` 3/3 通过 |
| **遗留问题** | 抽取器目前为启发式基线（真实 LLM 结构化抽取待接入后调优）；向量化语义召回归入后续（当前为字面/单元重叠）；在线 Provider 需 TLS 客户端 |
| **文件** | `src/memory/{memory_store,memory_extractor,memory_retriever,memory_manager,memory_command,text_features}.hpp/cpp`, `third_party/sqlite3/`, `tests/test_memory.cpp` |

### GUI - Qt6 桌面界面 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | 从命令行迁移到 Qt6 GUI（QApplication + MainWindow）。`AgentController` 在独立工作线程内装配 Orchestrator/Agent/Memory/Search，Qt 信号槽把业务事件（stateChanged / llmToken / toolCalled 等）安全桥接到 UI 线程。三面板布局：左侧对话历史 + 文本输入，右侧语音控制 + 实时 ASR/LLM/日志面板。构建切换到 MSVC 2022 + Ninja + Qt 6.8.3 msvc2022_64 套件，抽离 `voice_agent_core` 静态库供 CLI/GUI/测试复用；`just run` 一键编译并启动 GUI，`just deploy` 用 windeployqt 部署 Qt DLL |
| **验收结果** | ✅ Debug 编译通过；`ctest` 3/3 通过（ring_buffer/agent/memory，顺带修复 MSVC Debug 暴露的 ring buffer 覆盖语义 + 工具参数校验悬空迭代器）；GUI 已通过 windeployqt 独立运行 |
| **遗留问题** | 真实模型（SenseVoice/Qwen3/Kokoro）后续在 GUI 环境中端到端验证 |
| **文件** | `src/main.cpp`, `src/gui/{main_window,agent_controller}.hpp/cpp`, `CMakeLists.txt`, `Justfile` |

### GUI+模型管理 - Qt6 桌面界面 + 设置页 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | 在 Qt6 GUI 增加"设置"Tab，实现模型管理闭环：**①模型选择**（每类 VAD/ASR/TTS/LLM 的"当前使用"下拉，含默认项）+ **②模型下载**（内置可下载开源小模型清单：Silero VAD 112MB / Whisper tiny.en 78MB / Kokoro 86MB / Qwen2.5-0.5B 491MB，URL 均经 HF/GitHub 等核验；基于 Qt6::Network，支持 HTTPS/TLS + 断点式流式写盘到 models/ 的 .part 临时文件）+ **③下载信息展示**（进度条 + 已下载/总大小百分比 + 阶段/错误状态文本，错误精准上报）+ **④模型切换**（设置页"应用切换"→ AgentController 新增 SetModels 任务，在 worker 线程重建 LLM/VAD/ASR/TTS 并重建 Orchestrator 重新挂载 agent/memory，更新并持久化 configs/agent.yaml，modelsChanged 信号回显当前模型） |
| **验收结果** | ✅ Debug 编译通过；`ctest` 3/3 通过；GUI 冒烟启动正常；windeployqt 已部署 Qt6Network + schannel TLS 后端供 HTTPS 下载 |
| **遗留问题** | 真实模型需接入 ONNX Runtime / llama.cpp 推理方能在对话中使用；下载依赖网络连通性 |
| **文件** | `src/gui/{model_catalog,model_downloader,settings_panel}.hpp/cpp`, `src/gui/main_window.*`（QTabWidget）, `src/gui/agent_controller.*`（SetModels+持久化）, `CMakeLists.txt`（Qt6::Network） |

### 真实推理 - VAD+ASR 语音链路（ONNX Runtime）✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-20 |
| **完成内容** | 接入 ONNX Runtime 1.19.2 CPU（自动下载解压到 `third_party`，CMake 条件编译 `USE_ONNXRUNTIME`）。**①Whisper tiny ASR**：log-mel 前端（400-FFT/Slaney 归一化，与 OpenAI 官方对齐）、byte-level BPE tokenizer、encoder/decoder 两段式 ONNX 推理 + greedy 解码，`ASR::transcribe_segment` 走真实 Whisper 后端。**②Silero 流式 VAD**：按模型输入签名动态探测并构建输入（波形 + h/c 或单 state，可带可选 sr），逐 512 样本推理，回流状态机触发 SpeechStart/SpeechEnd。**③链路打通**：Orchestrator 在 VAD 段起止间缓冲语音，SpeechEnd 整段交给 Whisper 转写 |
| **验收结果** | ✅ Release 编译通过；`ctest` 5/5 通过；用 Windows SAPI 生成的 16kHz 真实语音 WAV 端到端验证：Silero 正确切出 3 个语音段，Whisper 逐段转写并回传文本（whisper-tiny 精度有限 + 合成音，梗概可读） |
| **遗留问题** | whisper-tiny 识别精度低（最小模型，正式用 sense_voice/更大 whisper）；LLM/TTS 仍为 Mock（Qwen3/Kokoro 待接入）；VAD 段内含前后静音边沿，可加切边优化精度 |
| **关键修复** | ①错误下载的 117MB silero_vad.onnx 是**整段变体**（无 state、输出 [batch,frames,999] 的奇怪分布），不可用 → 换成 k2-fsa 维护的 629KB 流式版（x+h/c）；②ORT 1.19.2 CPU EP 对**带状态更新的 LSTM 模型**若以 `nullptr` 输出名 Run 会 fail-fast（0xC0000409），必须**显式指定输出名**（prob+new_h+new_c）；③Silero 输入名/形状按真实模型现场探测（不再硬编码 input/state/sr） |
| **文件** | `src/asr/whisper_onnx.hpp/cpp`, `src/asr/whisper_frontend.hpp/cpp`, `src/vad/vad.cpp`（Silero 动态签名）, `src/asr/asr.cpp`, `src/orchestrator/orchestrator.cpp`, `third_party/onnxruntime-win-x64-1.19.2/`, `models/{asr/whisper-tiny,vad/silero_vad.onnx}`, `data/mel_std.py`, `tests/test_whisper.cpp`, `tests/test_vad_asr.cpp` |

### 真实推理 - LLM + TTS 语音链路（llama.cpp + Kokoro）✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-21 |
| **完成内容** | **①真实 LLM**：静态链接 llama.cpp（FetchContent 源码树，MSVC 编译），`LLM` 模块加载 GGUF（Qwen2.5 系列）并流式生成，替换 Mock。**②真实 TTS**：接入 sherpa-onnx 预编译 C API（`third_party/sherpa-onnx-win-x64`），`TTS` 模块加载 Kokoro 多语言模型（model.onnx + voices.bin + tokens.txt + espeak-ng-data + lexicon-zh.txt），`speaker_id=45`（zf_xiaobei 中文女声），生成 24kHz 中文语音，替换 Mock 正弦波。**③onnxruntime 统一**：sherpa 捆绑 ORT 1.28.2 与原有 1.19.2 冲突，统一升级头文件/导入库到 1.28.0，运行时使用 sherpa 自带的 1.28.2 DLL。**④测试 DLL 修复**：所有链接 voice_agent_core 的测试统一复制 onnxruntime.dll + sherpa-onnx-c-api.dll（静态库把导入表带进每个 exe），解决 0xc0000135。**⑤缓存污染修复**：CMakeLists 变更触发 ninja 重跑 cmake 时误用 Strawberry GCC 覆盖编译器，改为 VsDevShell 中显式 `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl` 重配 |
| **验收结果** | ✅ Release 编译通过；`ctest` 6/6 通过（含 test_kokoro：加载真实模型 + 中文合成 60 字符 → 4.78s @ 24kHz，173/240 帧有语音能量）；windeployqt 部署 + 运行时 DLL 齐全，GUI 冒烟启动正常 |
| **遗留问题** | LLM GGUF 模型未下载（Qwen2.5-0.5B/1.5B Q4_K_M 需从 HF 下载，GUI 模型管理已提供下载项）；llama.cpp 尚未启用 GPU（Vulkan）后端；whisper-tiny 精度有限 |
| **关键修复** | ①MSVC 默认 `__cplusplus` 报 199711 导致 llama.cpp u8 字面量 char8_t 不匹配 → 加 `/Zc:__cplusplus`；②ggml-cpu 需要 `_WIN32_WINNT=0x0A00` 才有 THREAD_POWER_THROTTLING_STATE；③onnxruntime 1.19.2 与 sherpa 捆绑 1.28.2 冲突 → 统一到 1.28.x；④spdlog 强制静态链接避免 DLL 缺失 |
| **文件** | `src/llm/llm.cpp`（llama.cpp 后端）, `src/tts/tts.hpp/cpp`（Kokoro/sherpa-onnx 后端）, `src/gui/agent_controller.cpp`（kokoro_config）, `src/util/config.hpp`, `configs/agent.yaml`, `third_party/sherpa-onnx-win-x64/`, `third_party/onnxruntime-win-x64-1.28.0/`, `tests/test_kokoro.cpp`, `tests/CMakeLists.txt`, `CMakeLists.txt`（sherpa + llama + 编译器选项） |

### GPU 加速改造（LLM/VAD/ASR + DirectML/Vulkan）
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-09-21 |
| **完成内容** | **①LLM GPU**：llama.cpp 启 Vulkan 后端（GGML_VULKAN + glslc 自动探测），GUI 配置 `n_gpu_layers=999` 全量下放 GPU，实测全部层下放到 `Vulkan0`（Intel Arc），`provider_label()="Vulkan GPU"`。**②VAD/ASR GPU**：接入 ONNX Runtime DirectML 1.24.4（NuGet `Microsoft.ML.OnnxRuntime.DirectML`），VAD(Silero) 与 ASR(Paraformer-zh)** fp32** 均走 DirectML（`provider_label()="DirectML GPU"`）；ASR fp32 + `ORT_ENABLE_ALL`（`configs/dml_ort_all.conf`，GraphOptimizationLevel=99）使整段转写从 6.5s 降至 0.97s。**③自编译 sherpa-onnx（DML 版）**：从源码 `SHERPA_ONNX_ENABLE_DIRECTML=ON` 构建并替换运行时（onnxruntime 1.24.4 + DirectML.dll），解决官方预编译包无 GPU 支持。**④TTS(Kokoro) 需 CPU 回退**：见下方"已知问题"，Intel Arc + DirectML 对 grouped ConvTranspose 有驱动级崩溃，已实现启动时 Intel 检测自动回退 CPU。**⑤GUI 状态条**：新增各模块推理后端标签显示（`· Vulkan GPU / · DirectML GPU / · CPU`），悬停可见详细推理后端 |
| **验收结果** | ✅ LLM 全部层落 Vulkan0、生成首 token 延迟达标；ASR 转写 600ms、VAD 单帧推理正常；`test_gpu_probe` 含 LLM(Vulkan)+ASR/VAD(DML)+TTS 端到端 GPU 探针通过；GUI 状态条正确显示各模块后端 |
| **遗留问题** | TTS(Kokoro) 尚未上 GPU（见"已知问题"第 4 条）；若需 TTS GPU 需换算子兼容模型（VITS/Matcha/Piper 系） |
| **关键修复** | ①Vulkan SDK 经 `winget install KhronosGroup.VulkanSDK` 安装；②System32 旧版 onnxruntime 1.17 抢先加载 → 显式拷贝正确 ORT DLL 到 probe 目录；③sherpa-onnx 从源码构建需网络重试（piper_phonemize 下载）+ 手拷 DLL 到 `sherpa-onnx-dml/lib` 而非 `cmake --install`（避免装到 Program Files）；④onnxruntime 版本冲突以 DirectML 1.24.4 优先；⑤paraformer int8 + DirectML 在 `ORT_ENABLE_ALL` 下会话创建死锁 → 降 `ORT_ENABLE_BASIC`（fp32 经 conf 走 ALL）；⑥`CreateDXGIFactory1` 未解析 → 链接 `dxgi.lib` |
| **文件** | `CMakeLists.txt`（Vulkan/DirectML 后端）、`justfile`（configure 加 `-DENABLE_VULKAN=ON` + SDK 探测）、`src/llm/*`、`src/vad/*`、`src/asr/sherpa_asr.cpp`、`src/tts/tts.cpp`、`src/util/gpu.cpp/hpp`、`src/gui/agent_controller.cpp`、`configs/dml_ort_all.conf`、`tests/test_gpu_probe.cpp` |

### R0 - Conversation Orchestrator 骨架 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 在既有 M0-M8 之上加一层 Conversation Runtime，**不推倒重来**。①**EventType 扩展**（`src/core/types.hpp`）：新增 Task*/Tool*/ModelFirstToken/Playback*/UserIntent/TopicChanged/ResponseSuperseded/QuickResponse/TimingDecision/RouteDecision 共 20 余种事件，Event 增加 `turn_id`/`task_id` 关联字段（`EventType::Error` 保持最后一位，EventBus 数组定容不受影响）。②**Task/TaskManager**（`src/core/{task.hpp,cpp}`）：`TaskPriority`(Foreground/Background/Speculative) + `TaskState`(含 Superseded) + `TaskPolicy`(Cancel/Pause/Continue/Supersede 四种打断处置)；固定 worker 池 + 优先级队列，**前台任务享保留位**（前台队列非空时不启动新后台任务）；`supersede_topic()` 换话题时打标记但**不 kill**（结果入 cache 不播报）；`apply_interrupt()` 按各任务 policy 分别处置，杜绝一刀切 cancel；结果统一走 `ResultSink` 回调，不阻塞。③**WorkingContext + ContextManager**（`src/core/working_context.hpp/cpp`）：WorkingContext 承载话题/活跃项目/最近实体，**换话题自动清空临时项**（长期有效项须显式给 TTL 或写入 Memory）；ContextManager 按 P0~P7 分层拼装上下文，各层独立预算，**预算不足时先裁低优先级层**。④**ResponsePolicy**（`src/orchestrator/response_policy.{hpp,cpp}`）：纯规则（零模型开销）判定 `silence/backchannel/quick_reply/answer/search/agent/deep_reasoning` + needs_memory/search/agent + depth + latency_budget + allow_background；含时效性/本地操作/个人上下文/复杂推理四类信号词识别；带强制静默窗口与"Agent 说话时不抢话"约束。⑤**ProgressUtterancePolicy**：按 task_type 轮换话术（杜绝每次都说"让我查一下"），快任务静默、用户不耐烦时闭嘴、同一任务最多说 2 句、不复读。⑥**UserSpeechIntent**（`src/orchestrator/user_intent.{hpp,cpp}`）：`Content/Backchannel/Continuation/Correction/Interruption/TopicChange` 六类正式枚举 + 中英词表分类器，把散落的 `if (text=="嗯")` 收进模型。 |
| **验收结果** | ✅ 四个新文件 MSVC `/W3` 零警告编译；`test_runtime` **10 组断言全部 PASS** —— 含核心验收项「前台回答在 300ms 后台搜索未完成时即已返回，两者 max_concurrent=2 真并发」；supersede 语义验证「任务仍跑完但 `should_speak=false`」；ContextManager 预算裁剪验证（300 字符预算下老轮次被裁、最新轮次与话题保留）；backchannel 关键回归「"嗯对了帮我查一下 Qwen3" 判为 Content 而非 Backchannel」 |
| **遗留问题** | TaskManager 尚未接入 Orchestrator（下一步 R1/R3 接线）；ResponsePolicy 仍是纯规则，未接小模型；EventType 新事件已定义但除 Task* 外尚无发射方；ContextManager 已能拼装上下文但 LLM 调用点仍在用 AgentLoop 自己的 history |
| **文件** | `src/core/{task,working_context}.hpp/cpp`, `src/orchestrator/{user_intent,response_policy}.hpp/cpp`, `src/core/types.hpp`, `tests/test_runtime.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` |

### R1 - 接线：Conversation Runtime 接入 Orchestrator ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 把 R0 的四个模块真正接进 Orchestrator。①**TaskManager 可重启**（`start()`/`stop()` 幂等）：GUI 的启停开关与模型切换会反复 `Orchestrator::start/stop`（`agent_controller.cpp` 里 start 2 处、stop 3 处），原先构造即起线程的写法会第二次 start 挂掉；同时 `submit()` 加懒启动兜底。②**打断改为按 policy 分派**：`interrupt_agent_()` 先 `tasks_.apply_interrupt()`，再走 session_token_/LLM/TTS 停声；额外清空 `stream_sentence_`（打断后残留半句会被继续播）与发 `ResponseSuperseded` 事件。③**新增 TurnAdmission**（`src/orchestrator/turn_admission.{hpp,cpp}`）：把"该不该起这一轮"抽成独立决策器 —— 意图分类 → 换话题判定 → ResponsePolicy → 是否打断/起轮次。**这是唯一决策源**，Orchestrator 只执行结论不重复实现规则（两处规则必然漂移）。抽出它的直接动因：真实语音链路要模型+音频设备才能跑，而"嗯/对/好不打断"这类自然度行为必须能脱离模型精确断言。④**WorkingContext 单一实例**：TurnAdmission 通过 `&context_.working()` 共享 ContextManager 那一份（否则"准入时清了一份、拼 prompt 读另一份"，指代消解永远对不上）。⑤**ContextManager 接入轮次**：`on_llm_complete_` 把本轮问答 `add_turn`；召回记忆从手拼 `"[相关记忆]"` 改为写入 P5 层，由 `build_system_prompt_with_context_` 统一按 P0~P7 拼装。⑥**后台任务 API**：`submit_background_task()` + `absorb_background_result_()`（结果一律入 cache，`should_speak=false` 绝不主动播报）。 |
| **验收结果** | ✅ MSVC `/W3` 零警告；`ctest` **9/9 通过**（新增 `test_turn_admission` 10 组）；`voice-agent.exe` 与全部既有测试无回归 |
| **遗留问题** | 前台轮次仍在当前线程同步跑（保序 + 首响应最快），未做成 Foreground Task —— 后台已完全异步化；`bg_cache_` 已写入但**还没有读取路径**（R3 才会让"引用旧搜索结果"生效）；ResponsePolicy 的 `needs_search/needs_agent` 只做了决策上报，**尚未据此自动派生后台任务**（R3）；ContextManager 的 P7 老历史只输出"已省略 N 轮"占位，未做摘要 |
| **文件** | `src/orchestrator/{turn_admission,orchestrator}.{hpp,cpp}`, `src/core/task.{hpp,cpp}`, `tests/test_turn_admission.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` |

### R3 - Fast Response + Background Agent ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 打通"Policy 决策 → 派生后台任务 → 抢答 → 结果落地 → 后续引用"闭环。①**FastResponseLayer**（`src/orchestrator/fast_response.{hpp,cpp}`）：按 Policy 结论 + **自适应延迟估计**决定要不要先垫一句。阈值不是硬编码 —— 用每类任务的 EWMA 滑动均值（`record_latency`/`estimate_latency_ms`），因为本地模型与在线 API 的耗时差异极大，固定 700ms 在某一边必然错。话术由 `ProgressUtterancePolicy` 轮换；Policy 判 Silence 时**绝不抢话**；同轮最多 1 句；打断后允许下一轮重新抢答。②**BackgroundCache**（`src/orchestrator/background_cache.{hpp,cpp}`）：补上 R1 遗留的"结果只写不读"缺口 —— 支持按 topic 精确取 + **关键词指代消解**（"刚才那个 Qwen3 的搜索结果"），带 TTL（过期宁缺勿给错的）、容量上限（LRU）、LRU 覆盖保留 hit_count。③**Orchestrator 接线**：`spawn_background_for_decision_()` 按 `needs_search`/`needs_memory` 派生后台任务（`search_router_` 走 SearchRouter、`memory_` 走 recall，**结果 should_speak=false 一律不主动播报**）；`speak_ack_()` 把抢答直接推 TTS（不进 LLM、不进对话历史）；`inject_cached_results_()` 在每轮把相关后台结果注入 P6 层供引用；`absorb_background_result_()` 改为写 BackgroundCache 并**回报实测耗时给 FastResponseLayer**（形成自适应闭环）。 |
| **验收结果** | ✅ MSVC `/W3` 零警告；`test_fast_response` **12 组断言全部 PASS** —— 覆盖"快任务闭嘴/慢任务抢答/静默时不抢/同轮只说一句/不复读/TTL/指代消解/长跑内存红线"；端到端用例验证「慢搜索 → 结果标记 superseded 落地 → 下一轮关键词引用成功」；**长跑红线**：5000 次写入后 cache 稳定在 32 条 / 6.7KB，不涨 |
| **遗留问题** | 后台结果**默认永不主动播报**（`should_speak=false` 硬编码）—— 用户问完就走、再也没人引用时结果就浪费了；需 R4 补"前台空闲且结果仍 relevant 时主动汇报"的策略；`search_router_` 目前只在 `attach_agent()` 时才有，**若上层未挂 agent 则搜索派生不生效**（GUI 的 `agent_controller.cpp` 无条件调用 `attach_agent`，故实际路径正常；纯文本/嵌入式使用需注意）；ContextManager 的 P6 层被后台结果占用后会覆盖本轮真实工具结果 |
| **文件** | `src/orchestrator/{fast_response,background_cache,orchestrator}.{hpp,cpp}`, `tests/test_fast_response.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` |

### R4 - Model Router（分档路由）✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 让 `ModelTier` 从"只写进事件供观测"变成**真正影响生成**。①**修 LLM 采样参数硬编码**（`src/llm/llm.cpp`）：原先 `llama_sampler_init_top_k(50)` / `top_p(0.95)` 在 `load()` 里写死，`LLMConfig` 里改什么都不生效 —— 抽出 `Impl::build_sampler_()` 统一构建，并给 `LLMConfig` 补 `top_k`/`top_p` 字段；新增 `LLM::apply_sampling()` 支持运行时切档（重建 sampler chain，不重载模型；生成中拒绝切换以免采样行为与已产出 token 不一致）。②**ModelRouter**（`src/orchestrator/model_router.{hpp,cpp}`）：`IModelEngine` 抽象 + 六档参数档位 + 引擎注册/可用性探测 + 统计（各档路由次数、降级次数）。**分档依据是语音场景而非通用 LLM 常识**：FAST 低温度短输出（抢答说错话比慢半秒糟糕）、DEEP 温度比 NORMAL 更低但输出更长（宁可慢也不能胡编）、AGENT 极低温度（工具参数不能编）。同档多候选时按注册顺序取首个可用者，**引擎缺失不静默失败而是降级并记录**。③**LocalLlamaEngine**（`src/orchestrator/model_engines.{hpp,cpp}`）：把现有 `LLM` 最小侵入地包装成 `IModelEngine`（不重写推理），profile 无变化时不触发 sampler 重建。④**Orchestrator 接线**：`initialize()` 把同一个本地 LLM 注册到全部六档（靠参数区分）；`enter_thinking_()` 按本轮 tier 调 `apply_sampling()` —— Search 档且有工具时自动升为 Agent 档（工具调用需稳定 JSON），Background 档不用于前台。 |
| **验收结果** | ✅ MSVC `/W3` 零警告；`test_model_router` **9 组断言全部 PASS** —— 含核心验收「简单问题 96 tok vs 深度档 1536 tok（16 倍差距）」、引擎不可用降级、整档缺失不崩、Policy→Router 打通；`ctest` **11/11 通过**，无回归 |
| **关键修复** | **Policy 情感倾诉误判**（探针发现，非测试暴露）：`looks_like_freshness_query` 含"最近"，导致"我最近真的有好多事情"被判成 `SEARCH` → 会去搜"最近"并返回一堆无关结果，极其不自然。修法：新增 `looks_like_emotional_smalltalk()`（情绪词表 + "无疑问词且无请求动词"启发式）并置于 Search 判定**之前**。已在 `test_runtime` 补 3 条回归断言 |
| **遗留问题** | 六个档位目前**共用同一个本地 LLM**，只靠采样参数区分 —— 真正的"在线强模型"（R7 的 OpenAI-compatible provider）尚未接入，DEEP 档目前只是"本地模型 + 低温度 + 长输出"；`ModelRouter::route()` 的降级链只做了"同档换引擎"，**未实现跨档降级**（DEEP→Normal→Fast 的完整链在 `route()` 里还没走，`allow_degrade` 目前只影响同档候选选择）；后台结果主动汇报策略仍缺（R3 遗留） |
| **文件** | `src/orchestrator/{model_router,model_engines,orchestrator}.{hpp,cpp}`, `src/llm/{llm.hpp,cpp}`, `src/orchestrator/response_policy.cpp`, `tests/{test_model_router,test_runtime}.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` |

### R5 - Context 分层收尾 + 跨档降级链 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 补 R3/R4 三笔欠账。①**ModelRouter 跨档降级链**（`degrade_chain()` + `route()` 重写）：此前只做同档候选选择，`DEEP→NORMAL→FAST` 这条链根本没走（头注释写了但实现没跟上）。降级只往"更便宜/更快"方向，**绝不升级** —— 否则"简单问题不烧重模型"会被反向破坏（有 DEEP 引擎时请求 FAST 会被抬到 DEEP，是错的）。全档缺失时不静默失败，用默认引擎出话。降级计入统计，且**统计按"请求档位"而非"实际用档"**——统计要回答的是"用户在问什么量级的问题"。②**P6 层多来源并存**：`tool_results_` 单一 vector 改为 `std::vector<ContextEntry>{source, content}`，新增 `add_tool_result()` / `add_background_result()` / `clear_results()`。原实现中后台缓存注入会**顶掉本轮真实工具结果**——那恰好是最不该丢的信息。每轮开始 `clear_results()`（内容本身在 BackgroundCache 里不会丢）。③**P7 老历史滚动摘要**：原先只输出"已省略 N 轮"干巴巴占位，模型会忘记前面聊过什么（长对话里表现为反复问同一件事）。改为增量滚动摘要 —— 超出窗口的轮次压缩成"问:… 答:…（前 60 字）"并入 `older_summary_`，有长度上限且**超长时裁最早部分**（保留最近的更符合当前话题）。 |
| **验收结果** | ✅ MSVC `/W3` 零警告；`test_runtime` 扩到 **13 组**（新增 P6 多来源 / P7 摘要 / P7 长跑红线），`test_model_router` 扩到 **14 组**（新增跨档降级 / 就近降级 / 不向上降级 / 可关闭降级 / 降级计数）；`ctest` **11/11 通过**。长跑红线：500 轮 → 摘要稳定 400 字符、turns 稳定 20 条 |
| **关键修复** | **ContextManager 自死锁**（既有 bug，R5 改 P7 时暴露）：`build_context()` 已持有 `mutex_`，却在 P7 分支调用 `has_older_history()` —— 后者内部 `lock(mutex_)`，而 **`std::mutex` 不可重入**。症状极具误导性：Release 下表现为 `0xC0000409`（STATUS_STACK_BUFFER_OVERRUN，栈保护误报），Debug 下是 `_XDEBUG_ASSERT`，**完全不像死锁**。曾先后误判为"Debug/Release 混链""预算耗尽越界""结构体布局错乱"，最后靠逐行插桩 + 单独调用 `snapshot()`/`turns()` 都正常、唯独 `build_context()` 崩，才定位到锁重入。修法：`build_context` 内直接读 `turns_`，不调会加锁的 getter |
| **遗留问题** | 六档仍共用同一个本地 LLM（只靠采样参数区分），**真正的在线强模型要等 R7** —— DEEP 档目前只是"本地模型+低温度+长输出"，名不副实；后台结果 `should_speak=false` 仍是硬编码，"前台空闲 + 结果仍 relevant 时主动汇报"的策略还缺；P7 摘要是**纯抽取式**（截前 60 字），不是 LLM 生成式摘要 —— 无需模型、零延迟，但质量有限 |
| **文件** | `src/orchestrator/model_router.{hpp,cpp}`, `src/core/working_context.{hpp,cpp}`, `src/orchestrator/orchestrator.cpp`, `tests/{test_runtime,test_model_router}.cpp` |

### R7 - Online Strong LLM（在线强模型）✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 让 DEEP 档名副其实 —— R0~R6 的分档都只靠采样参数，本质上还是同一个本地模型。①**WinHTTP HTTPS + SSE 客户端**（`src/util/http_winhttp.cpp` 新增，`src/util/http.{hpp,cpp}` 扩展）：原 `http.cpp` 只支持 `http://` 无 TLS，接不了任何在线 API。用系统自带 WinHTTP（Schannel TLS）补 https 与**流式读取** —— 拉 libcurl/CPR 会显著增加 Windows 部署负担，WinHTTP 只需链接 `winhttp.lib`。流式是硬需求：`WinHttpReadData` 前**不能设任何"收完 body"标志**（如 `ENABLE_READING`），否则整个流被缓冲，首响应延迟从几百毫秒变几秒。`Url` 结构扩展为记录 scheme 并提供 `effective_port()`/`is_tls()`/`host_port()`/`target()`。②**RemoteLLM**（`src/orchestrator/remote_llm.{hpp,cpp}`）：实现 `IModelEngine`，对接 OpenAI 兼容 `/chat/completions`。SSE 逐 token 转发（首响应延迟的关键）；取消真的生效（`stop()` + CancelToken 双通道，打断后必须立即停否则对着空气继续说）；**不把 `top_k`/`top_p` 发给服务端**（OpenAI 兼容 API 不认，硬塞会 400）；`available()` 含配置检查（未配置则 Router 降级）；失败不抛异常，走降级链。支持 tool calling 的 SSE 增量拼接（arguments 分片到达需跨帧累积）。③**Orchestrator 接线**：`attach_remote_llm()` 把远程引擎注册到 DEEP/AGENT/SEARCH（**默认不给 FAST/NORMAL** —— 简单问题坚决不碰网络）；`enter_thinking_` 按 `applied.engine` 前缀判断走远程还是本地，**降级后落到本地档时必须为本地**（否则会用远程请求回答本该本地快答的问题）；远程分支**刻意不走**本地 AgentLoop 工具链（远程 tool calling 与本地 GBNF grammar 是两套系统，混用产生难查的行为差异），工具结果通过 ContextManager 的 P6 层喂入；远程失败时**降级回本地继续答**而非让用户等在沉默里。④**配置**：`configs/agent.yaml` 新增 `remote_llm` 段；**密钥不落 yaml**（配置文件可能进 git），走环境变量 `VOICE_AGENT_REMOTE_API_KEY`。 |
| **验收结果** | ✅ MSVC `/W3` 零警告（`http_winhttp.cpp`/`remote_llm.cpp` 单独编译验证）；`ctest` **12/12 通过**（新增 `test_remote_llm` 9 组，不联网）—— 覆盖 https URL 解析、请求 JSON 组装（断言**不含** top_k/top_p）、工具定义、**SSE 文本增量**、**SSE 工具调用跨帧 arguments 拼接**、错误帧不崩、`available()` 门控、有/无远程的路由降级、FAST 档绝不走远程 |
| **遗留问题** | **未做真实联网端到端验证**（需要 API key，且当前不联网）—— SSE 解析逻辑已按 OpenAI 帧格式单测覆盖，但真实服务的分帧/错误格式可能有差异；远程的 tool calling 只做到"解析增量并回调"，**没有工具执行回路**（远程档目前只做对话+推理，工具结果靠 P6 层预置）；`RemoteLLM` 无连接池，每轮新建 WinHTTP session（延迟叠加在 TTFT 上，R8 要优化）；`do_stream` 里 `WINHTTP_FLAG_SECURE` 由 `open_request` 自动补，但代理配置（`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`）在部分企业环境可能需要显式指定 |
| **文件** | `src/util/http.{hpp,cpp}`, `src/util/http_winhttp.cpp`, `src/orchestrator/remote_llm.{hpp,cpp}`, `src/orchestrator/{orchestrator,model_engines}.{hpp,cpp}`, `src/core/types.hpp`, `src/util/config.hpp`, `src/gui/agent_controller.cpp`, `configs/agent.yaml`, `tests/test_remote_llm.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` |

### R6 - Prosody / TTS 韵律层 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 把"说什么"（LLM）与"怎么说"（TTS）彻底分离。①**Prosody**（`src/tts/prosody.{hpp,cpp}`）：用 **6 个连续变量**（energy/warmth/certainty/urgency/pace/pause_density）替代 happy/sad/angry 粗标签 —— 粗标签无法表达"用户有点烦但我理解"这类真实状态，且换 TTS 时全要重做映射。情绪→韵律是**小幅偏移而非覆盖**（情绪是染色不是换人），并支持 `intensity` 插值：**半吊子的情绪比没有情绪更像机器人**，所以强度不足时完全保持中性。②**句级切分**（`split_sentences`）：主标点切句、连续标点并入同段、超长文本按次级标点二次切分、**纯标点段必须丢弃**（否则会合成一段没有内容的音频）。粒度取"句"而非"词"的原因写进注释：逐 token 直灌会让 TTS 总耗时暴增，且 SAPI 一次 Speak 会清空读本、逐字喂会互相打断只读几字。③**ResponsePlan**（`src/tts/response_plan.{hpp,cpp}`）：管理分段生命周期（Pending/Synthesized/Playing/Played/Discarded），实现 **Response Revision** —— *已播出的不可改，未播出的可丢弃重生成*。这是模型"意识到自己说错"时能修正的原理。④**ITtsAdapter**（`src/tts/tts_adapter.{hpp,cpp}`）：把统一韵律语义映射到各引擎的具体参数。Kokoro/Piper 只有语速（energy 只能微推速度，"有劲"和"快"在不同后端上听感难以分离）；SAPI 有 Rate/Volume/Pitch，映射能力更强但音调变化刻意压在 ±5% 以内（超过就"阴阳怪气"）。`supports_full_prosody()` 让上层知道该引擎能否表达完整维度。⑤**链路接入**（`src/orchestrator/orchestrator.{hpp,cpp}`）：`on_llm_token_` 改走 `feed_speech_text_()` → `ProsodyPlanner::split_sentences` → `plan_segment`（加韵律）→ `ResponsePlan::append` → `pump_next_segment_()` → `ITtsAdapter::synthesize`；`on_tts_chunk_(is_last)` 推进播放游标并连播下一段；`on_llm_complete_` 调 `flush_speech_tail_()` 补尾句（无标点也播出来）。**`stream_sentence_` 裸缓冲与 `find_sentence_boundary()`/`utf8_hold_back()` 一并删除** —— 切分逻辑两处并存必然漂移。`initialize()` 按实际引擎建 adapter（SAPI 与模型引擎参数能力不同）；`enter_thinking_`/`interrupt_agent_`/`set_tts_enabled(false)` 三处都重置 `ResponsePlan`（新一轮/被打断/关闭播报都不能把旧内容接着念）；情绪在新一轮复位（上一轮情绪不该延续）。 |
| **验收结果** | ✅ MSVC `/W3` 零警告（修掉一处 float→int 转换警告）；`test_prosody` **12 组断言全部 PASS** —— 在原有 10 组基础上新增「**端到端 token 流 → 切段 → 韵律 → ResponsePlan**」（逐 token 喂入模拟 LLM 流，验证产出 3 段且段文本正确、无残留泄漏）与「情绪对最终段的影响（节奏/停顿）」；`ctest` **13/13 通过** |
| **关键设计** | `prosody_for_emotion` 的偏移量刻意做小（测试断言 `< 0.35`）：`happy` 相对中性只 +0.18 energy，若做成覆盖会导致每句话音色突变，反而更像机器人。`user_irritated` 单独定义（energy↓ pace↓ urgency↓ warmth↑）—— 用户已经烦了，再用"热情"回应是火上浇油 |
| **遗留问题** | **`Qwen3-TTS` / `Chatterbox` 两个 adapter 尚未实现**（只有 `ITtsAdapter` 接口与 Kokoro/SAPI 两个实现留口）—— 需要下载模型并实测才能做，接口已就位随时可加；`SapiTtsAdapter` 的 volume/pitch 映射出来了但 SAPI 无运行时接口，只能走 TTSConfig，故实际只有语速生效（已在代码注释里写明，避免"以为改了其实没改"的隐性 bug）；韵律的 `pause_before_ms` 目前只记录在 `SpeechSegment` 上，**未被播放链路消费** —— SAPI 是整段朗读无法插入段间静音，需要将来在 AudioRouter 侧实现；`set_emotion()` 目前无调用方（应由 Policy/GUI 根据用户状态设置），默认走中性；**未做真人听感验证** —— 韵律参数的具体数值只能在真实播放中调，纯逻辑测试无法覆盖"听着自然吗" |
| **文件** | `src/tts/{prosody,response_plan,tts_adapter}.{hpp,cpp}`, `src/orchestrator/orchestrator.{hpp,cpp}`, `tests/test_prosody.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` |

### R8 - 可观测与长跑稳定性 ✅
| 项目 | 内容 |
|------|------|
| **完成日期** | 2026-10-03 |
| **完成内容** | 让"自然度靠不靠得住"可量化，而不是靠"主观感觉不错"。①**VoiceTrace**（`src/util/voice_trace.{hpp,cpp}`）：每轮对话写一行 JSONL（决策 + 路由 + 延迟 + 完整时序事件）。**流式落盘、每轮立即 flush** —— 长跑崩溃时已完成的轮次不丢。`VoiceTraceReader` 可读回并计算**分位数**（p50/p90/p99，最近秩法：小样本下比线性插值稳且不会插出没测到的值）。启动时写 session header（引擎/后端/远程是否挂载），离线回放时能知道是哪套配置。②**LatencyMetrics**（`src/util/latency_metrics.{hpp,cpp}`）：10 类指标的聚合器（speech→asr、asr→first audio、LLM TTFT、TTS first audio、轮次总耗时、**barge-in 延迟**、EOU 延迟、工具/搜索/记忆/合成耗时）。**定长环形缓冲 + 全量累计**（分位数用样本、count/sum/min/max 用全量），内存有界。默认预算按"用户能否忍受"设定而非技术指标，**barge-in < 250ms** 直接对应实施计划里的硬指标。③**WinHTTP 连接池**（`src/util/http_winhttp.cpp`）：原先每轮新建 session，TCP+TLS 握手动辄 100~300ms，**这部分开销直接叠在 TTFT 上**。改为按 `host:port` 缓存 session+connect（WinHTTP 内部会做 keep-alive），请求句柄仍每次新建（这是正确用法）。④**Orchestrator 接线**：`trace_path` 配置项（空=关闭零开销）；关键时点埋点（首 token、首段音频、barge-in、轮次收尾）；`latency_summary()` 输出一行摘要。 |
| **验收结果** | ✅ MSVC `/W3` 零警告；`test_long_run` **7 组全部 PASS** —— 覆盖：2000 任务无泄漏（工作集增长 < 64MB）、20 轮 start/stop 幂等、**50000 次指标记录内存增长 < 32MB**、3000 轮 trace 写读往返 + 分位数、**500 任务并发 cancel+supersede 零卡死**、20000 次操作后各容器仍在容量红线内、5000 轮对话循环工作集增长 < 48MB；`ctest` **14/14 通过** |
| **关键修复** | **匿名 namespace 花括号错位**（R6 尾巴删 `find_sentence_boundary()` 时引入）：删除时把 `sentence_has_content()` 插到了 `}  // namespace` **之后**，导致多出一个右花括号 —— 整个文件后续代码落进畸形命名空间。症状极具误导性：MSVC 报 `ModelTier: left of '::' must be a class/struct/union`，而 `ModelTier` 在同一文件的探针程序里完全可见、枚举定义也确认无误。先后误判为"陈旧 obj"、"宏冲突"、"include 缺失"，最后靠"读报错行之前的结构"发现多了一个 `}`。教训：**报"X 不是类"且 X 确实可见时，往前看结构，别改报错行** |
| **遗留问题** | **barge-in 延迟的测量点不精确** —— 目前从 `on_vad_speech_start`（VAD 检测到语音）算起，而不是从用户实际开口算起。VAD 本身有 100~300ms 的检测延迟，所以这个指标**偏乐观**；真实值需要硬件级时间戳（R8 后续补）。**未做真实 1h/4h/8h 长跑** —— `test_long_run` 是加速版（同等操作量压缩到秒级），真实的长时间运行还需要：真实音频设备连续占用、模型常驻显存、真实网络请求。这三项都需要实际环境，当前无法在无 GPU/无音频的环境里验证。远程链路的连接池**未做真实验证**（需要 API key）；`pause_before_ms` 仍未被播放链路消费（见 R6 遗留） |
| **文件** | `src/util/{voice_trace,latency_metrics}.{hpp,cpp}`, `src/util/http_winhttp.cpp`, `src/orchestrator/orchestrator.{hpp,cpp}`, `tests/test_long_run.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` |

### M7 - MCP 集成
| 项目 | 内容 |
|------|------|
| **状态** | ⏳ 待开始 |
| **计划内容** | MCP C++ SDK、工具注册 |
| **验收标准** | 能连接外部 MCP Server |

### M8 - 优化与调参
| 项目 | 内容 |
|------|------|
| **状态** | ⏳ 待开始 |
| **计划内容** | 延迟调优、显存优化、稳定性测试 |
| **验收标准** | 30 分钟无泄漏，延迟达标 |

---

## 待下载模型

| 模型 | 用途 | 大小 | 路径 |
|------|------|------|------|
| ten-vad | VAD | ~2MB | models/ |
| silero_vad | VAD 复核 | 629KB ✅已下载 | models/vad/silero_vad.onnx |
| smart_turn | 轮次判定 | ~8MB | models/ |
| whisper-tiny | ASR（当前启用） | ~78MB ✅已下载 | models/asr/whisper-tiny/ |
| sense_voice | ASR | ~300MB | models/ |
| paraformer | ASR 流式 | ~300MB | models/ |
| kokoro | TTS | ~300MB | models/ |
| qwen3-1.5b-q4 | LLM（Intel Arc 2GB 建议） | ~1GB | models/ |
| qwen3-4b-q4 | LLM（大显存） | ~2.5GB | models/ |

---

## 已知问题

1. **Intel Arc 显存限制**：2GB，建议用 Qwen3-1.5B Q4
2. **GPU 加速**：需要 Vulkan 后端支持，后续测试 llama.cpp GGML_VULKAN
3. **Windows 构建**：需要 MinGW-w64 或 MSVC 2019+
4. **TTS(Kokoro) 无法在 Intel Arc 上跑 GPU（关键结论）**：经实测确认，强制 Kokoro 走 ONNX Runtime DirectML 在 Intel Arc 上一合成即触发驱动级栈溢出崩溃（`0xC0000409` / STATUS_STACK_BUFFER_OVERRUN，引擎能创建、运算时崩溃，不可 try-catch）。根因是 Kokoro 模型的 **grouped ConvTranspose** 算子与 Intel Arc 的 DirectML 驱动不兼容。已排除的因素：①驱动版本——升级 Intel Arc 驱动 8243→9030（2026-09-17，全新版）后仍稳定复现；②包完整性——官方 SHA512 校验完全一致；③杀软（火绒）、磁盘空间、.NET。**驱动已升级到 9030（含 system 部署、git 无关），投屏等显卡功能正常**。当前方案为启动时检测 Intel 适配器自动回退 CPU 合成（约 3s/4s 语音，对话场景可接受）；若需 TTS 真正上 GPU，须换 grouped ConvTranspose 之外的模型（如 VITS/Matcha/Piper 系 sherpa-onnx 模型）
