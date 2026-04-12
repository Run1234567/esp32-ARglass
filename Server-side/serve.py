import asyncio
import websockets
import wave
import os
import time
import numpy as np
import base64    # 用于图片加密处理
import glob      # 用于查找最新的一张照片
import re        # 用于过滤（微笑）这种表情词汇
from faster_whisper import WhisperModel
from openai import OpenAI, AsyncOpenAI  # 💡 增加异步客户端
import pyttsx3   # 用于离线语音
from pydub import AudioSegment # 💡 新增：用于音频解码
import edge_tts  # 💡 引入库
import io
import pyttsx3 # 确保你顶部导入了它
#C:/Users/20461/AppData/Local/Programs/Python/Python311/python.exe -m pip install websockets 不要删
# =========================================
# ⚙️ 基础环境与目录初始化
# =========================================
if not os.path.exists("received"):
    os.makedirs("received")

# 屏蔽 Hugging Face 烦人的软链接警告
os.environ["HF_HUB_DISABLE_SYMLINKS_WARNING"] = "1" 


# 💡 定义本地 Python 路径，用于语音播报子进程隔离
# Linux 下直接调用系统环境变量即可
python_path = "python3"
AudioSegment.converter = "ffmpeg"
AudioSegment.ffprobe   = "ffprobe"
# =========================================
# 🧠 AI 双脑、语音模型与全局状态初始化
# =========================================
print("⏳ 正在加载 Whisper 语音识别模型 (tiny)...")
model = WhisperModel("base", device="cpu", compute_type="int8")
print("✅ 语音识别模型加载完毕！")

# 1. 聊天主脑 (DeepSeek)
AI_CLIENT = OpenAI(
    api_key="sk-fb636ddd5e504d509a38f8978434bdeb",
    base_url="https://api.deepseek.com" 
)

# 2. 视觉副脑 (阿里云通义千问 Qwen-VL)
VISION_CLIENT = OpenAI(
    api_key="sk-5a19d3da5ff94f489bf13b7c5ece3fdb",
    base_url="https://dashscope.aliyuncs.com/compatible-mode/v1"
)

# 💡 新增：流式专用的异步主脑
ASYNC_AI_CLIENT = AsyncOpenAI(
    api_key="sk-fb636ddd5e504d509a38f8978434bdeb",
    base_url="https://api.deepseek.com"
)

chat_history = [
    {"role": "system", "content": "你是一个戴在用户头上的智能AR眼镜助手贾维斯。回答必须极其口语化、简明扼要，控制在50字内，绝不使用任何括号或表情符号。可能包含同音字、错别字或断句错误（如把'贾维斯'识别成'假卫士'）。如果用户要求翻译（无论是中译英还是英译中），请直接输出翻译结果，不需要说“好的，翻译结果是”这类废话。"}
]

is_translation_mode = False 

# 💡 新增：全局花名册，存放所有连接进来的 ESP32
connected_clients = set()

# =========================================
# 🗣️ 核心功能函数区
# =========================================
def ask_ai_brain(user_text):
    print("\n🧠 [主脑思考中...]")
    chat_history.append({"role": "user", "content": user_text})
    try:
        response = AI_CLIENT.chat.completions.create(
            model="deepseek-chat", 
            messages=chat_history,
            max_tokens=100
        )
        ai_reply = response.choices[0].message.content
        chat_history.append({"role": "assistant", "content": ai_reply})
        return ai_reply
    except Exception as e:
        return f"主脑短路了，错误信息是：{e}"

def encode_image(image_path):
    with open(image_path, "rb") as image_file:
        return base64.b64encode(image_file.read()).decode('utf-8')

def ask_ai_to_look(image_path, user_question):
    print(f"\n👁️ [视觉副脑正在查看照片...]")
    system_prompt = "你是戴在用户头上的AR眼镜助手贾维斯。注意：用户的提问来自实时语音识别，"
    "可能包含同音字、错别字或断句错误（如把'贾维斯'识别成'假卫士'）。"
    "请务必结合上下文自动纠错，理解用户的真实意图并给出回答。"
    "回答必须极其口语化、简明扼要，控制在50字以内，绝不使用任何括号或表情符号。"
    try:
        base64_image = encode_image(image_path)
        response = VISION_CLIENT.chat.completions.create(
            model="qwen-vl-plus", 
            messages=[
                {"role": "system", "content": system_prompt},
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": user_question}, 
                        {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{base64_image}"}}
                    ]
                }
            ],
            max_tokens=100
        )
        reply = response.choices[0].message.content
        return reply
    except Exception as e:
        return f"我的眼睛好像进沙子了，错误信息是：{e}"

