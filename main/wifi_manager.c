/**
 * @file wifi_manager.c
 * @brief WiFi管理器 - 统一管理ESP32的WiFi功能
 *
 * 【模块功能】
 * 1. WiFi初始化（STA+AP模式支持）
 * 2. 连接到指定的WiFi热点（STA模式）
 * 3. 开启自身热点（AP模式）
 * 4. 扫描周围WiFi网络
 * 5. WiFi事件处理（连接、断开、获取IP等）
 *
 * 【WiFi工作模式说明】
 * STA模式:  作为客户端连接到其他热点（上网模式）
 * AP模式:   作为热点，允许其他设备连接（配网模式）
 * APSTA模式: 同时作为热点和客户端（本项目配网时使用）
 *
 * 【事件驱动架构】
 * ESP-IDF的WiFi使用事件循环机制：
 * - WiFi驱动产生事件（连接、断开等）
 * - 事件通过默认事件循环分发
 * - 注册的回调函数处理事件
 */

#include "wifi_manager.h"
#include "esp_event.h" // 事件循环
#include "esp_log.h"   // 日志系统
#include "esp_mac.h"   // MAC地址宏（MACSTR, MAC2STR）
#include "esp_netif.h" // 网络接口层（TCP/IP）
#include "esp_netif_types.h"
#include "esp_wifi.h" // WiFi驱动
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h" // 信号量
#include "freertos/task.h"
#include "lwip/ip4_addr.h" // IP地址宏
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "wifi_manager"
/*============================================================================
 *                           配置常量
 *============================================================================*/

/** AP模式的默认SSID（热点名称） */
#define DEFAULT_AP_SSID "ESP32_wifi"

/** AP模式的默认密码 */
#define DEFAULT_AP_PASSWORD "12345678"

/** STA模式连接失败后的最大重试次数 */
#define MAX_CONNECT_RETRY 6

/*============================================================================
 *                           模块静态变量
 *============================================================================*/

/** STA已连接尝试次数（用于限制重试） */
static int sta_connect_count = 0;

/** AP网络接口句柄（用于配置AP的IP地址） */
static esp_netif_t *ap_netif = NULL;

/** WiFi状态变化回调函数（通知上层应用） */
static p_wifi_state_callback wifi_state_cb = NULL;

/** WiFi扫描任务句柄 */
static TaskHandle_t scan_task_handle = NULL;

/** 扫描信号量（防止并发扫描） */
static SemaphoreHandle_t scan_semaphore = NULL;

/**
 * WiFi扫描任务上下文结构体
 * 用于将回调函数传递给扫描任务
 */
typedef struct
{
    p_wifi_scan_callback cb; // 扫描完成回调
} wifi_scan_task_ctx_t;

/** STA当前是否已连接（用于判断断开事件是否需要通知） */
static bool is_sta_connected = false;

/*============================================================================
 *                           WiFi事件处理
 *============================================================================*/

