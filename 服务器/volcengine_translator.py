"""
火山引擎同传大模型 2.0 接入
- WebSocket 实时双向连接
- Protobuf 二进制协议
- 双线程：麦克风录音发送 + 接收翻译结果

使用前：
1. 从火山引擎控制台获取 API Key
2. 解压 ast_python_client.zip，python_protogen/ 目录已自动解压
3. pip install websockets protobuf sounddevice numpy
"""

import asyncio
import websockets
import numpy as np
import uuid
import sys
import os
import threading
import queue

import sounddevice as sd

# --- 导入火山引擎 Protobuf 协议文件 ---
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python_protogen"))

try:
    from products.understanding.ast.ast_service_pb2 import TranslateRequest, TranslateResponse
    from common.events_pb2 import Type
    HAS_PROTOBUF = True
    print("[OK] 已加载 protobuf 协议文件")
except ImportError as e:
    HAS_PROTOBUF = False
    print(f"[警告] 未找到 protobuf 协议: {e}")
    print("       请确保 python_protogen/ 目录存在\n")

# --- 火山引擎配置 ---
API_KEY = "8f61b5ed-8636-4d00-bb0c-17748bd45f1e"
RESOURCE_ID = "volc.service_type.10053"
WS_URL = "wss://openspeech.bytedance.com/api/v4/ast/v2/translate"

# --- 音频参数 ---
SAMPLE_RATE = 16000
CHANNELS = 1
CHUNK_DURATION = 0.08  # 80ms 一包，文档推荐
CHUNK_SAMPLES = int(SAMPLE_RATE * CHUNK_DURATION)  # 1280 samples


def create_start_session_request(session_id, source_lang="zh", target_lang="en", speaker_id="zh_female_vv_uranus_bigtts"):
    """创建建连请求 (Event 100)"""
    req = TranslateRequest()
    req.event = Type.StartSession
    req.request_meta.SessionID = session_id
    req.user.uid = "ar_glass_client"
    req.user.did = "ar_glass_client"
    # 源音频配置
    req.source_audio.format = "wav"
    req.source_audio.rate = SAMPLE_RATE
    req.source_audio.bits = 16
    req.source_audio.channel = CHANNELS
    # 目标音频配置（s2s 模式必填）
    req.target_audio.format = "pcm"
    req.target_audio.rate = 24000
    # 请求配置
    req.request.mode = "s2s"
    req.request.source_language = source_lang
    req.request.target_language = target_lang
    req.request.speaker_id = speaker_id
    return req.SerializeToString()


def create_audio_request(session_id, audio_chunk):
    """创建音频数据请求 (Event 200)"""
    req = TranslateRequest()
    req.event = Type.TaskRequest
    req.request_meta.SessionID = session_id
    req.source_audio.format = "wav"
    req.source_audio.rate = SAMPLE_RATE
    req.source_audio.bits = 16
    req.source_audio.channel = CHANNELS
    req.source_audio.binary_data = audio_chunk  # 80ms 的 PCM 数据
    return req.SerializeToString()


def create_finish_request(session_id):
    """创建结束会话请求 (Event 102)"""
    req = TranslateRequest()
    req.event = Type.FinishSession
    req.request_meta.SessionID = session_id
    return req.SerializeToString()


def parse_response(data):
    """解析火山引擎返回的 Protobuf 数据"""
    resp = TranslateResponse()
    resp.ParseFromString(data)
    return resp


async def send_audio_stream(ws, audio_queue, session_id, session_ready):
    """发送线程：等 SessionStarted 后，从队列获取音频并发送到云端"""
    # 关键：必须等到 SessionStarted 再发音频
    await session_ready.wait()

    while True:
        audio_chunk = await audio_queue.get()
        if audio_chunk is None:  # 哨兵值，结束信号
            break
        data = create_audio_request(session_id, audio_chunk)
        await ws.send(data)


# --- 非阻塞音频播放（排队播放，不打断上一句） ---
_play_queue = queue.Queue()
_play_event = threading.Event()

def _audio_playback_worker():
    """播放线程：用 OutputStream 顺序写入音频，不会打断正在播放的句子"""
    with sd.OutputStream(samplerate=24000, channels=1, dtype='float32') as stream:
        while True:
            audio_np = _play_queue.get()
            if audio_np is None:
                break
            try:
                # 按小块写入，播放完再取下一句
                chunk_size = 4800  # 200ms 一块
                for i in range(0, len(audio_np), chunk_size):
                    stream.write(audio_np[i:i+chunk_size].reshape(-1, 1))
            except Exception:
                pass
        _play_event.set()

_play_thread = threading.Thread(target=_audio_playback_worker, daemon=True)
_play_thread.start()


