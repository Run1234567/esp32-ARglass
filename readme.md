# J.A.R.V.I.S. - 基于 ESP32-S3 的分布式 AR 智能眼镜系统

这是一个基于 ESP32-S3 驱动的极客风分布式 AR 眼镜生态系统，代号 **"J.A.R.V.I.S."**。整个系统彻底打破了单片机的性能瓶颈，通过 **两颗 ESP32-S3 芯片双芯协同**（音频 AI 大脑 + 视觉传感中枢），外加多个 BLE 无线外设（魔法手柄）、一个独立视频对讲机，以及支持 MQTT 的手机 Web 控制面板，共同构建了一套功能极其硬核的软硬件开源 AR 生态。

## 🌟 核心硬核特性

* 🧠 **双芯高速协同**：MCU1 负责 AI/音频/系统调度，MCU2 负责 31 屏 LVGL 渲染与 8 大传感器融合，两者通过 1Mbaud UART 协议超高速同步。
* 🎙️ **全双工 AI 语音与翻译**：内建 WakeNet9 离线唤醒引擎、ESP-TTS 中文语音合成，结合火山引擎实现极低延迟的 Opus 编码 AI 对话与实时同传翻译。
* 🕸️ **音频分流黑科技**：基于 PDM 麦克风的单路输入，通过 Core 1 独占任务原子化分发至 7 个环形缓冲区（录音、AI、唤醒、测音高、对讲等），零拷贝极速响应。
* 👆 **边缘 AI 动作识别**：在眼镜端与 BLE 魔杖端均部署了 TensorFlow Lite Micro 神经网络，配合 esp-nn 硬件加速，实现毫秒级 6 轴 IMU 空间手势识别（上划、下划、画圈等）。
* 🌐 **分布式无线生态**：自研跨设备统一 BLE 协议，包含触控轮、轨迹球、模拟摇杆等 6 款形态的“赛博魔杖”，并提供暗黑苹果玻璃风的 Web 端 3D 传感器看板。
* 🔥 **极致内存压榨**：全局 PSRAM 内存管理策略，将所有环形缓冲区、TTS 模型、Opus 编码器全部定向分配至 8MB OPI PSRAM，彻底释放 512KB SRAM 的算力潜能。

## 📂 核心代码库解析

本仓库包含系统生态的所有固件与前端代码，划分为以下五大核心模块：

### 1. `ARGLASS_IDF` — 眼镜“大脑” (Seeed XIAO ESP32-S3)

负责整个系统的声学、视觉与 AI 调度。

* **多路音频引擎**：通过 `audio_hub_task` 将麦克风数据同时分发给唤醒词检测、SD卡录音、AI 对话、流媒体翻译、分贝检测以及专业的 YIN 算法音高检测。
* **交互逻辑**：二次确认防误触的双击唤醒机制（"Jarvis" -> "我在" -> 开启收音），自适应码率 (Q18-28) 的视频推流，并在弱网下自动降级进行本地 SD 卡视频缓存。

### 2. `esp32_LVGL` — 眼镜“显示+传感器” (ESP32-S3 DevKitC-1)

眼镜的视觉呈现与环境感知系统。

* **31屏状态机 UI**：覆盖从 AR 主页、全息相机、骨传导音乐同步歌词、噪声监测、小说阅读，到多达 9 款内置游戏（如 Cyber Runner、2048 等）。
* **超级总线**：同时挂载 MPU6050 (姿态/计步器)、BMP280 (温度)、PAJ7620 (手势)、MAX30102 (心率血氧)、ATGM336H (GPS) 以及光照等 8 种硬件外设。支持 I2C 总线自愈防卡死。

### 3. `ESP32_BlueYa` — "Cyberry\_Wand" 蓝牙外设家族

使用 Apache NimBLE 栈编写的 6 个独立硬件项目，作为眼镜的无线输入设备：

* **电容触摸轮 (ChuMo)** / **轨迹球 (GuiJiQiu)** / **模拟摇杆 (YaoGan)**
* **IMU 魔杖 (Imu)**：内置 TFLite 模型，识别上划/下划/画圈等 6 种空中手势。
* **融合方案 (Imu1)**：传感器复合输入，单 Characteristic 报文分发。

### 4. `ShiPingTH` — 视频对讲机

一套基于 ESP32-S3 与 OV2640 的独立 VoIP 通话设备。支持 TCP 握手信令、16kHz 无损 PCM 实时全双工传输与 5 屏 LVGL 拨号 UI。

### 5. `esp32_html` — Web 控制中心

一套采用原生 HTML/CSS/JS 开发的深色科技风手机控制台。

* 支持 Apple TV 风格的 MQTT 遥控面板。
* 利用 Chart.js 与 Leaflet 实现光照、温度、噪声、GPS 坐标的 3D 实时仪表盘。

## 🛠️ 技术栈总览


| **领域**         | **核心技术栈**                                          |
| ---------------- | ------------------------------------------------------- |
| **主控芯片**     | ESP32-S3 (双核 Xtensa LX7 @ 240MHz)                     |
| **软件框架**     | ESP-IDF v5.5.0 (原生 C/C++)                             |
| **图形与显示**   | LVGL 8.x + ST7789 (RGB565, 240x240 & 128x160)           |
| **边缘 AI 推理** | TensorFlow Lite Micro + esp-nn 硬件指令集加速           |
| **声学处理**     | esp-sr WakeNet9, ESP-TTS, YIN 算法, Opus 编码           |
| **无线通信**     | WiFi, Apache NimBLE (双模), MQTT (EMQX), WebSocket, TCP |
| **算法实现**     | Mahony AHRS, 一阶 IIR 滤波, 基于峰值检测的计步器        |
| **前端交互**     | HTML5, CSS3 (毛玻璃效果), MQTT.js, Chart.js, Leaflet    |

## 🚀 快速开始

本项目完全基于原生 ESP-IDF 开发，未使用 Arduino Core。

1. **环境准备**：安装 [ESP-IDF v5.5.0](https://docs.espressif.com/projects/esp-idf/en/v5.5.0/esp32s3/get-started/index.html)。
2. **克隆代码**：

   ```bash
   git clone https://github.com/Run1234567/esp32-ARglass.git
   cd esp32-ARglass
   ```
3. **编译烧录**：
   依次进入 `ARGLASS_IDF` 与 `esp32_LVGL` 目录，通过 `idf.py build flash monitor` 编译并烧录至对应的 ESP32-S3 芯片中。*(注：需提前在 menuconfig 中配置好 WiFi 账户与模型分区)*

## 📄 协议与授权

本项目采用 [MIT License](https://choosealicense.com/licenses/mit/) 开源，欢迎提交 PR、Fork 与二创！

如果有任何关于硬件接线、API 配置或服务器部署的问题，欢迎提交 Issue。
