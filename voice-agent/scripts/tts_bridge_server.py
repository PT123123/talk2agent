#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
voice-agent TTS 推理 bridge（Qwen3-TTS / Chatterbox）
====================================================

为什么需要它
------------
Qwen3-TTS（QwenLM）与 Chatterbox（ResembleAI）都是 **PyTorch** 模型，没有原生
C++ 推理后端。本机的voice-agent 是纯 C++/Qt 工程，直接把权重塞进 sherpa-onnx /
ONNX Runtime 是行不通的（sherpa-onnx 只认 VITS/Matcha/Kokoro 系）。

因此采用业界通用做法：**C++ 侧保留 Agent 逻辑与韵律计算，神经网络推理交给一个
独立的本地 Python 进程**，两者用 localhost HTTP + JSON 通信。

    ┌────────────────────┐   POST /synthesize  {text, instruction, speed, ...}
    │  voice-agent (C++) │  ────────────────────────────────────────────▶┌──────────────┐
    │  Agent / Prosody   │◀────────────────────────────────────────────   │ bridge 服务   │
    │  适配器(算 controls)│   audio/wav (int16 PCM)                │ qwen-tts /   │
    └────────────────────┘                                            │ chatterbox    │
                                                                      └──────────────┘

这个方案的边界（诚实声明）
--------------------------
1. **进程边界是真实的**：Python 崩溃/OOM 不会带走 C++ 主进程，只是这一轮没声音。
   代价是多一次 localhost 往返（同机约 0.3~2ms，可忽略）。
2. **推理仍需 GPU/CPU 真实环境**。本机（Intel Arc 2GB）跑 Qwen3-TTS 0.6B
   （fp32 约 2.5GB 权重）显存不够，需要 fp16 + CPU offload，或换 1.7B 之外的
   小模型。**本脚本在没有权重/没有 torch 的机器上也能启动**（会明确报
   `backend_unavailable`），便于先验证链路再装环境。
3. **绝不在这里做重试/降级到 SAPI**：让C++ 侧决定回退，Python 只报告失败原因。

安装
----
    # Qwen3-TTS（官方建议 Python 3.12 独立环境）
    pip install -U qwen-tts
    # Chatterbox（二选一即可，两个可共存）
    pip install chatterbox-tts

    # 强烈建议装 soundfile 用于写 WAV；没装则退回内置 wave 模块
    pip install soundfile

启动
----
    python scripts/tts_bridge_server.py --engine qwen3tts --port 8770
    python scripts/tts_bridge_server.py --engine chatterbox --port 8770

模型权重路径（本地已有则不再联网下载）：
    --model-path models/tts/qwen3tts
    --model-path models/tts/chatterbox

接口
----
GET  /health
    → {"ok":true,"engine":"qwen3tts","backend_ready":false,"detail":"..."}
    backend_ready=false 时C++ 侧应回退到底层声学引擎。

POST /synthesize
    请求：{"text":"你好","instruction":"温柔亲切","speed":1.05,"voice":"",
           "exaggeration":0.5,"cfg_weight":0.5,"lang":"zh","sample_rate":24000}
    成功：200，Content-Type: audio/wav，body 为单声道 int16 PCM 的 WAV
    失败：4xx/5xx，Content-Type: application/json，{"error":"..."}

POST /interrupt
    →打断当前正在进行的合成（下一轮 barge-in 用）。
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional, Tuple

# 采样率：voice-agent 播放链路固定 24kHz（见 orchestrator.hpp playback_sample_rate）
DEFAULT_SAMPLE_RATE = 24000
DEFAULT_PORT = 8770

# 单次合成文本长度上限。C++侧已按句切分（ProsodyPlanner::split_sentences），
# 这个上限只是防御性的——超长文本会让推理时间线性爆炸。
MAX_CHARS = 500


# ============================================================
# WAV 写入（优先 soundfile，退回标准库 wave）
# ============================================================
def pcm16_to_wav(pcm: bytes, sample_rate: int) -> bytes:
    """把单声道 int16 PCM 打包成 WAV（44字节头+ data）。

    优先用 soundfile；没装就用标准库 wave（纯标准库，无二进制依赖）。
    两者产出的都是标准 16-bit PCM WAV，C++ 侧解析逻辑一致。
    """
    try:
        import io

        import numpy as np
        import soundfile as sf

        audio = np.frombuffer(pcm, dtype=np.int16)
        buf = io.BytesIO()
        sf.write(buf, audio, samplerate=sample_rate, subtype="PCM_16",
                 format="WAV")
        return buf.getvalue()
    except Exception:
        # 没装 soundfile（或 numpy）就用标准库 wave —— 纯标准库，零额外依赖。
        pass

    import io
    import wave

    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


