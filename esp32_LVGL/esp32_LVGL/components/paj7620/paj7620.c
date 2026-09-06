// ============================================================
// paj7620.c
// J.A.R.V.I.S. AR 智能眼镜 —— PAJ7620 手势识别传感器驱动 (优化稳定版)
// ============================================================

#include "paj7620.h"
#include "ui_globals.h"
#include "ui_manager.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PAJ7620";

// ---- 硬件配置 ----
#define PAJ7620_SCL         1
#define PAJ7620_SDA         2
#define PAJ7620_I2C_PORT    I2C_NUM_0
#define PAJ7620_I2C_FREQ    50000   // 降频到 50kHz，提高稳定性
static uint8_t paj7620_addr = 0x73; // 运行时自动探测
#define PAJ7620_TIMEOUT     1000

// ---- 错误恢复配置 ----
#define MAX_CONSECUTIVE_ERRORS   10     // 连续读取失败次数阈值，触发重置
#define I2C_BUS_RECOVERY_CLKS    9      // I2C 总线恢复时钟脉冲数

// ---- 任务配置 ----
#define GESTURE_TASK_STACK  4096
#define GESTURE_TASK_PRIO   5

static TaskHandle_t gesture_task_handle = NULL;
static bool is_initialized = false;
// 引入 volatile 保证跨线程修改的可见性，控制任务安全退出
static volatile bool s_run_gesture_task = false;
static int s_consecutive_errors = 0;     // 连续读取错误计数 

// ---- 基础固件初始化数组 (55 个元素) ----
static const uint16_t init_array[] = {
    0xEF00, 0x4100, 0x4200, 0x3707, 0x3817, 0x3906, 0x4201, 0x462D,
    0x470F, 0x483C, 0x4900, 0x4A1E, 0x4C22, 0x5110, 0x5E10, 0x6027,
    0x8042, 0x8144, 0x8204, 0x8B01, 0x9006, 0x950A, 0x960C, 0x9705,
    0x9A14, 0x9C3F, 0xA519, 0xCC19, 0xCD0B, 0xCE13, 0xCF64, 0xD021,
    0xEF01, 0x020F, 0x0310, 0x0402, 0x2501, 0x2739, 0x287F, 0x2908,
    0x3EFF, 0x5E3D, 0x6596, 0x6797, 0x69CD, 0x6A01, 0x6D2C, 0x6E01,
    0x7201, 0x7335, 0x7400, 0x7701, 0xEF00, 0x41FF, 0x4201
};

// ---- 手势识别模式配置数组 (29 个元素) ----
static const uint16_t gesture_mode_array[] = {
    0xEF00, 0x4100, 0x4200, 0x483C, 0x4900, 0x5110, 0x8320, 0x9FF9,
    0xEF01, 0x011E, 0x020F, 0x0310, 0x0402, 0x4140, 0x4330, 0x6596,
    0x6600, 0x6797, 0x6801, 0x69CD, 0x6A01, 0x6BB0, 0x6C04, 0x6D2C,
    0x6E01, 0x7400, 0xEF00, 0x41FF, 0x4201
};

// ============================================================
//   底层 I2C 读写
// ============================================================
static esp_err_t paj7620_write_reg(uint8_t reg_addr, uint8_t data) {
    uint8_t buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(PAJ7620_I2C_PORT, paj7620_addr, buf, 2, PAJ7620_TIMEOUT / portTICK_PERIOD_MS);
}

// 升级版：支持读取指定长度的连续寄存器内容
static esp_err_t paj7620_read_regs(uint8_t start_reg, uint8_t *data_buf, size_t len) {
    return i2c_master_write_read_device(PAJ7620_I2C_PORT, paj7620_addr, &start_reg, 1, data_buf, len, PAJ7620_TIMEOUT / portTICK_PERIOD_MS);
}

static void paj7620_wakeup(void) {
    // PAJ7620 唤醒：需要写 0x00 到寄存器 0xEF 切到 Bank 0
    // 先发两次确保芯片从深度休眠中醒来
    paj7620_write_reg(0xEF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));
    paj7620_write_reg(0xEF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));
}

