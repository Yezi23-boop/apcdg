/**
 * @file ap_wifi.c
 * @brief AP配网模块 - 提供Web界面的WiFi配置功能
 *
 * 【模块功能】
 * 1. 初始化SPIFFS文件系统，加载配网网页HTML
 * 2. 提供AP配网入口函数 ap_wifi_apcfg()
 * 3. 处理WebSocket消息（扫描WiFi、接收用户配置）
 * 4. 使用事件组同步WiFi连接操作
 *
 * 【配网流程】
 * 用户按键 → 开启AP热点 → 启动WebSocket服务器 →
 * 用户连接热点访问网页 → 扫描WiFi → 用户选择并输入密码 →
 * ESP32连接目标WiFi → 关闭AP和服务器
 */

#include "ap_wifi.h"
#include "esp_spiffs.h"   // SPIFFS文件系统（存储HTML网页）
#include "wifi_manager.h" // WiFi管理模块
#include "ws_server.h"    // WebSocket服务器
#include <cJSON.h>        // JSON解析库
#include <esp_log.h>      // 日志系统
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h> // 文件状态（用于获取文件大小）

/** SPIFFS挂载路径（访问文件时的前缀） */
#define SPIFFS_BASE_PATH "/spiffs"

/** 配网HTML页面在SPIFFS中的完整路径 */
#define SPIFFS_HTML_PATH "/spiffs/apcfg.html"

#define TAG "ap_wifi"

/*============================================================================
 *                           模块静态变量
 *============================================================================*/

/**
 * 用户选择的WiFi SSID（最大32字节+结束符）
 * WiFi标准规定SSID最长32字节
 */
static char current_ssid[33] = {0};

/**
 * 用户输入的WiFi密码（最大64字节+结束符）
 * WPA2密码最长63个ASCII字符或64个十六进制字符
 */
static char current_password[65] = {0};

/**
 * 配网HTML网页内容（从SPIFFS加载）
 * 存储在堆内存中，程序运行期间一直保持
 */
static char *html_code = NULL;

/**
 * 配网事件组句柄
 * 用于在WebSocket回调和配网任务之间同步
 */
static EventGroupHandle_t apcfg_ev;

/** 事件位：用户已提交WiFi配置，准备连接 */
#define APCFG_WIFI_CONNECTED_BIT BIT0
/** 事件位：WiFi连接失败，需要重新配网 */
#define APCFG_WIFI_FAIL_BIT BIT1
/** 事件位：WiFi连接成功 */
#define APCFG_WIFI_SUCCESS_BIT BIT2

/** 配网状态标志：true表示正在配网流程中 */
static bool is_configuring = false;

/** 用户注册的WiFi状态回调（传递给main.c） */
static p_wifi_state_callback user_wifi_cb = NULL;

/*============================================================================
 *                           SPIFFS文件操作
 *============================================================================*/

/**
 * @brief 从SPIFFS加载配网HTML页面到内存
 *
 * 【函数流程】
 * 1. 注册并挂载SPIFFS分区
 * 2. 使用stat()获取HTML文件大小
 * 3. 根据文件大小分配内存缓冲区
 * 4. 使用fread()读取文件内容
 *
 * @return char* 成功返回HTML内容指针，失败返回NULL
 * @note 返回的内存由调用者负责释放（但本项目中一直使用不释放）
 */
static char *init_web_page_buffer(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false};
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != 0)
    {
        printf("Failed to mount or format filesystem\n");
        return NULL;
    }

    // 获取HTML文件信息
    struct stat st;
    if (stat(SPIFFS_HTML_PATH, &st) != 0)
    {
        printf("Failed to stat file\n");
        return NULL;
    }
    // 根据文件大小分配缓冲区
    char *buffer = malloc(st.st_size + 1);
    if (buffer == NULL)
    {
        printf("Failed to allocate memory\n");
        return NULL;
    }

    // 读取文件内容
    FILE *f = fopen(SPIFFS_HTML_PATH, "r");
    if (f == NULL)
    {
        printf("Failed to open file\n");
        free(buffer);
        return NULL;
    }
    /**
     * fread参数说明：
     * @param buffer    目标缓冲区
     * @param 1         每个元素的大小（1字节）
     * @param st.st_size 要读取的元素数量（即文件大小）
     * @param f         文件指针
     * @return 实际读取的元素数量
     */
    fread(buffer, 1, st.st_size, f);
    buffer[st.st_size] = '\0'; // 添加字符串结束符（C字符串必须以\0结尾）
    fclose(f);

    return buffer;
}

