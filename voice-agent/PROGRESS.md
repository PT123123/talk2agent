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
