"""
火山引擎同传 - 云端 HTTP 服务器
ESP32 AR 眼镜通过 HTTP 发送音频，服务器转发给火山引擎翻译

API:
  POST /start_session          — 开始新会话
  POST /translate              — 发送音频，返回翻译结果
  POST /stop_session/<id>      — 结束会话

启动: python cloud_server.py
"""

import asyncio
import websockets
import numpy as np
import uuid
import sys
import os
import threading
import queue
from flask import Flask, request, jsonify

# --- 导入火山引擎 Protobuf ---
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python_protogen"))
try:
    from products.understanding.ast.ast_service_pb2 import TranslateRequest, TranslateResponse
    from common.events_pb2 import Type
    print("[OK] Protobuf loaded")
except ImportError as e:
    print(f"[ERROR] Protobuf not found: {e}")
    sys.exit(1)

# --- 火山引擎配置 ---
API_KEY = "8f61b5ed-8636-4d00-bb0c-17748bd45f1e"
RESOURCE_ID = "volc.service_type.10053"
WS_URL = "wss://openspeech.bytedance.com/api/v4/ast/v2/translate"

SAMPLE_RATE = 16000
CHANNELS = 1

app = Flask(__name__)

# 会话管理
sessions = {}  # session_id -> {ws, latest_source, latest_translation, audio_queue, loop}


def create_start_request(session_id, source_lang="en", target_lang="zh"):
    req = TranslateRequest()
    req.event = Type.StartSession
    req.request_meta.SessionID = session_id
    req.user.uid = "ar_glass_server"
    req.user.did = "ar_glass_server"
    req.source_audio.format = "pcm"
    req.source_audio.rate = SAMPLE_RATE
    req.source_audio.bits = 16
    req.source_audio.channel = CHANNELS
    req.target_audio.format = "pcm"
    req.target_audio.rate = 24000
    req.request.mode = "s2s"
    req.request.source_language = source_lang
    req.request.target_language = target_lang
    req.request.speaker_id = "en_female_amazon_kristy"
    return req.SerializeToString()


def create_audio_request(session_id, audio_chunk):
    req = TranslateRequest()
    req.event = Type.TaskRequest
    req.request_meta.SessionID = session_id
    req.source_audio.format = "pcm"
    req.source_audio.rate = SAMPLE_RATE
    req.source_audio.bits = 16
    req.source_audio.channel = CHANNELS
    req.source_audio.binary_data = audio_chunk
    return req.SerializeToString()


def create_finish_request(session_id):
    req = TranslateRequest()
    req.event = Type.FinishSession
    req.request_meta.SessionID = session_id
    return req.SerializeToString()


def parse_response(data):
    resp = TranslateResponse()
    resp.ParseFromString(data)
    return resp


async def ws_receiver(ws, session_data):
    """接收火山引擎返回的翻译结果"""
    async for message in ws:
        resp = parse_response(message)

        if resp.event == Type.SessionStarted:
            session_data["ready"] = True
            print(f"  [{session_data['id'][:8]}] Session started")

        elif resp.event == Type.SourceSubtitleEnd:
            if resp.text:
                session_data["latest_source"] = resp.text
                print(f"  [{session_data['id'][:8]}] Source: {resp.text}")

        elif resp.event == Type.TranslationSubtitleEnd:
            if resp.text:
                session_data["latest_translation"] = resp.text
                print(f"  [{session_data['id'][:8]}] Translation: {resp.text}")

        elif resp.event == Type.SessionFailed:
            msg = resp.response_meta.Message if hasattr(resp, 'response_meta') else "unknown"
            print(f"  [{session_data['id'][:8]}] Failed: {msg}")
            break

        elif resp.event == Type.SessionFinished:
            print(f"  [{session_data['id'][:8]}] Finished")
            break


async def ws_sender(ws, session_data):
    """发送音频数据到火山引擎"""
    while True:
        try:
            audio_chunk = await asyncio.wait_for(
                session_data["audio_queue"].get(), timeout=30)
        except asyncio.TimeoutError:
            continue
        except Exception:
            break

        if audio_chunk is None:
            break

        data = create_audio_request(session_data["id"], audio_chunk)
        await ws.send(data)