/**
 * @brief WiFi和IP事件统一处理函数
 *
 * @param arg        用户注册时传入的参数（本项目中为NULL）
 * @param event_base 事件基类（WIFI_EVENT或IP_EVENT）
 * @param event_id   具体事件ID
 * @param event_data 事件携带的数据（不同事件有不同结构体）
 *
 * 【ESP-IDF事件系统架构】
 * 1. 系统组件（WiFi驱动、TCP/IP栈等）产生事件
 * 2. 事件发送到默认事件循环
 * 3. 事件循环根据注册信息调用回调函数
 * 4. 回调函数在事件任务上下文中执行
 *
 * 【注意】回调函数中不应执行耗时操作，会阻塞事件处理
 */
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    /*------------------------------------------------------------------------
     * WiFi事件处理
     *------------------------------------------------------------------------*/
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        /**
         * STA模式启动事件
         * 触发时机: esp_wifi_start()调用后，WiFi以STA模式启动完成
         * 处理逻辑: 如果是纯STA模式，自动开始连接
         */
        case WIFI_EVENT_STA_START:
        {
            wifi_mode_t mode;
            esp_wifi_get_mode(&mode);
            if (mode == WIFI_MODE_STA)
            {
                ESP_LOGI(TAG, "STA模式启动，开始连接...");
                esp_wifi_connect(); // 启动WiFi连接
            }
            break;
        }

        /**
         * STA连接成功事件
         * 触发时机: 与路由器建立WiFi连接（但还未获取IP）
         * 注意: 这不意味着可以上网，需要等待IP_EVENT_STA_GOT_IP
         */
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已连接到路由器（等待获取IP）");
            break;

        /**
         * STA断开连接事件
         * 触发时机: 与路由器断开连接（信号不好、密码错误、路由器关闭等）
         * 处理逻辑:
         *   1. 如果之前已连接，通知上层断开
         *   2. 在最大重试次数内自动重连
         */
        case WIFI_EVENT_STA_DISCONNECTED:
            // 如果之前已连接成功，通知上层断开事件
            if (is_sta_connected)
            {
                if (wifi_state_cb)
                    wifi_state_cb(WIFI_STATE_DISCONNECTED);
                is_sta_connected = false;
            }

            // 还有重试次数，继续尝试连接
            if (sta_connect_count < MAX_CONNECT_RETRY)
            {
                wifi_mode_t mode;
                esp_wifi_get_mode(&mode);
                if (mode == WIFI_MODE_STA)
                {
                    ESP_LOGI(TAG, "WiFi断开，第%d次重连...", sta_connect_count + 1);
                    esp_wifi_connect();
                }
                sta_connect_count++;
            }
            else
            {
                ESP_LOGW(TAG, "达到最大重试次数(%d)，停止重连", MAX_CONNECT_RETRY);
            }
            break;

        /**
         * AP模式 - 有客户端连接
         * 触发时机: 手机连接到ESP32热点
         */
        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "客户端连接到AP, MAC: " MACSTR, MAC2STR(event->mac));
            break;
        }

        /**
         * AP模式 - 客户端断开
         */
        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "客户端断开, MAC: " MACSTR, MAC2STR(event->mac));
            break;
        }

        default:
            break;
        }
    }

    /*------------------------------------------------------------------------
     * IP事件处理
     *------------------------------------------------------------------------*/
    if (event_base == IP_EVENT)
    {
        switch (event_id)
        {
        /**
         * 获取IP地址事件
         * 触发时机: DHCP客户端从路由器获取到IP地址
         * 意义: 这时才算真正连接成功，可以进行网络通信
         */
        case IP_EVENT_STA_GOT_IP:
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "获取IP地址: " IPSTR, IP2STR(&event->ip_info.ip));
            is_sta_connected = true;
            sta_connect_count = 0; // 重置重试计数器

            // 通知上层应用WiFi已连接
            if (wifi_state_cb)
                wifi_state_cb(WIFI_STATE_CONNECTED);
            break;
        }
        default:
            break;
        }
    }
}

/*============================================================================
 *                           初始化函数
 *============================================================================*/

/**
 * @brief 初始化WiFi管理器
 *
 * @param f WiFi状态变化回调函数
 *
 * 【初始化顺序很重要】
 * 1. esp_netif_init()        - 初始化TCP/IP协议栈
 * 2. esp_event_loop_create() - 创建事件循环
 * 3. esp_netif_create_xxx()  - 创建网络接口
 * 4. esp_wifi_init()         - 初始化WiFi驱动
 * 5. esp_event_handler_register() - 注册事件回调
 * 6. esp_wifi_set_mode()     - 设置WiFi模式
 * 7. esp_wifi_start()        - 启动WiFi
 */
void wifi_manager_init(p_wifi_state_callback f)
{
    /*------------------------------------------------------------------------
     * 第一部分：网络协议栈初始化
     *------------------------------------------------------------------------*/

    // 初始化lwIP（TCP/IP协议栈）
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认事件循环（用于分发系统事件）
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建STA和AP的网络接口
    // 这些函数创建TCP/IP栈与WiFi驱动的桥接层
    esp_netif_create_default_wifi_sta();           // 创建STA网络接口
    ap_netif = esp_netif_create_default_wifi_ap(); // 创建并保存AP网络接口句柄

    /*------------------------------------------------------------------------
     * 第二部分：WiFi驱动初始化
     *------------------------------------------------------------------------*/

    // 使用默认配置初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /*------------------------------------------------------------------------
     * 第三部分：注册事件回调
     *------------------------------------------------------------------------*/

    // 注册WiFi事件回调（所有WiFi事件）
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,       // 事件基类
        ESP_EVENT_ANY_ID, // 所有WiFi事件
        &event_handler,   // 回调函数
        NULL));           // 用户参数

    // 注册IP事件回调（仅关心获取IP事件）
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &event_handler,
        NULL));

    /*------------------------------------------------------------------------
     * 第四部分：保存回调和创建同步原语
     *------------------------------------------------------------------------*/

    wifi_state_cb = f; // 保存上层回调函数

    // 创建扫描信号量（用于防止并发扫描）
    scan_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(scan_semaphore); // 初始状态为可用

    /*------------------------------------------------------------------------
     * 第五部分：启动WiFi
     *------------------------------------------------------------------------*/

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // 设置为STA模式
    ESP_ERROR_CHECK(esp_wifi_start());                 // 启动WiFi

    ESP_LOGI(TAG, "WiFi初始化完成");
}

