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
| silero_vad | VAD 复核 | ~1MB | models/ |
| smart_turn | 轮次判定 | ~8MB | models/ |
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