# ============================================================
# 后端封装
# ============================================================
class TTSBackend:
    """统一封装两个 PyTorch TTS 的加载与推理。

    刻意做成"懒加载"：HTTP 服务立刻起来（秒级），
    第一次 /synthesize 才付出几十秒的模型加载代价。
    这样 C++ 侧的健康探测不会被模型加载阻塞住——
    探测只回答"能不能用"，不能回答"现在就能用吗"。
    """

    def __init__(self, engine: str, model_path: Optional[str],
                 device: str, dtype: str):
        self.engine = engine
        self.model_path = model_path
        self.device = device
        self.dtype = dtype
        self._model = None
        self._impl = None
        self._load_lock = threading.Lock()
        self._interrupt = threading.Event()
        # "loading" / "ready" / "error" —— 与 backend_ready 分开报，
        # 因为"正在加载几十秒"和"加载失败"对调用方是完全不同的处置。
        self._state = "loading"
        self.last_error: str = ""

    # ---- 加载 ----
    def load(self) -> Tuple[bool, str]:
        """幂等加载。返回 (是否成功, 详情)。失败不抛异常，只记录原因。"""
        if self._model is not None:
            return True, "already loaded"
        with self._load_lock:
            if self._model is not None:
                return True, "already loaded"
            self._state = "loading"
            try:
                if self.engine == "qwen3tts":
                    ok, detail = self._load_qwen3()
                elif self.engine == "chatterbox":
                    ok, detail = self._load_chatterbox()
                else:
                    self._state = "error"
                    return False, f"unknown engine: {self.engine}"
                self._state = "ready" if ok else "error"
                return ok, detail
            except Exception as exc:  # 加载失败要能看到根因
                self._state = "error"
                self.last_error = f"{type(exc).__name__}: {exc}"
                traceback.print_exc()
                return False, self.last_error

    def _resolve_dtype(self, torch):
        # dtype 字符串 → torch dtype。fp16 在 Intel Arc 上通常没有有效的
        # 半加速但省显存；用户显式指定才用，默认 fp32 最稳。
        return {
            "fp32": torch.float32,
            "fp16": torch.float16,
            "bf16": torch.bfloat16,
        }.get(self.dtype, torch.float32)

    def _load_qwen3(self) -> Tuple[bool, str]:
        try:
            from qwen_tts import Qwen3TTSModel
        except ImportError:
            return False, ("qwen-tts 未安装：pip install -U qwen-tts。"
                           "（本bridge 本身可正常启动，仅该引擎不可用）")
        import torch

        path = self.model_path or "Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice"
        self._model = Qwen3TTSModel.from_pretrained(
            path, device_map=self.device, dtype=self._resolve_dtype(torch))
        self._impl = "qwen3"
        return True, f"qwen-tts loaded from {path}"

    def _load_chatterbox(self) -> Tuple[bool, str]:
        try:
            from chatterbox.mtl_tts import ChatterboxMTL
        except ImportError:
            try:
                from chatterbox.tts import ChatterboxTTS as ChatterboxMTL  # 旧版
            except ImportError:
                return False, ("chatterbox-tts 未安装："
                               "pip install chatterbox-tts。"
                               "（本 bridge 本身可正常启动，仅该引擎不可用）")
        self._model = ChatterboxMTL.from_pretrained(
            self.device, self.model_path)
        self._impl = "chatterbox"
        return True, f"chatterbox loaded from {self.model_path or 'default'}"

    # ---- 推理 ----
    def synthesize(self, req: Dict[str, Any]) -> Tuple[Optional[bytes], str]:
        """返回 (pcm_bytes, error)。成功时 error 为空串。

        注意：这里**不做任何重试**。失败就把原因如实回给 C++，
        由上层决定回退到哪个引擎（隐藏重试会让"这一轮为什么没声音"变成玄学）。
        """
        ok, detail = self.load()
        if not ok:
            return None, detail

        text = (req.get("text") or "").strip()
        if not text:
            return None, "empty text"
        if len(text) > MAX_CHARS:
            # 不静默截断——截断后的音频会以"话没说完"的形式播出去，
            # 那是更难排查的问题。直接报错让上层重新切句。
            return None, f"text too long: {len(text)} > {MAX_CHARS}"

        self._interrupt.clear()
        t0 = time.perf_counter()
        try:
            if self._impl == "qwen3":
                wav, sr = self._synth_qwen3(text, req)
            else:
                wav, sr = self._synth_chatterbox(text, req)
        except Exception as exc:
            self.last_error = f"{type(exc).__name__}: {exc}"
            traceback.print_exc()
            return None, self.last_error

        if self._interrupt.is_set():
            # 被 barge-in 打断：这不是错误，返回空让上层静默丢弃即可。
            return None, "interrupted"

        want_sr = int(req.get("sample_rate") or DEFAULT_SAMPLE_RATE)
        pcm = self._to_pcm16(wav, sr, want_sr)
        if pcm is None:
            return None, "failed to convert backend output to int16 PCM"
        elapsed = (time.perf_counter() - t0) * 1000.0
        print(f"[bridge] synth ok: {len(text)} chars -> "
              f"{len(pcm) // 2} samples @{want_sr}Hz in {elapsed:.0f}ms",
              flush=True)
        return pcm, ""

    def _synth_qwen3(self, text: str, req: Dict[str, Any]):
        """Qwen3-TTS：instruction（自然语言风格）+ voice（发音人）。

        这是它与其它引擎的本质差异——风格靠**自然语言提示**表达，
        而不是数值旋钮，所以上层算出的 instruction 必须真的喂进来。
        """
        kwargs: Dict[str, Any] = {}
        instruction = (req.get("instruction") or "").strip()
        voice = (req.get("voice") or "").strip()
        if instruction:
            kwargs["instruct"] = instruction
        if voice:
            kwargs["speaker"] = voice

        lang = (req.get("lang") or "zh").strip()
        try:
            wav = self._model.generate_custom_voice(text, language=lang, **kwargs)
        except TypeError:
            # 某些版本签名不含 language / speaker，逐级退化而不是直接失败
            wav = self._model.generate_custom_voice(text, **kwargs)
        return self._as_numpy_wav(wav)

    def _synth_chatterbox(self, text: str, req: Dict[str, Any]):
        """Chatterbox：exaggeration（情绪强度）+ cfg_weight（克制程度）。

        上层 map_prosody_for_chatterbox 算出的两个旋钮在这里真正生效——
        这正是"适配器存在的意义"：统一韵律语义 → 各引擎原生参数。
        """
        exaggeration = float(req.get("exaggeration", 0.5))
        cfg_weight = float(req.get("cfg_weight", 0.5))
        # 钳制到官方推荐区间。上层已钳制过，这里再钳一次是防御外部调用。
        exaggeration = min(1.0, max(0.0, exaggeration))
        cfg_weight = min(1.0, max(0.1, cfg_weight))

        wav = self._model.generate(
            text,
            exaggeration=exaggeration,
            cfg_weight=cfg_weight,
        )
        return self._as_numpy_wav(wav)

    @staticmethod
    def _as_numpy_wav(wav):
        """把后端五花八门的返回统一成 (float32 ndarray, sample_rate)。

        各版本可能返回：torch.Tensor / np.ndarray / (ndarray, sr) 元组。
        """
        import numpy as np

        sr = DEFAULT_SAMPLE_RATE
        if isinstance(wav, tuple):
            wav, maybe_sr = wav[0], wav[1]
            if maybe_sr:
                sr = int(maybe_sr)
        if hasattr(wav, "detach"):          # torch.Tensor
            wav = wav.detach().cpu().numpy()
        wav = np.asarray(wav, dtype=np.float32)
        if wav.ndim > 1:
            wav = wav.reshape(-1)          # 多声道取平（voice-agent 是单声道播放）
        return wav, sr

    @staticmethod
    def _to_pcm16(wav, src_sr: int, dst_sr: int):
        """float [-1,1] → int16，必要时做线性重采样。

        重采样用线性插值而非 scipy：语音 24kHz 场景下线性插值的可闻差异极小，
        而 scipy 是额外依赖。采样率一致时直接走快速路径。
        """
        import numpy as np

        if wav is None or wav.size == 0:
            return None
        wav = np.clip(wav, -1.0, 1.0)
        if src_sr and dst_sr and src_sr != dst_sr:
            n_out = int(round(wav.size * (dst_sr / float(src_sr))))
            if n_out <= 0:
                return None
            x_old = np.linspace(0.0, 1.0, wav.size, endpoint=False)
            x_new = np.linspace(0.0, 1.0, n_out, endpoint=False)
            wav = np.interp(x_new, x_old, wav)
        return (wav * 32767.0).astype(np.int16).tobytes()

    def interrupt(self) -> None:
        self._interrupt.set()