async def receive_results(ws, session_ready, tag=""):
    """接收线程：处理云端实时返回的字幕和语音"""
    tts_buffer = bytearray()
    tts_lock = threading.Lock()
    flush_timer = None

    async def schedule_flush():
        """收到 TTSSentenceEnd 后等 500ms，没有新数据才播放"""
        nonlocal tts_buffer, flush_timer
        await asyncio.sleep(0.8)
        with tts_lock:
            data = bytes(tts_buffer)
            tts_buffer = bytearray()
            flush_timer = None
        if data:
            try:
                audio_np = np.frombuffer(data, dtype=np.float32)
                _play_queue.put(audio_np)
            except Exception:
                pass

    async for message in ws:
        resp = parse_response(message)

        if resp.event == Type.SessionStarted:
            print(f"{tag} 同传通道建立成功，可以开始说话...")
            session_ready.set()

        elif resp.event == Type.SourceSubtitleStart:
            pass

        elif resp.event == Type.SourceSubtitleResponse:
            pass

        elif resp.event == Type.SourceSubtitleEnd:
            if resp.text:
                print(f"{tag} [原文] {resp.text}")

        elif resp.event == Type.TranslationSubtitleStart:
            pass

        elif resp.event == Type.TranslationSubtitleResponse:
            pass

        elif resp.event == Type.TranslationSubtitleEnd:
            if resp.text:
                print(f"{tag} [译文] {resp.text}")

        elif resp.event == Type.TTSSentenceStart:
            pass

        elif resp.event == Type.TTSResponse:
            if resp.data:
                with tts_lock:
                    tts_buffer.extend(resp.data)
                # 有新数据，重置定时器
                if flush_timer is not None:
                    flush_timer.cancel()
                flush_timer = asyncio.ensure_future(schedule_flush())

        elif resp.event == Type.TTSSentenceEnd:
            # 重置定时器，再等 500ms
            if flush_timer is not None:
                flush_timer.cancel()
            flush_timer = asyncio.ensure_future(schedule_flush())

        elif resp.event == Type.AudioMuted:
            muted_ms = resp.muted_duration_ms if hasattr(resp, 'muted_duration_ms') else 0
            if muted_ms > 3000:
                print(f"{tag} [系统] 检测到停顿 ({muted_ms}ms)")

        elif resp.event == Type.UsageResponse:
            pass

        elif resp.event == Type.SessionFinished:
            print(f"{tag} 会话正常结束")
            break

        elif resp.event == Type.SessionFailed:
            msg = resp.response_meta.Message if hasattr(resp, 'response_meta') else "未知错误"
            print(f"{tag} [错误] 会话失败: {msg}")
            break


async def run_session(audio_queue, source_lang, target_lang, speaker_id, tag):
    """运行单个翻译会话"""
    session_id = str(uuid.uuid4())
    session_ready = asyncio.Event()

    headers = {
        "X-Api-Key": API_KEY,
        "X-Api-Resource-Id": RESOURCE_ID,
        "X-Api-Connect-Id": str(uuid.uuid4()),
    }

    print(f"{tag} 正在连接...")
    try:
        async with websockets.connect(
            WS_URL,
            additional_headers=headers,
            max_size=1000000000,
            ping_interval=None
        ) as ws:
            log_id = ws.response.headers.get('X-Tt-Logid') if hasattr(ws, 'response') else 'N/A'
            print(f"{tag} [连接] LogID: {log_id}")

            start_req = create_start_session_request(session_id, source_lang, target_lang, speaker_id)
            await ws.send(start_req)
            print(f"{tag} 建连请求已发送，等待 SessionStarted...")

            send_task = asyncio.create_task(
                send_audio_stream(ws, audio_queue, session_id, session_ready))
            recv_task = asyncio.create_task(
                receive_results(ws, session_ready, tag))

            await asyncio.gather(send_task, recv_task)

    except websockets.exceptions.ConnectionClosed as e:
        print(f"{tag} [连接关闭] {e}")
    except Exception as e:
        print(f"{tag} [错误] {e}")


async def volcengine_translator():
    """主函数：英文→中文翻译"""
    audio_queue = asyncio.Queue()
    loop = asyncio.get_event_loop()

    def audio_callback(indata, frames, time_info, status):
        if status:
            print(f"[音频状态] {status}", file=sys.stderr)
        loop.call_soon_threadsafe(audio_queue.put_nowait, indata.copy().tobytes())

    print("=== 火山引擎同传 2.0 ===")
    print(f"模式: 英文 → 中文")
    print(f"正在连接...\n")

    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=CHANNELS,
        dtype='int16',
        blocksize=CHUNK_SAMPLES,
        callback=audio_callback
    ):
        await run_session(audio_queue, "en", "zh", "en_female_amazon_kristy", "")


if __name__ == "__main__":
    if API_KEY == "你的API_KEY":
        print("=" * 50)
        print("请先配置密钥！")
        print("1. 登录火山引擎控制台")
        print("2. 获取 API Key")
        print("3. 修改本文件中的 API_KEY")
        print("=" * 50)
        sys.exit(1)

    asyncio.run(volcengine_translator())