static esp_err_t paj7620_write_array(const uint16_t *array, size_t size) {
    for (size_t i = 0; i < size; i++) {
        uint8_t reg = (array[i] >> 8) & 0xFF;
        uint8_t val = array[i] & 0xFF;
        esp_err_t err = paj7620_write_reg(reg, val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Reg 0x%02X write 0x%02X failed", reg, val);
            return err;
        }
    }
    return ESP_OK;
}

// ============================================================
//   I2C 总线恢复机制
// ============================================================
/**
 * @brief I2C 总线恢复 - 当 SDA 被从设备拉低卡死时，通过时钟脉冲恢复
 *
 * 原理：主机产生最多 9 个时钟脉冲，让从设备释放 SDA 线
 * 这是 I2C 协议标准的总线恢复方法
 */
static void i2c_bus_recovery(void) {
    ESP_LOGW(TAG, "🔧 执行 I2C 总线恢复 (发送 %d 个时钟脉冲)", I2C_BUS_RECOVERY_CLKS);

    // 配置 SCL 为输出模式，手动产生时钟脉冲
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PAJ7620_SCL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // 产生时钟脉冲，尝试释放 SDA
    for (int i = 0; i < I2C_BUS_RECOVERY_CLKS; i++) {
        gpio_set_level(PAJ7620_SCL, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(PAJ7620_SCL, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // 产生 STOP 条件：SDA 从低到高（SCL 为高时）
    gpio_set_direction(PAJ7620_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level(PAJ7620_SDA, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(PAJ7620_SDA, 1);
    vTaskDelay(pdMS_TO_TICKS(1));

    // 恢复为 I2C 功能模式
    gpio_set_direction(PAJ7620_SDA, GPIO_MODE_INPUT);
    gpio_set_direction(PAJ7620_SCL, GPIO_MODE_INPUT);

    ESP_LOGI(TAG, "✅ I2C 总线恢复完成");
}

/**
 * @brief PAJ7620 完全重新初始化
 *
 * 包含总线恢复 + 芯片重置 + 寄存器重写
 * 用于连续读取错误过多时的自动恢复
 */
static esp_err_t paj7620_reinit(void) {
    ESP_LOGW(TAG, "🔄 PAJ7620 开始重新初始化...");

    // 1. 先尝试 I2C 总线恢复
    i2c_bus_recovery();
    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. 唤醒芯片
    paj7620_wakeup();
    vTaskDelay(pdMS_TO_TICKS(20));

    // 3. 验证芯片是否响应
    uint8_t part_id = 0;
    uint8_t reg = 0x00;
    paj7620_write_reg(0xEF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));

    esp_err_t ret = i2c_master_write_read_device(
        PAJ7620_I2C_PORT, paj7620_addr, &reg, 1, &part_id, 1, 100 / portTICK_PERIOD_MS);

    if (ret != ESP_OK || part_id != 0x20) {
        ESP_LOGE(TAG, "❌ PAJ7620 重新初始化失败 - 芯片无响应 (ID: 0x%02X)", part_id);
        return ESP_FAIL;
    }

    // 4. 重写所有配置寄存器
    if (paj7620_write_array(init_array, sizeof(init_array) / sizeof(init_array[0])) != ESP_OK) {
        ESP_LOGE(TAG, "❌ 重写 init_array 失败");
        return ESP_FAIL;
    }
    if (paj7620_write_array(gesture_mode_array, sizeof(gesture_mode_array) / sizeof(gesture_mode_array[0])) != ESP_OK) {
        ESP_LOGE(TAG, "❌ 重写 gesture_mode_array 失败");
        return ESP_FAIL;
    }

    paj7620_write_reg(0xEF, 0x00);

    // 5. 重置错误计数
    s_consecutive_errors = 0;

    ESP_LOGI(TAG, "✅ PAJ7620 重新初始化成功！");
    return ESP_OK;
}

// ============================================================
//   初始化
// ============================================================
esp_err_t paj7620_init(void) {
    if (is_initialized) return ESP_OK;

    ESP_LOGI(TAG, "PAJ7620 初始化开始 (SCL:%d SDA:%d, I2C_PORT:%d)", PAJ7620_SCL, PAJ7620_SDA, PAJ7620_I2C_PORT);

    // PAJ7620 与 MPU6050/BMP280 共享 I2C_NUM_0（GPIO 1/2）
    // I2C 总线已在 main.cpp 的 i2c_master_init() 中初始化
    // 直接跳过驱动安装，因为 I2C 总线已可用

    // 尝试多个可能的地址
    uint8_t part_id = 0;
    uint8_t try_addrs[] = {0x73, 0x70, 0x71, 0x72};
    bool found = false;
    for (int a = 0; a < 4; a++) {
        ESP_LOGI(TAG, "尝试地址 0x%02X...", try_addrs[a]);

        uint8_t dummy = 0;
        esp_err_t ret = i2c_master_write_to_device(PAJ7620_I2C_PORT, try_addrs[a], &dummy, 1, 50 / portTICK_PERIOD_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "地址 0x%02X 写入失败: %s", try_addrs[a], esp_err_to_name(ret));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));

        uint8_t buf[2] = {0xEF, 0x00};
        i2c_master_write_to_device(PAJ7620_I2C_PORT, try_addrs[a], buf, 2, 100 / portTICK_PERIOD_MS);
        uint8_t reg = 0x00;
        ret = i2c_master_write_read_device(PAJ7620_I2C_PORT, try_addrs[a], &reg, 1, &part_id, 1, 100 / portTICK_PERIOD_MS);

        ESP_LOGI(TAG, "地址 0x%02X -> Chip ID: 0x%02X (读取结果: %s)", try_addrs[a], part_id, esp_err_to_name(ret));
        if (part_id == 0x20) {
            paj7620_addr = try_addrs[a];
            found = true;
            ESP_LOGI(TAG, "找到 PAJ7620！地址: 0x%02X", paj7620_addr);
            break;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "PAJ7620 未找到！请检查接线 (SCL:%d SDA:%d)", PAJ7620_SCL, PAJ7620_SDA);
        return ESP_FAIL;
    }

    paj7620_wakeup();
    paj7620_write_reg(0xEF, 0x00);

    if (paj7620_write_array(init_array, sizeof(init_array) / sizeof(init_array[0])) != ESP_OK) return ESP_FAIL;
    if (paj7620_write_array(gesture_mode_array, sizeof(gesture_mode_array) / sizeof(gesture_mode_array[0])) != ESP_OK) return ESP_FAIL;

    paj7620_write_reg(0xEF, 0x00);

    is_initialized = true;
    ESP_LOGI(TAG, "PAJ7620 初始化完成！");
    return ESP_OK;
}

// ============================================================
//   优化后的手势检测任务
// ============================================================
static void gesture_task(void *pvParameters) {
    uint8_t gesture_data[2] = {0}; // 用一个本地缓冲区存放连续读取的寄存器数据

    while (s_run_gesture_task) { // 使用标志位，确保能安全退出循环

        // 核心优化：一次性连续读取 0x43 和 0x44 寄存器
        // 降低 I2C 事务开销，大幅优化 AR 眼镜整机功耗
        esp_err_t ret = paj7620_read_regs(0x43, gesture_data, 2);

        if (ret == ESP_OK) {
            // 读取成功，重置连续错误计数
            s_consecutive_errors = 0;

            uint8_t reg0 = gesture_data[0]; // 0x43 的内容
            uint8_t reg1 = gesture_data[1]; // 0x44 的内容

            // 打印原始寄存器数据（每50次打印一次）
            static int print_count = 0;
            if (++print_count >= 50) {
                ESP_LOGI(TAG, "原始数据 | REG_0x43: 0x%02X | REG_0x44: 0x%02X", reg0, reg1);
                print_count = 0;
            }

            ui_cmd_t cmd = UI_CMD_NONE;

            // 优先处理 0x43 寄存器的 8 种基础方向手势
            if (reg0 != 0) {
                switch (reg0) {
                    case 0x01: cmd = UI_CMD_RIGHT;   ESP_LOGI(TAG, "→ 右"); break;
                    case 0x02: cmd = UI_CMD_LEFT;    ESP_LOGI(TAG, "← 左"); break;
                    case 0x04: cmd = UI_CMD_UP;      ESP_LOGI(TAG, "↑ 上"); break;
                    case 0x08: cmd = UI_CMD_DOWN;    ESP_LOGI(TAG, "↓ 下"); break;
                    case 0x10: cmd = UI_CMD_FORWARD;  ESP_LOGI(TAG, "⊗ 向前靠近"); break;
                    case 0x20: cmd = UI_CMD_BACKWARD; ESP_LOGI(TAG, "⊙ 向后远离"); break;
                    case 0x40: cmd = UI_CMD_CIRCLE;   ESP_LOGI(TAG, "↻ 顺时针画圈"); break;
                    case 0x80: cmd = UI_CMD_CIRCLE;   ESP_LOGI(TAG, "↺ 逆时针画圈"); break;
                    default:   ESP_LOGI(TAG, "未定义手势: 0x%02X", reg0); break;
                }
            }
            // 协同读取 0x44 寄存器（挥手手势）
            else if (reg1 & 0x01) {
                cmd = UI_CMD_WAVE;
                ESP_LOGI(TAG, "👋 快速挥手");
            }

            // 发送指令到队列
            if (cmd != UI_CMD_NONE && ui_cmd_queue != NULL) {
                xQueueSend(ui_cmd_queue, &cmd, 0);
            }

            // 防抖冷却：如果触发了任何有效动作，延迟等待
            if (reg0 != 0 || (reg1 & 0x01)) {
                vTaskDelay(pdMS_TO_TICKS(400));
                continue;
            }
        } else {
            // 读取失败，累加错误计数
            s_consecutive_errors++;
            ESP_LOGW(TAG, "⚠️ PAJ7620 读取失败 (连续第 %d 次): %s", s_consecutive_errors, esp_err_to_name(ret));

            // 连续失败超过阈值，尝试重新初始化
            if (s_consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGE(TAG, "❌ 连续读取失败 %d 次，触发自动重置", s_consecutive_errors);

                if (paj7620_reinit() != ESP_OK) {
                    ESP_LOGE(TAG, "❌ 重新初始化也失败，任务退出");
                    break;  // 彻底失败，退出任务
                }

                // 重置后等待一段时间再继续
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // 20Hz 稳定采样
    }

    // 当 s_run_gesture_task 变为 false，任务会运行到这里，安全地释放所有内部资源
    gesture_task_handle = NULL;
    ESP_LOGI(TAG, "手势检测任务安全销毁");
    vTaskDelete(NULL); // 任务自我销毁，绝不拖欠 Mutex 锁
}

// ============================================================
//   安全启动/停止任务
// ============================================================
esp_err_t paj7620_start_task(void) {
    if (!is_initialized) return ESP_ERR_INVALID_STATE;
    if (gesture_task_handle != NULL) return ESP_OK;

    s_run_gesture_task = true; // 允许任务运行
    BaseType_t ret = xTaskCreatePinnedToCore(gesture_task, "gesture_task", GESTURE_TASK_STACK,
                                             NULL, GESTURE_TASK_PRIO, &gesture_task_handle, 0);
    if (ret != pdPASS) {
        s_run_gesture_task = false;
        ESP_LOGE(TAG, "手势任务创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "手势检测任务已启动");
    return ESP_OK;
}

esp_err_t paj7620_stop_task(void) {
    if (gesture_task_handle == NULL) return ESP_OK;

    s_run_gesture_task = false; // 发出安全退出信号

    // 优雅等待任务在两轮轮询（约100ms）之内自行释放资源并自销毁
    int timeout_counter = 0;
    while (gesture_task_handle != NULL && timeout_counter < 10) {
        vTaskDelay(pdMS_TO_TICKS(20));
        timeout_counter++;
    }

    // 极其罕见的兜底逻辑：如果任务真死锁在底层了，才执行强杀（带有警告）
    if (gesture_task_handle != NULL) {
        ESP_LOGW(TAG, "任务未能在规定时间内响应退出，强行注销");
        vTaskDelete(gesture_task_handle);
        gesture_task_handle = NULL;
    }

    ESP_LOGI(TAG, "手势检测任务已完全停止");
    return ESP_OK;
}