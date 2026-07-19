// ============================================================
// paj7620.c
// J.A.R.V.I.S. AR 智能眼镜 —— PAJ7620 手势识别传感器驱动 (优化稳定版)
// ============================================================

#include "paj7620.h"
#include "ui_globals.h"
#include "ui_manager.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PAJ7620";

// ---- 硬件配置 ----
#define PAJ7620_SCL         1
#define PAJ7620_SDA         2
#define PAJ7620_I2C_PORT    I2C_NUM_1
#define PAJ7620_I2C_FREQ    10000
static uint8_t paj7620_addr = 0x73; // 运行时自动探测
#define PAJ7620_TIMEOUT     1000

// ---- 任务配置 ----
#define GESTURE_TASK_STACK  4096
#define GESTURE_TASK_PRIO   5

static TaskHandle_t gesture_task_handle = NULL;
static bool is_initialized = false;
// 引入 volatile 保证跨线程修改的可见性，控制任务安全退出
static volatile bool s_run_gesture_task = false; 

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
//   初始化
// ============================================================
esp_err_t paj7620_init(void) {
    if (is_initialized) return ESP_OK;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PAJ7620_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = PAJ7620_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PAJ7620_I2C_FREQ,
    };
    esp_err_t err = i2c_param_config(PAJ7620_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(PAJ7620_I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C install failed: %s", esp_err_to_name(err));
        return err;
    }

    // 尝试两个可能的地址（0x73 和 0x70）
    uint8_t part_id = 0;
    uint8_t try_addrs[] = {0x73, 0x70};
    bool found = false;
    for (int a = 0; a < 2; a++) {
        // 临时用这个地址发唤醒命令
        uint8_t dummy = 0;
        i2c_master_write_to_device(PAJ7620_I2C_PORT, try_addrs[a], &dummy, 1, 50 / portTICK_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(5));

        // 写 Bank 0 并读芯片 ID
        uint8_t buf[2] = {0xEF, 0x00};
        i2c_master_write_to_device(PAJ7620_I2C_PORT, try_addrs[a], buf, 2, 100 / portTICK_PERIOD_MS);
        uint8_t reg = 0x00;
        i2c_master_write_read_device(PAJ7620_I2C_PORT, try_addrs[a], &reg, 1, &part_id, 1, 100 / portTICK_PERIOD_MS);

        ESP_LOGI(TAG, "地址 0x%02X -> Chip ID: 0x%02X", try_addrs[a], part_id);
        if (part_id == 0x20) {
            // 找到了！更新全局地址
            paj7620_addr = try_addrs[a];
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "PAJ7620 未找到！检查接线 (SCL:%d SDA:%d)", PAJ7620_SCL, PAJ7620_SDA);
        return ESP_FAIL;
    }

    paj7620_wakeup();
    paj7620_write_reg(0xEF, 0x00);

    if (paj7620_write_array(init_array, sizeof(init_array) / sizeof(init_array[0])) != ESP_OK) return ESP_FAIL;
    if (paj7620_write_array(gesture_mode_array, sizeof(gesture_mode_array) / sizeof(gesture_mode_array[0])) != ESP_OK) return ESP_FAIL;

    paj7620_write_reg(0xEF, 0x00); 

    is_initialized = true;
    ESP_LOGI(TAG, "PAJ7620 初始化完成 (SCL:%d SDA:%d)", PAJ7620_SCL, PAJ7620_SDA);
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
        if (paj7620_read_regs(0x43, gesture_data, 2) == ESP_OK) {
            uint8_t reg0 = gesture_data[0]; // 0x43 的内容
            uint8_t reg1 = gesture_data[1]; // 0x44 的内容

            ui_cmd_t cmd = UI_CMD_NONE;

            // 优先处理 0x43 寄存器的 8 种基础方向手势
            if (reg0 != 0) {
                switch (reg0) {
                    case 0x01: cmd = UI_CMD_LEFT;     ESP_LOGI(TAG, "← 左"); break;
                    case 0x02: cmd = UI_CMD_RIGHT;    ESP_LOGI(TAG, "→ 右"); break;
                    case 0x04: cmd = UI_CMD_DOWN;       ESP_LOGI(TAG, "↑ 上"); break;
                    case 0x08: cmd = UI_CMD_UP;     ESP_LOGI(TAG, "↓ 下"); break;
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