/*============================================================================
 *                           配网后台任务
 *============================================================================*/

/**
 * @brief 发送配网状态JSON给网页
 *
 * @param status "connected" 或 "failed"
 * @param ssid   连接的WiFi名称
 * @param ip     IP地址（成功时）或NULL（失败时）
 */
static void send_status_to_web(const char *status, const char *ssid, const char *ip)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "ssid", ssid);
    if (ip)
    {
        cJSON_AddStringToObject(root, "ip", ip);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "发送配网状态: %s", json_str);
    ws_server_send((uint8_t *)json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(root);
}

static void ap_wifi_task(void *arg)
{
    EventBits_t ev; // 用于存储等待到的事件位
    /** 等待的所有事件位 */
    const EventBits_t ALL_BITS = APCFG_WIFI_CONNECTED_BIT | APCFG_WIFI_FAIL_BIT | APCFG_WIFI_SUCCESS_BIT;

    while (1)
    {
        /**
         * 等待多个事件：
         * - CONNECTED_BIT: 用户提交了WiFi配置
         * - FAIL_BIT: WiFi连接失败
         * - SUCCESS_BIT: WiFi连接成功
         */
        ev = xEventGroupWaitBits(apcfg_ev,
                                 ALL_BITS,
                                 pdTRUE,                    // 清除事件位
                                 pdFALSE,                   // 等待任意一个位
                                 pdMS_TO_TICKS(1000 * 10)); // 10秒超时

        // 【事件1】用户提交WiFi配置，开始连接
        if (ev & APCFG_WIFI_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "收到WiFi配置，准备连接: %s", current_ssid);
            // 保持APSTA模式和WebSocket连接，直接尝试连接WiFi
            wifi_manager_connect(current_ssid, current_password);
        }

        // 【事件2】WiFi连接失败
        if (ev & APCFG_WIFI_FAIL_BIT)
        {
            ESP_LOGW(TAG, "WiFi连接失败（密码错误或找不到热点）");

            // 直接通过现有WebSocket连接发送失败状态给网页
            // AP和WebSocket一直保持运行，用户可以直接重试
            send_status_to_web("failed", current_ssid, NULL);

            // 重置配网标志，允许用户重新尝试
            is_configuring = false;
        }

        // 【事件3】WiFi连接成功
        if (ev & APCFG_WIFI_SUCCESS_BIT)
        {
            char ip_str[16] = {0};
            wifi_manager_get_ip(ip_str);
            ESP_LOGI(TAG, "WiFi连接成功！IP: %s", ip_str);

            // 发送成功状态给网页
            send_status_to_web("connected", current_ssid, ip_str);

            // 配网完成
            is_configuring = false;

            // 延迟一段时间让网页收到消息
            vTaskDelay(pdMS_TO_TICKS(2000));

            // 关闭WebSocket服务器
            ws_server_stop();
            ESP_LOGI(TAG, "配网完成，已关闭WebSocket服务器");

            // 关闭AP热点，切换到纯STA模式
            wifi_manager_stop_ap();
            ESP_LOGI(TAG, "AP热点已关闭");
        }
    }
}

/**
 * @brief 内部WiFi状态回调 - 处理配网结果
 *
 * 当WiFi连接成功或失败时，设置对应事件位通知ap_wifi_task
 * 同时转发状态给用户回调
 */