/*============================================================================
 *                           WiFi连接函数
 *============================================================================*/

/**
 * @brief 连接到指定的WiFi网络
 *
 * @param ssid     WiFi名称
 * @param password WiFi密码
 * @return ESP_OK 成功启动连接（不代表连接成功，连接结果通过回调通知）
 *
 * 【注意】
 * 这个函数是异步的：
 * - 函数返回只表示启动了连接过程
 * - 实际连接结果通过wifi_state_cb回调通知
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "连接WiFi: %s", ssid);

    sta_connect_count = 0; // 重置重试计数器

    // 配置WiFi连接参数
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // 最低可接受的加密方式
        },
    };

    // 使用snprintf安全复制字符串（防止缓冲区溢出）
    snprintf((char *)wifi_config.sta.ssid, 31, "%s", ssid);
    snprintf((char *)wifi_config.sta.password, 63, "%s", password);

    // 先断开现有连接
    ESP_ERROR_CHECK(esp_wifi_disconnect());

    // 检查当前模式，如果不是STA模式需要切换
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode != WIFI_MODE_STA)
    {
        // 从其他模式切换到STA模式
        ESP_LOGI(TAG, "切换到STA模式");
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_start(); // 启动后会触发WIFI_EVENT_STA_START，然后自动连接
    }
    else
    {
        // 已经是STA模式，直接配置并连接
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_connect();
    }

    return ESP_OK;
}
/*============================================================================
 *                           AP模式函数
 *============================================================================*/

/**
 * @brief 开启AP热点模式
 *
 * @return ESP_OK 成功
 *
 * 【AP配置说明】
 * - SSID: ESP32_wifi
 * - 密码: 12345678
 * - IP地址: 192.168.100.1
 * - 信道: 5
 * - 最大连接数: 2
 *
 * 【工作模式选择】
 * 使用WIFI_MODE_APSTA而不是WIFI_MODE_AP的原因：
 * APSTA模式允许同时作为热点和客户端，
 * 配网时可以扫描周围WiFi
 */
esp_err_t wifi_manager_ap(void)
{
    // 检查当前模式，如果已经是APSTA模式则不需要重新配置
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA)
    {
        ESP_LOGI(TAG, "已经是APSTA模式，无需重新配置");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "切换到AP+STA模式");

    // 停止当前WiFi
    esp_wifi_disconnect();
    esp_wifi_stop();

    // 设置为APSTA模式
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    // 配置AP参数
    wifi_config_t wifi_config = {
        .ap = {
            .channel = 5,                   // WiFi信道
            .max_connection = 2,            // 最大连接数
            .authmode = WIFI_AUTH_WPA2_PSK, // 加密方式
        },
    };

    // 设置SSID和密码
    snprintf((char *)wifi_config.ap.ssid, 31, "%s", DEFAULT_AP_SSID);
    wifi_config.ap.ssid_len = strlen(DEFAULT_AP_SSID);
    snprintf((char *)wifi_config.ap.password, 63, "%s", DEFAULT_AP_PASSWORD);

    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);

    /*------------------------------------------------------------------------
     * 配置AP的静态IP地址
     * 默认AP的IP是192.168.4.1，这里改成192.168.100.1
     *------------------------------------------------------------------------*/
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 100, 1);      // AP的IP地址
    IP4_ADDR(&ip_info.gw, 192, 168, 100, 1);      // 网关地址
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0); // 子网掩码

    // 必须先停止DHCP服务器才能修改IP
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif); // 重新启动DHCP服务器

    ESP_LOGI(TAG, "AP已启动 - SSID: %s, IP: 192.168.100.1", DEFAULT_AP_SSID);

    return esp_wifi_start();
}
/*============================================================================
 *                           WiFi扫描函数
 *============================================================================*/