async def speak_sentence(text, websocket):
    """
    轻量级 TTS 函数：优先极速 Edge-TTS，若微软服务器被墙，瞬间切换本地离线语音
    """
    safe_text = re.sub(r'[（\(【\[].*?[）\)】\]]', '', text).strip()
    if not safe_text: return
    
    print(f"  🗣️ [秒回发声]: {safe_text}")
    try:
        # 🚀 尝试连接微软服务器
        communicate = edge_tts.Communicate(safe_text, "zh-CN-XiaoxiaoNeural")
        mp3_buffer = io.BytesIO()
        
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                mp3_buffer.write(chunk["data"])
                
        mp3_buffer.seek(0)
        audio = AudioSegment.from_file(mp3_buffer, format="mp3")
        
        # 💡 修改点1：强制设为单声道 (set_channels(1))，适配ESP32单喇叭
        audio = audio.set_frame_rate(16000).set_sample_width(2).set_channels(1) 
        raw_pcm_data = audio.raw_data
        
        # 💡 修改点2：精准控制下发速率（每次3200字节=0.1秒数据，延时0.09秒）
        chunk_size = 3200
        for i in range(0, len(raw_pcm_data), chunk_size):
            # 💡 遍历花名册里的所有设备，挨个发过去！
            for client in connected_clients:
                try:
                    await client.send(raw_pcm_data[i:i+chunk_size])
                except:
                    pass # 如果某个设备突然掉线了，忽略报错，继续发给下一个
            await asyncio.sleep(0.09) 
            
    except Exception as e:
        # 🛡️ 微软服务器连不上，瞬间启用本地离线引擎兜底
        print(f"  ⚠️ [微软接口被墙，光速切换本地离线]: {e}")
        try:
            def generate_offline():
                engine = pyttsx3.init()
                engine.setProperty('rate', 180) # 语速稍快
                # 使用微妙级时间戳防冲突
                temp_file = f"offline_reply_{int(time.time()*1000)}.wav"
                engine.save_to_file(safe_text, temp_file)
                engine.runAndWait()
                return temp_file

            temp_file = await asyncio.to_thread(generate_offline)
            
            audio = AudioSegment.from_file(temp_file)
            # 💡 修改点3：离线语音也必须强制单声道
            audio = audio.set_frame_rate(16000).set_sample_width(2).set_channels(1)
            raw_pcm_data = audio.raw_data
            
            # 阅后即焚
            if os.path.exists(temp_file):
                os.remove(temp_file)
                
            # 💡 修改点4：离线语音也用同样的缓冲速率发包
            chunk_size = 3200
            for i in range(0, len(raw_pcm_data), chunk_size):
                await websocket.send(raw_pcm_data[i:i+chunk_size])
                await asyncio.sleep(0.09)
                
        except Exception as e2:
            print(f"  ❌ 离线语音也彻底失败了: {e2}")
async def process_and_speak_stream(user_text, websocket):
    """
    全链路流式大脑：边接收文字，边切分句子，边触发发声
    """
    print(f"\n🧠 [主脑流式思考中...]")
    chat_history.append({"role": "user", "content": user_text})
    
    try:
        response = await ASYNC_AI_CLIENT.chat.completions.create(
            model="deepseek-chat",
            messages=chat_history,
            max_tokens=100,
            stream=True # 💡 开启魔法开关
        )
        
        sentence_buffer = ""
        full_reply = ""
        
        # 遇到这些符号，说明一句话结束了，立刻送去语音合成！
        punctuation = ['，', '。', '！', '？', ',', '.', '!', '?']
        
        async for chunk in response:
            content = chunk.choices[0].delta.content
            if content:
                sentence_buffer += content
                full_reply += content
                
                # 如果缓冲区里出现了标点符号
                if any(punc in sentence_buffer for punc in punctuation):
                    # 立刻把缓冲区的话拿去播放，不阻塞后续内容生成
                    await speak_sentence(sentence_buffer, websocket)
                    sentence_buffer = "" # 清空，准备接下一句
        
        # 结尾如果没有标点符号，把剩下的话播完
        if sentence_buffer:
            await speak_sentence(sentence_buffer, websocket)
            
        # 记录完整历史
        chat_history.append({"role": "assistant", "content": full_reply})
        print("✅ [主脑流式输出完毕]")
        
    except Exception as e:
        print(f"主脑流式短路了：{e}")