# ============================================================
# HTTP handler
# ============================================================
class Handler(BaseHTTPRequestHandler):
    # 由 serve() 注入
    backend: TTSBackend = None  # type: ignore[assignment]
    protocol_version = "HTTP/1.1"   # 开启 keep-alive，省掉每轮 TCP 握手

    # 静音默认的逐请求日志（真正的日志走 print(flush=True)）
    def log_message(self, fmt: str, *args: Any) -> None:
        return

    # ---- helpers ----
    def _send_json(self, code: int, obj: Dict[str, Any]) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_audio(self, pcm: bytes, sample_rate: int) -> None:
        wav = pcm16_to_wav(pcm, sample_rate)
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(wav)))
        self.send_header("X-Sample-Rate", str(sample_rate))
        self.end_headers()
        self.wfile.write(wav)

    def _read_json(self) -> Dict[str, Any]:
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        return json.loads(raw.decode("utf-8"))

    # ---- routes ----
    def do_GET(self) -> None:  # noqa: N802
        if self.path.rstrip("/") in ("/health", ""):
            # 刻意**不**触发模型加载：健康探测必须毫秒级返回，
            # 否则 C++ 启动流程会被几十秒的权重加载卡住。
            ready = self.backend._model is not None
            detail = {
                "ready": "loaded",
                "loading": "loading weights (first run can take a minute)",
                "error": self.backend.last_error or "backend unavailable",
            }.get(self.backend._state, "unknown state")
            self._send_json(200, {
                "ok": True,
                "engine": self.backend.engine,
                "backend_ready": ready,
                # 显式状态：调用方据此区分"再等等"与"放弃回退"。
                "backend_state": self.backend._state,
                "detail": detail,
                "device": self.backend.device,
            })
            return
        self._send_json(404, {"error": f"unknown path: {self.path}"})

    def do_POST(self) -> None:  # noqa: N802
        path = self.path.rstrip("/")
        try:
            req = self._read_json()
        except Exception as exc:
            self._send_json(400, {"error": f"bad json: {exc}"})
            return

        if path == "/interrupt":
            self.backend.interrupt()
            self._send_json(200, {"ok": True})
            return

        if path == "/warmup":
            # 可选：提前触发加载，避免第一轮用户等几十秒
            ok, detail = self.backend.load()
            self._send_json(200 if ok else 503,
                            {"ok": ok, "detail": detail})
            return

        if path != "/synthesize":
            self._send_json(404, {"error": f"unknown path: {self.path}"})
            return

        pcm, err = self.backend.synthesize(req)
        if pcm is None:
            code = 503 if "未安装" in err or "unavailable" in err else 500
            # interrupted 不是错误：barge-in 之后上层会静默丢弃这一段
            if err == "interrupted":
                self._send_json(409, {"error": "interrupted"})
            else:
                self._send_json(code, {"error": err})
            return

        self._send_audio(pcm, int(req.get("sample_rate") or DEFAULT_SAMPLE_RATE))