/**
 * @brief WiFi扫描任务（在单独的FreeRTOS任务中执行）
 *
 * @param pvParameters 任务参数（wifi_scan_task_ctx_t*）
 *
 * 【为什么用单独的任务】
 * esp_wifi_scan_start()在阻塞模式下会占用较长时间
 * 如果在主任务或回调中执行，会影响系统响应
 */
static void scan_task(void *pvParameters)
{
    wifi_scan_task_ctx_t *ctx = (wifi_scan_task_ctx_t *)pvParameters;

    // 执行WiFi扫描（阻塞模式，扫描完成后返回）
    wifi_scan_config_t scan_config = {0}; // 使用默认配置，扫描所有信道
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);

    if (err == ESP_OK)
    {
        // 获取扫描到的AP数量
        uint16_t ap_num = 0;
        err = esp_wifi_scan_get_ap_num(&ap_num);

        if (err == ESP_OK && ap_num > 0)
        {
            ESP_LOGI(TAG, "扫描到 %d 个WiFi网络", ap_num);

            // 分配内存存储扫描结果
            wifi_ap_record_t *ap_records = malloc(sizeof(*ap_records) * ap_num);
            if (ap_records)
            {
                uint16_t ap_count = ap_num;
                err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);

                if (err == ESP_OK && ctx && ctx->cb)
                {
                    // 调用上层回调处理扫描结果
                    ctx->cb(ap_records, (int)ap_count);
                }

                free(ap_records);
            }
            else
            {
                ESP_LOGE(TAG, "分配扫描结果内存失败");
                if (ctx && ctx->cb)
                    ctx->cb(NULL, 0);
            }
        }
        else
        {
            ESP_LOGI(TAG, "未扫描到WiFi网络");
            if (ctx && ctx->cb)
                ctx->cb(NULL, 0);
        }
    }
    else
    {
        ESP_LOGE(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        if (ctx && ctx->cb)
            ctx->cb(NULL, 0);
    }

    // 清理工作
    xSemaphoreGive(scan_semaphore); // 释放信号量，允许下次扫描
    scan_task_handle = NULL;
    free(ctx);
    vTaskDelete(NULL); // 删除自己
}

/**
 * @brief 启动WiFi扫描
 *
 * @param f 扫描完成回调函数
 * @return ESP_OK 成功启动扫描
 *         ESP_ERR_INVALID_STATE 上一次扫描未完成
 *         ESP_ERR_NO_MEM 内存不足
 *
 * 【线程安全】
 * 使用信号量防止并发扫描，多次调用会返回INVALID_STATE
 */
esp_err_t wifi_manager_scan(p_wifi_scan_callback f)
{
    /**
     * 尝试获取信号量（等待时间0，立即返回）
     * 如果获取成功，说明没有正在进行的扫描
     */
    if (pdTRUE == xSemaphoreTake(scan_semaphore, 0))
    {
        // 清除之前的扫描结果
        esp_wifi_clear_ap_list();

        // 再次检查任务句柄
        if (scan_task_handle)
        {
            xSemaphoreGive(scan_semaphore);
            return ESP_ERR_INVALID_STATE;
        }

        // 创建任务上下文
        wifi_scan_task_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx)
        {
            xSemaphoreGive(scan_semaphore);
            return ESP_ERR_NO_MEM;
        }
        ctx->cb = f;

        // 创建扫描任务
        BaseType_t wifi_ap_scan = xTaskCreatePinnedToCore(
            scan_task,         // 任务函数
            "scan_task",       // 任务名称
            4096,              // 栈大小
            ctx,               // 任务参数
            5,                 // 优先级
            &scan_task_handle, // 任务句柄
            1);                // 运行在核心1

        if (wifi_ap_scan != pdPASS)
        {
            ESP_LOGE(TAG, "创建扫描任务失败");
            free(ctx);
            scan_task_handle = NULL;
            xSemaphoreGive(scan_semaphore);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "WiFi扫描任务已启动");
        return ESP_OK;
    }
    else
    {
        // 上一次扫描还未完成
        ESP_LOGW(TAG, "扫描进行中，请稍后重试");
        return ESP_ERR_INVALID_STATE;
    }
}