async def speak(text, websocket, loop):
    """
    终极极速版：首选内存流透传，网络断开时瞬间切换本地离线语音
    """
    print(f"🔊 [贾维斯思考完毕]: {text}")
    
    # 1. 文本预处理
    safe_text = re.sub(r'[（\(].*?[）\)]', '', text)
    safe_text = re.sub(r'[【\[].*?[】\]]', '', safe_text).replace('\n', '，')
    if not safe_text.strip(): return

    raw_pcm_data = None

    # ====================================================
    # 🚀 第一阶段：尝试极速在线 TTS (内存流)
    # ====================================================
    try:
        voice = "zh-CN-XiaoxiaoNeural"
        communicate = edge_tts.Communicate(safe_text, voice)
        mp3_buffer = io.BytesIO()
        
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                mp3_buffer.write(chunk["data"])
        
        mp3_buffer.seek(0)
        audio = AudioSegment.from_file(mp3_buffer, format="mp3")
        
        # 💡 修改点5：单声道
        audio = audio.set_frame_rate(16000).set_sample_width(2).set_channels(1)
        raw_pcm_data = audio.raw_data 

    # ====================================================
    # 🛡️ 第二阶段：网络报错，瞬间切换离线 TTS (本地生成)
    # ====================================================
    except Exception as e:
        print(f"⚠️ [在线网络受阻，瞬间启用本地离线引擎]: {e}")
        try:
            def generate_offline():
                engine = pyttsx3.init()
                engine.setProperty('rate', 180) 
                temp_file = f"offline_reply_{int(time.time())}.wav"
                engine.save_to_file(safe_text, temp_file)
                engine.runAndWait()
                return temp_file

            temp_file = await asyncio.to_thread(generate_offline)
            
            audio = AudioSegment.from_file(temp_file)
            
            # 💡 修改点6：单声道
            audio = audio.set_frame_rate(16000).set_sample_width(2).set_channels(1)
            raw_pcm_data = audio.raw_data
            
            if os.path.exists(temp_file):
                os.remove(temp_file)
                
        except Exception as e2:
            print(f"❌ 离线语音也彻底失败了: {e2}")
            return

    # ====================================================
    # 📡 第三阶段：统一将处理好的波形数据切片下发给【所有】ESP32
    # ====================================================
    if raw_pcm_data:
        total_bytes = len(raw_pcm_data)
        print(f"📡 正在向 {len(connected_clients)} 台设备广播波形流... ({total_bytes} 字节)")

        chunk_size = 8192 
        for i in range(0, total_bytes, chunk_size):
            chunk = raw_pcm_data[i:i+chunk_size]
            
            # 💡 遍历花名册里的所有设备，挨个发过去！
            for client in connected_clients:
                try:
                    await client.send(chunk)
                except:
                    pass # 如果某个设备突然掉线了，忽略报错，继续发给下一个
                
        print("✅ 播报下发完毕")
def transcribe_audio(file_path, websocket, loop):
    global is_translation_mode  
    
    def sync_speak(text):
        if text:
            asyncio.run_coroutine_threadsafe(speak(text, websocket, loop), loop)

    def request_photo():
        print("📸 [指令] 要求眼镜即时拍照...")
        asyncio.run_coroutine_threadsafe(websocket.send("CAPTURE"), loop)
        time.sleep(1.2)

    try:
        segments, info = model.transcribe(
            file_path, 
            beam_size=5, 
            initial_prompt="这是一段对话。你好，Hello。请翻译，Please translate.", 
            vad_filter=True, 
            vad_parameters=dict(min_silence_duration_ms=400)
        )
        text = "".join([segment.text for segment in segments])
        
        if not text.strip():
            return
            
        print(f"\n👤 [收音({info.language})]: {text}")
        
        if "开启同声传译" in text or "打开翻译模式" in text:
            is_translation_mode = True
            reply = "已进入双向同传模式。我会自动为你中英互译。"
            print(f"🤖 [系统]: {reply}\n")
            sync_speak(reply) # 💡 调用新的助手函数
            return
            
        if "退出同声传译" in text or "退出翻译模式" in text or "关闭翻译" in text:
            is_translation_mode = False
            reply = "已退出同传模式，贾维斯主脑已重新上线。"
            print(f"🤖 [系统]: {reply}\n")
            sync_speak(reply) # 💡 调用新的助手函数
            return

        if is_translation_mode:
            print(f"🔄 [同传处理中... 识别语种: {info.language}]")
            if info.language == 'zh':
                sys_prompt = "You are a professional interpreter. Translate the following Chinese text into English. Output ONLY the translation without any explanations or quotes."
            else:
                sys_prompt = "你是一个专业的同声传译员。请将输入的外语翻译成地道的中文口语。只输出翻译结果，不要标点符号，不要任何解释。"
            
            response = AI_CLIENT.chat.completions.create(
                model="deepseek-chat", 
                messages=[
                    {"role": "system", "content": sys_prompt},
                    {"role": "user", "content": text}
                ],
                max_tokens=150,
                temperature=0.1 
            )
            reply = response.choices[0].message.content
            print(f"🎧 [翻译结果]: {reply}\n")
            sync_speak(reply) # 💡 调用新的助手函数
            return

        vision_keywords = ["看", "这是什么", "眼前", "照片", "视线", "帮我看看"]
        translate_image_keywords = ["翻译文字", "翻译一张照片", "看看上面写了什么", "翻译这段话", "提取文字"]

        need_image_translation = any(keyword in text for keyword in translate_image_keywords)
        need_vision = any(keyword in text for keyword in vision_keywords)
        
        if need_image_translation or need_vision:
            if need_image_translation:
                print("\n🌐 [路由: 图像文字翻译通道]")
            else:
                print("\n👁️ [路由: 视觉问答通道]")

            request_photo()
            list_of_files = glob.glob('received/photo_*.jpg')
            if list_of_files:
                latest_file = max(list_of_files, key=os.path.getctime)
                if need_image_translation:
                    prompt = "请详细分析这张图片中的外语文字，并翻译成中文。"
                else:
                    prompt = text
                reply = ask_ai_to_look(latest_file, prompt)
            else:
                reply = "贾维斯还没准备好画面，请再试一次。"
        else:
            # 💡 直接触发异步流式流水线，彻底抛弃死等的 ask_ai_brain
            asyncio.run_coroutine_threadsafe(
                process_and_speak_stream(text, websocket),
                loop
            )
            return # 直接返回，因为声音播放已经在流水线里处理了
            
        # 注意：下面这两行只为视觉大模型保留（因为 Qwen-VL 目前没开流式）
        print(f"🤖 [视觉脑回复]: {reply}\n")
        sync_speak(reply)
        
    except Exception as e:
        print(f"❌ [语音识别报错]: {e}")
    finally:
        if os.path.exists(file_path):
            os.remove(file_path)