static void internal_wifi_callback(WIFI_STATE state)
{
    switch (state)
    {
    case WIFI_STATE_CONNECTED:
        ESP_LOGI(TAG, "内部回调: WiFi连接成功");
        // 如果正在配网流程中，设置成功事件位
        if (is_configuring)
        {
            xEventGroupSetBits(apcfg_ev, APCFG_WIFI_SUCCESS_BIT);
        }
        // 转发给用户回调
        if (user_wifi_cb)
            user_wifi_cb(WIFI_STATE_CONNECTED);
        break;

    case WIFI_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "内部回调: WiFi断开");
        // 转发给用户回调
        if (user_wifi_cb)
            user_wifi_cb(WIFI_STATE_DISCONNECTED);
        break;

    case WIFI_STATE_CONNECT_FAIL:
        ESP_LOGW(TAG, "内部回调: WiFi连接失败");
        // 如果正在配网流程中，设置失败事件位
        if (is_configuring)
        {
            xEventGroupSetBits(apcfg_ev, APCFG_WIFI_FAIL_BIT);
        }
        // 转发给用户回调（用户可能也想知道）
        if (user_wifi_cb)
            user_wifi_cb(WIFI_STATE_CONNECT_FAIL);
        break;
    }
}

/*============================================================================
 *                           模块初始化
 *============================================================================*/

/**
 * @brief 初始化AP配网模块
 *
 * @param f WiFi状态变化的回调函数（连接成功/断开时调用）
 *
 * 【初始化顺序】
 * 1. 初始化WiFi管理器（包含WiFi驱动初始化）
 * 2. 从SPIFFS加载HTML网页
 * 3. 创建事件组（用于任务间同步）
 * 4. 创建后台任务（处理WiFi连接）
 */
void ap_wifi_init(p_wifi_state_callback f)
{
    // 保存用户回调
    user_wifi_cb = f;

    // 1. 初始化WiFi管理器，传入内部回调（用于处理配网结果）
    wifi_manager_init(internal_wifi_callback);

    // 2. 加载配网HTML页面到内存
    html_code = init_web_page_buffer();
    if (html_code == NULL)
    {
        ESP_LOGE(TAG, "加载HTML页面失败！请检查SPIFFS分区");
    }

    // 3. 创建事件组
    apcfg_ev = xEventGroupCreate();

    // 4. 创建配网任务
    // xTaskCreatePinnedToCore参数：
    // - 任务函数, 任务名, 栈大小, 参数, 优先级, 任务句柄, 核心ID
    xTaskCreatePinnedToCore(ap_wifi_task, "ap_wifi_task", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "AP配网模块初始化完成");
}

/**
 * @brief 连接到已保存的WiFi（预留接口）
 * TODO: 实现从NVS读取保存的WiFi配置并自动连接
 */
void ap_wifi_connect()
{
    // 未实现
}

/*============================================================================
 *                           WiFi扫描结果处理
 *============================================================================*/

/**
 * @brief WiFi扫描完成回调 - 将结果转为JSON并发送给网页
 *
 * @param ap       扫描到的AP记录数组
 * @param ap_count 扫描到的AP数量
 *
 * 【JSON格式示例】
 * {
 *   "wifi_list": [
 *     {"ssid": "MyWiFi", "rssi": -45, "encrypted": true},
 *     {"ssid": "OpenNet", "rssi": -70, "encrypted": false}
 *   ]
 * }
 * encrypted: false=开放网络, true=需要密码
 */
void wifi_scan_handle(wifi_ap_record_t *ap, int ap_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi_list = cJSON_AddArrayToObject(root, "wifi_list");
    for (int i = 0; i < ap_count; i++)
    {
        cJSON *ap_item = cJSON_CreateObject();
        cJSON_AddStringToObject(ap_item, "ssid", (const char *)ap[i].ssid);
        cJSON_AddNumberToObject(ap_item, "rssi", ap[i].rssi);
        if (ap[i].authmode == WIFI_AUTH_OPEN)
        {
            cJSON_AddBoolToObject(ap_item, "encrypted", cJSON_False);
        }
        else
        {
            cJSON_AddBoolToObject(ap_item, "encrypted", cJSON_True);
        }
        cJSON_AddItemToArray(wifi_list, ap_item);
    }
    /**
     * cJSON_Print: 将JSON对象转为格式化的字符串
     * 返回的字符串是动态分配的，必须用cJSON_free释放
     */
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "发送扫描结果: %s", json_str);

    // 通过WebSocket发送给浏览器
    ws_server_send((uint8_t *)json_str, strlen(json_str));

    /**
     * 【cJSON内存管理】
     * cJSON_free: 释放cJSON_Print返回的字符串
     * cJSON_Delete: 释放整个JSON对象树（包括所有子对象）
     * 两者都要调用，顺序不重要
     */
    cJSON_free(json_str);
    cJSON_Delete(root);
}

