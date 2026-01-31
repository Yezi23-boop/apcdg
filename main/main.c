/**
 * @file main.c
 * @brief ESP32-S3 智能手表主程序 - 按键触发AP配网模式
 *
 * 【项目功能概述】
 * 本项目实现了一个基于ESP32-S3的WiFi配网系统：
 * 1. 用户按下BOOT按钮（GPIO0）触发配网模式
 * 2. ESP32开启AP热点，用户手机连接该热点
 * 3. 通过WebSocket网页界面选择要连接的WiFi并输入密码
 * 4. ESP32接收配置后连接到目标WiFi
 *
 * 【使用的组件】
 * - espressif__button: 乐鑫官方按键组件，支持单击/双击/长按等事件检测
 * - esp_http_server: HTTP服务器，提供网页和WebSocket通信
 * - SPIFFS: 存储HTML网页文件
 * - cJSON: JSON数据解析
 */

#include "ap_wifi.h"     // AP配网模块（本项目自定义）
#include "button_gpio.h" // GPIO按键驱动（espressif__button组件的一部分）
#include "driver/gpio.h"
#include "esp_log.h"    // ESP-IDF日志系统
#include "iot_button.h" // 按键组件主头文件（提供事件检测API）
#include "nvs.h"        // NVS存储（用于保存WiFi配置）
#include "nvs_flash.h"  // NVS Flash初始化
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#define TAG "MAIN"

/**
 * @brief BOOT按钮的GPIO引脚号
 *
 * ESP32-S3开发板上的BOOT按钮默认连接到GPIO0
 * 按下时GPIO0变为低电平，松开时为高电平（需要上拉电阻）
 */
#define BUTTON_GPIO_NUM GPIO_NUM_10

/*============================================================================
 *                           按键回调函数
 *============================================================================*/

/**
 * @brief 按键单击事件回调函数
 *
 * 【回调函数签名说明】
 * 所有按键事件回调函数必须遵循以下格式：
 *   void callback(void *button_handle, void *usr_data)
 *
 * @param button_handle 触发事件的按键句柄（可用于多按键共用回调时区分来源）
 * @param usr_data      用户自定义数据（注册回调时传入的最后一个参数）
 *
 * 【调用时机】
 * 当检测到"单击"事件时，按键组件会自动调用此函数
 * 单击定义：按下 -> 松开，且按下时间 < long_press_time
 */
static void button_single_click_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "按键单击！启动AP配网模式...");
    ESP_LOGI(TAG, "========================================");

    /**
     * 调用AP配网函数，执行以下操作：
     * 1. 切换WiFi到AP+STA模式
     * 2. 创建名为"ESP32_wifi"的热点（密码：12345678）
     * 3. 启动HTTP服务器和WebSocket服务
     * 4. 等待用户通过网页提交WiFi配置
     */
    ap_wifi_apcfg();
}

/*============================================================================
 *                           WiFi状态回调函数
 *============================================================================*/

/**
 * @brief WiFi连接状态变化回调
 *
 * @param state 当前WiFi状态：
 *              - WIFI_STATE_CONNECTED: 成功连接并获取IP
 *              - WIFI_STATE_DISCONNECTED: 断开连接
 */
static void wifi_state_callback(WIFI_STATE state)
{
    if (state == WIFI_STATE_CONNECTED)
    {
        ESP_LOGI(TAG, "✓ WiFi已连接！可以进行网络操作了");
    }
    else
    {
        ESP_LOGW(TAG, "✗ WiFi断开连接");
    }
}

/*============================================================================
 *                           主函数
 *============================================================================*/

/**
 * @brief 应用程序入口函数
 *
 * 【ESP-IDF程序执行流程】
 * 1. 芯片上电/复位
 * 2. 一级引导程序（ROM中）加载二级引导程序
 * 3. 二级引导程序加载应用程序
 * 4. 调用 app_main() 函数
 *
 * 【注意】app_main运行在一个FreeRTOS任务中，可以使用所有FreeRTOS API
 */