# =========================================
# 🔌 WebSocket 服务器与 VAD 断句逻辑
# =========================================
async def handle_client(websocket):
    loop = asyncio.get_running_loop()
    
    # 💡 1. 设备连入，登记到花名册
    connected_clients.add(websocket)
    print(f"✅ [服务器] 新设备已连接，当前在线终端数: {len(connected_clients)}")
    
    def create_wav(index):
        wf = wave.open(f"received/audio_{index}.wav", "wb")
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(16000)
        return wf

    img_count = 0
    audio_count = 1
    wav_file = create_wav(audio_count)

    SILENCE_THRESHOLD = 1500      
    MAX_SILENCE_DURATION = 0.5   
    MAX_RECORD_DURATION = 10.0   

    silence_timer = 0.0
    current_record_time = 0.0
    has_spoken = False

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                if len(message) < 3000: 
                    audio_data = np.frombuffer(message, dtype=np.int16)
                    volume = int(np.max(np.abs(audio_data))) 
                    chunk_time = len(audio_data) / 16000.0  

                    current_record_time += chunk_time

                    bar_len = min(int(volume / 200), 40) 
                    bar_str = "█" * bar_len + "-" * (40 - bar_len)
                    print(f"\r🎤 实时音量: {volume:5d} [{bar_str}]", end="", flush=True)

                    if volume > SILENCE_THRESHOLD:
                        silence_timer = 0.0  
                        has_spoken = True
                    else:
                        silence_timer += chunk_time 

                    wav_file.writeframes(message)
                    
                    if (has_spoken and silence_timer >= MAX_SILENCE_DURATION) or current_record_time >= MAX_RECORD_DURATION:
                        
                        current_file_path = f"received/audio_{audio_count}.wav"
                        wav_file.close()
                        print(f"\n✂️ 自动断句 ({current_record_time:.1f}秒)，开始识别...")
                        
                        asyncio.create_task(asyncio.to_thread(transcribe_audio, current_file_path, websocket, loop))
                        
                        audio_count += 1
                        wav_file = create_wav(audio_count)
                        silence_timer = 0.0
                        current_record_time = 0.0
                        has_spoken = False
                        
                else:
                    img_count += 1
                    with open(f"received/photo_{img_count}.jpg", "wb") as f:
                        f.write(message)
                    if img_count % 100 == 0:
                        print(f"\r📸 持续接收视频流中... 最新: photo_{img_count}.jpg", end="", flush=True)
            else:
                print(f"\n💬 收到文本: {message}")

    except Exception as e:
        print(f"\n🔌 连接异常: {e}")
    finally:
        if wav_file:
            wav_file.close()
            
        # 💡 2. 设备断开，从花名册中除名
        connected_clients.remove(websocket)
        print(f"❌ [服务器] 设备断开，当前在线终端数: {len(connected_clients)}")

async def main():
    async with websockets.serve(handle_client, "0.0.0.0", 8765):
        print("🚀 J.A.R.V.I.S 双脑服务器已启动，正在监听 8765 端口...")
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