async def run_ws_session(session_id, source_lang, target_lang):
    """运行一个 WebSocket 会话"""
    session_data = {
        "id": session_id,
        "ready": False,
        "latest_source": "",
        "latest_translation": "",
        "audio_queue": asyncio.Queue(),
    }
    sessions[session_id] = session_data

    headers = {
        "X-Api-Key": API_KEY,
        "X-Api-Resource-Id": RESOURCE_ID,
        "X-Api-Connect-Id": str(uuid.uuid4()),
    }

    try:
        async with websockets.connect(
            WS_URL,
            additional_headers=headers,
            max_size=1000000000,
            ping_interval=None
        ) as ws:
            session_data["ws"] = ws

            start_req = create_start_request(session_id, source_lang, target_lang)
            await ws.send(start_req)

            send_task = asyncio.create_task(ws_sender(ws, session_data))
            recv_task = asyncio.create_task(ws_receiver(ws, session_data))

            await asyncio.gather(send_task, recv_task)

    except Exception as e:
        print(f"  [{session_id[:8]}] Error: {e}")
    finally:
        session_data["ready"] = False


# --- Flask 路由 ---

@app.route("/start_session", methods=["POST"])
def start_session():
    """开始新的翻译会话"""
    session_id = str(uuid.uuid4())
    source_lang = request.json.get("source_lang", "en") if request.is_json else "en"
    target_lang = request.json.get("target_lang", "zh") if request.is_json else "zh"

    loop = asyncio.new_event_loop()
    session_thread = threading.Thread(
        target=lambda: loop.run_until_complete(run_ws_session(session_id, source_lang, target_lang)),
        daemon=True
    )
    session_thread.start()

    # 等待 session 就绪
    import time
    for _ in range(50):
        if session_id in sessions and sessions[session_id].get("ready"):
            break
        time.sleep(0.1)

    return jsonify({"session_id": session_id, "status": "ok"})


@app.route("/translate", methods=["POST"])
def translate():
    """发送音频并获取翻译结果"""
    session_id = request.form.get("session_id", "")
    audio_data = request.files.get("audio")

    if not session_id or session_id not in sessions:
        return jsonify({"error": "invalid session_id"}), 400

    if not audio_data:
        return jsonify({"error": "no audio data"}), 400

    session = sessions[session_id]
    if not session.get("ready"):
        return jsonify({"error": "session not ready"}), 503

    # 把音频放入队列
    raw = audio_data.read()
    try:
        loop = None
        for t in threading.enumerate():
            if hasattr(t, '_loop'):
                loop = t._loop
                break
        if loop and loop.is_running():
            asyncio.run_coroutine_threadsafe(session["audio_queue"].put(raw), loop)
    except Exception:
        pass

    # 返回最新翻译结果
    return jsonify({
        "session_id": session_id,
        "source": session.get("latest_source", ""),
        "translation": session.get("latest_translation", "")
    })


@app.route("/stop_session/<session_id>", methods=["POST"])
def stop_session(session_id):
    """结束会话"""
    if session_id in sessions:
        session = sessions[session_id]
        try:
            finish_req = create_finish_request(session_id)
            asyncio.run_coroutine_threadsafe(
                session["ws"].send(finish_req), session.get("loop"))
        except Exception:
            pass
        del sessions[session_id]
        return jsonify({"status": "stopped"})
    return jsonify({"error": "session not found"}), 404


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "sessions": len(sessions)})


if __name__ == "__main__":
    print("=== 火山引擎同传云端服务器 ===")
    print(f"监听: http://0.0.0.0:5000")
    print(f"API:")
    print(f"  POST /start_session        — 开始会话")
    print(f"  POST /translate             — 发送音频")
    print(f"  POST /stop_session/<id>     — 结束会话")
    print(f"  GET  /health                — 健康检查")
    print()
    app.run(host="0.0.0.0", port=5000, debug=False)