/*============================================================================
 *                           WebSocket消息处理
 *============================================================================*/

/**
 * @brief WebSocket消息接收回调 - 处理网页发来的指令
 *
 * @param data 接收到的消息内容（JSON格式）
 * @param len  消息长度
 *
 * 【支持的消息类型】
 * 1. 扫描请求: {"scan": "start"}
 * 2. 连接请求: {"ssid": "WiFi名称", "password": "密码"}
 *
 * 注意：此函数不能声明为static，因为ap_wifi_task中需要通过extern引用
 */
void ws_receive_handle(const char *data, int len)
{
    ESP_LOGI(TAG, "收到WebSocket消息: %.*s", len, data);

    // 解析JSON消息
    cJSON *root = cJSON_Parse(data);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "JSON解析失败");
        return;
    }

    // 尝试获取各个字段（不存在则返回NULL）
    cJSON *scan_js = cJSON_GetObjectItem(root, "scan");
    cJSON *ssid_js = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_js = cJSON_GetObjectItem(root, "password");

    // 处理扫描请求: {"scan": "start"}
    if (scan_js)
    {
        char *scan_value = cJSON_GetStringValue(scan_js);
        if (scan_value && strcmp(scan_value, "start") == 0)
        {
            ESP_LOGI(TAG, "开始WiFi扫描...");
            // 启动异步扫描，结果通过wifi_scan_handle回调返回
            wifi_manager_scan(wifi_scan_handle);
        }
    }

    // 处理连接请求: {"ssid": "xxx", "password": "xxx"}
    if (ssid_js && password_js)
    {
        char *ssid_value = cJSON_GetStringValue(ssid_js);
        char *password_value = cJSON_GetStringValue(password_js);

        if (ssid_value && password_value)
        {
            // 保存WiFi配置到模块变量
            // snprintf比strcpy更安全，会自动截断并添加结束符
            snprintf(current_ssid, sizeof(current_ssid), "%s", ssid_value);
            snprintf(current_password, sizeof(current_password), "%s", password_value);

            ESP_LOGI(TAG, "收到WiFi配置 - SSID: %s", current_ssid);

            // 标记进入配网流程（用于判断连接成功/失败事件）
            is_configuring = true;

            // 设置事件位，通知ap_wifi_task去执行连接
            // 这样可以立即返回给WebSocket，不会阻塞
            xEventGroupSetBits(apcfg_ev, APCFG_WIFI_CONNECTED_BIT);
        }
    }

    // 释放JSON对象
    cJSON_Delete(root);
}

/*============================================================================
 *                           配网模式入口
 *============================================================================*/

/**
 * @brief 启动AP配网模式
 *
 * 【调用时机】
 * 用户按下配网按钮时调用此函数
 *
 * 【执行步骤】
 * 1. 将WiFi切换到AP+STA模式，开启热点
 * 2. 配置并启动WebSocket服务器
 * 3. 等待用户通过网页提交配置（由ap_wifi_task处理）
 *
 * 【用户操作流程】
 * 1. 用户手机搜索并连接"ESP32_wifi"热点（密码：12345678）
 * 2. 浏览器访问 http://192.168.100.1
 * 3. 点击扫描，选择目标WiFi，输入密码
 * 4. ESP32收到配置后自动连接
 */
void ap_wifi_apcfg()
{
    ESP_LOGI(TAG, "========== 启动AP配网模式 ==========");
    ESP_LOGI(TAG, "热点名称: ESP32_wifi");
    ESP_LOGI(TAG, "热点密码: 12345678");
    ESP_LOGI(TAG, "配网地址: http://192.168.100.1");
    ESP_LOGI(TAG, "=====================================");

    // 1. 开启AP热点
    wifi_manager_ap();

    // 2. 配置WebSocket服务器
    ws_server_config_t config = {
        .html_code = html_code,  // HTML网页内容
        .cb = ws_receive_handle, // 消息接收回调
    };

    // 3. 启动服务器
    ws_server_start(&config);

    ESP_LOGI(TAG, "WebSocket服务器已启动，等待用户连接...");
}