def serve(engine: str, model_path: Optional[str], port: int,
          device: str, dtype: str) -> None:
    backend = TTSBackend(engine, model_path, device, dtype)
    Handler.backend = backend

    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    httpd.daemon_threads = True

    print(f"[bridge] engine={engine} device={device} dtype={dtype} "
          f"model_path={model_path or '(from repo id)'}", flush=True)
    print(f"[bridge] listening on http://127.0.0.1:{port}", flush=True)
    print("[bridge] endpoints: GET /health | POST /synthesize | "
          "POST /warmup | POST /interrupt", flush=True)

    # 关键：**先监听，再后台预加载**。
    # 反过来（先加载完再 listen）会出现启动竞态：模型加载要几十秒，
    # 这期间端口是关的，C++ 侧健康探测连接被拒 → 判定 bridge 不可用 →
    # 整个会话永久降级到 SAPI，等模型加载完了也用不上。
    # 现在端口秒开，加载期间 /health 报 backend_state="loading"，
    # 调用方能区分"再等等"和"没戏了，回退"。
    def _preload() -> None:
        ok, detail = backend.load()
        print(f"[bridge] backend preflight: "
              f"{'OK - ' + detail if ok else detail}", flush=True)
        if not ok:
            print("[bridge] 服务仍在运行（/health 会返回 "
                  "backend_state=\"error\"），C++ 侧会自动回退到底层声学引擎。",
                  flush=True)

    threading.Thread(target=_preload, daemon=True).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] shutting down", flush=True)
    finally:
        httpd.server_close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="voice-agent TTS 推理 bridge (Qwen3-TTS / Chatterbox)")
    ap.add_argument("--engine", required=True,
                    choices=["qwen3tts", "chatterbox"],
                    help="要服务的 TTS 引擎")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"监听端口（默认 {DEFAULT_PORT}，仅 127.0.0.1）")
    ap.add_argument("--model-path", default=None,
                    help="本地权重目录；不给则用官方 repo id 自动下载")
    ap.add_argument("--device", default="cpu",
                    help="torch device：cpu / cuda:0")
    ap.add_argument("--dtype", default="fp32", choices=["fp32", "fp16", "bf16"],
                    help="权重精度（显存紧张用 fp16）")
    args = ap.parse_args(argv)

    serve(args.engine, args.model_path, args.port, args.device, args.dtype)
    return 0


if __name__ == "__main__":
    sys.exit(main())