void app_main(void)
{
    ESP_LOGI(TAG, "==== ESP32-S3 智能手表启动 ====");

    /*------------------------------------------------------------------------
     * 第一步：初始化NVS（Non-Volatile Storage）
     * NVS用于存储WiFi配置、用户设置等需要掉电保存的数据
     *------------------------------------------------------------------------*/
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS分区被占满或版本变更，需要擦除重新初始化
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS初始化完成");

    /*------------------------------------------------------------------------
     * 第二步：初始化AP WiFi模块
     * 这会初始化WiFi驱动、SPIFFS文件系统、创建配网任务
     *------------------------------------------------------------------------*/
    ap_wifi_init(wifi_state_callback);
    ESP_LOGI(TAG, "AP WiFi模块初始化完成");

    /*------------------------------------------------------------------------
     * 第三步：配置按钮参数
     *
     * button_config_t 结构体定义了按键的时间参数：
     * - long_press_time: 长按判定时间（按住超过此时间视为长按）
     * - short_press_time: 消抖时间（按下后需保持此时间才算有效按下）
     *
     * 这些值可以在menuconfig中配置：
     * Component config → Button → Button debounce/long press time
     *------------------------------------------------------------------------*/
    button_config_t btn_cfg = {
        .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,   // 默认1500ms
        .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS, // 默认180ms（消抖）
    };

    /*------------------------------------------------------------------------
     * 第四步：配置GPIO参数
     *
     * button_gpio_config_t 定义了GPIO的电气特性：
     * - gpio_num: 使用哪个GPIO引脚
     * - active_level: 按钮按下时的电平（0=低电平触发，1=高电平触发）
     * - enable_power_save: 是否启用低功耗模式（使用GPIO中断唤醒）
     * - disable_pull: 是否禁用内部上/下拉电阻
     *
     * 【BOOT按钮电路分析】
     * BOOT按钮一端接GPIO0，另一端接GND
     * 需要上拉电阻保证松开时为高电平
     * 设置 active_level=1 表示检测高电平（按下时触发）
     * 设置 disable_pull=false 启用内部上拉电阻
     *------------------------------------------------------------------------*/
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_GPIO_NUM, // GPIO0
        .active_level = 1,           // 高电平有效（按下=高电平）
        .enable_power_save = false,  // 不启用省电模式
        .disable_pull = false,       // 启用内部上拉（false=不禁用=启用）
    };

    /*------------------------------------------------------------------------
     * 第五步：创建GPIO按钮设备
     *
     * iot_button_new_gpio_device() 函数：
     * - 创建一个FreeRTOS定时器，周期性检测GPIO电平
     * - 实现消抖逻辑，过滤抖动信号
     * - 识别各种按键事件（单击、双击、长按等）
     *
     * 参数说明：
     * @param btn_cfg   按钮配置（时间参数）
     * @param gpio_cfg  GPIO配置（引脚参数）
     * @param btn_handle 输出参数，返回创建的按钮句柄
     *
     * 【按钮句柄】
     * 句柄（handle）是一个指针，代表创建的按钮对象
     * 后续所有操作都需要通过这个句柄来指定操作哪个按钮
     *------------------------------------------------------------------------*/
    button_handle_t btn_handle = NULL;
    ret = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "按钮创建失败！错误码: %s", esp_err_to_name(ret));
        return; // 初始化失败，退出
    }
    ESP_LOGI(TAG, "按钮设备创建成功 (GPIO%d)", BUTTON_GPIO_NUM);

    /*------------------------------------------------------------------------
     * 第六步：注册按键事件回调函数
     *
     * iot_button_register_cb() 函数签名：
     * esp_err_t iot_button_register_cb(
     *     button_handle_t btn,        // 按钮句柄
     *     button_event_t event,       // 要监听的事件类型
     *     button_event_config_t *cfg, // 事件配置（通常为NULL使用默认）
     *     button_cb_t cb,             // 回调函数指针
     *     void *usr_data              // 用户数据（传递给回调函数）
     * )
     *
     * 【可注册的事件类型】
     * BUTTON_PRESS_DOWN      - 按下瞬间
     * BUTTON_PRESS_UP        - 松开瞬间
     * BUTTON_SINGLE_CLICK    - 单击（按下并松开）
     * BUTTON_DOUBLE_CLICK    - 双击
     * BUTTON_LONG_PRESS_START - 长按开始
     * BUTTON_LONG_PRESS_HOLD  - 长按保持中（周期性触发）
     * BUTTON_LONG_PRESS_UP    - 长按后松开
     *------------------------------------------------------------------------*/
    ret = iot_button_register_cb(btn_handle,             // 按钮句柄
                                 BUTTON_SINGLE_CLICK,    // 监听单击事件
                                 NULL,                   // 使用默认配置
                                 button_single_click_cb, // 回调函数
                                 NULL);                  // 无用户数据
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册回调失败！");
    }
    else
    {
        ESP_LOGI(TAG, "已注册单击事件回调");
    }

    /*------------------------------------------------------------------------
     * 初始化完成提示
     *------------------------------------------------------------------------*/
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "系统初始化完成！");
    ESP_LOGI(TAG, "按下BOOT按钮(GPIO0)进入AP配网模式");
    ESP_LOGI(TAG, "====================================");

    /*------------------------------------------------------------------------
     * 主循环
     *
     * 【为什么需要这个循环？】
     * 1. app_main是一个FreeRTOS任务，如果函数返回，任务会被删除
     * 2. 按钮检测在后台定时器中运行，不需要主循环处理
     * 3. 这个循环只是保持任务存活，可以在这里添加其他周期性任务
     *
     * 【vTaskDelay的作用】
     * - 让出CPU给其他任务运行
     * - 降低CPU占用率
     * - 1000ms检查一次，可以在这里添加状态监控代码
     *------------------------------------------------------------------------*/
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 延时1秒
        // 可以在这里添加周期性任务，如：
        // - 检查电池电量
        // - 更新时间显示
        // - 检查传感器数据
    }
}
