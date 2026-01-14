#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "wifi_manager"
#define DEFAULT_AP_SSID "ESP32_wifi"
#define DEFAULT_AP_PASSWORD "12345678"
// 重连次数
#define MAX_CONNECT_RETRY 6
static int sta_connect_count = 0;
static esp_netif_t *ap_netif = NULL;
// 回调函数
static p_wifi_state_callback wifi_state_cb = NULL;
static TaskHandle_t scan_task_handle = NULL;
static SemaphoreHandle_t scan_semaphore = NULL;
typedef struct
{
    p_wifi_scan_callback cb;
} wifi_scan_task_ctx_t;

// 当前sta连接状态
static bool is_sta_connected = false;

/** 事件回调函数
 * @param arg   用户传递的参数
 * @param event_base    事件类别
 * @param event_id      事件ID
 * @param event_data    事件携带的数据
 * @return 无
 */
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START: // WIFI以STA模式启动后触发此事件
        {
            wifi_mode_t mode;
            esp_wifi_get_mode(&mode);
            if (mode == WIFI_MODE_STA)
                esp_wifi_connect(); // 启动WIFI连接
            break;
        }
        case WIFI_EVENT_STA_CONNECTED: // WIFI连上路由器后，触发此事件
            ESP_LOGI(TAG, "Connected to AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: // WIFI从路由器断开连接后触发此事件
            if (is_sta_connected)
            {
                if (wifi_state_cb)
                    wifi_state_cb(WIFI_STATE_DISCONNECTED);
                is_sta_connected = false;
            }
            if (sta_connect_count < MAX_CONNECT_RETRY)
            {
                wifi_mode_t mode;
                esp_wifi_get_mode(&mode);
                if (mode == WIFI_MODE_STA)
                    esp_wifi_connect(); // 继续重连
                sta_connect_count++;
            }
            ESP_LOGI(TAG, "connect to the AP fail,retry now");
            break;
        case WIFI_EVENT_AP_STACONNECTED: // WIFI AP模式下，有新的STA连接上后触发此事件
            ESP_LOGI(TAG, "Connected to ap");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED: // WIFI AP模式下，有新的STA断开连接后触发此事件
            ESP_LOGI(TAG, "Disconnected from ap");
            break;
        default:
            break;
        }
    }
    if (event_base == IP_EVENT) // IP相关事件
    {
        switch (event_id)
        {
        case IP_EVENT_STA_GOT_IP: // 只有获取到路由器分配的IP，才认为是连上了路由器
            ESP_LOGI(TAG, "Get ip address");
            is_sta_connected = true;
            if (wifi_state_cb)
                wifi_state_cb(WIFI_STATE_CONNECTED);
            break;
        default:
            break;
        }
    }
}

/** 初始化wifi，默认进入STA模式
 * @param 无
 * @return 无
 */
void wifi_manager_init(p_wifi_state_callback f)
{
    ESP_ERROR_CHECK(esp_netif_init()); // 用于初始化tcpip协议栈
    ESP_ERROR_CHECK(
        esp_event_loop_create_default()); // 创建一个默认系统事件调度循环，之后可以注册回调函数来处理系统的一些事件
    esp_netif_create_default_wifi_sta();           // 使用默认配置创建STA
    ap_netif = esp_netif_create_default_wifi_ap(); // 使用默认配置创建AP
    // 初始化WIFI
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_state_cb = f;
    scan_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(scan_semaphore);
    // 启动WIFI
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); // 设置工作模式为STA
    ESP_ERROR_CHECK(esp_wifi_start());                 // 启动WIFI

    ESP_LOGI(TAG, "wifi_init finished.");
}

/** 连接wifi
 * @param ssid
 * @param password
 * @return 成功/失败
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    sta_connect_count = 0;
    wifi_config_t wifi_config = {
        .sta =
            {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK, // 加密方式
            },
    };
    snprintf((char *)wifi_config.sta.ssid, 31, "%s", ssid);
    snprintf((char *)wifi_config.sta.password, 63, "%s", password);
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA)
    {
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_start();
    }
    else
    {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_connect();
    }
    return ESP_OK;
}
/** 开启AP模式
 * @return 成功/失败
 */
esp_err_t wifi_manager_ap(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA)
    {
        return ESP_OK;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t wifi_config = {
        .ap =
            {
                .channel = 5,
                .max_connection = 2,
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
    };
    snprintf((char *)wifi_config.ap.ssid, 31, "%s", DEFAULT_AP_SSID);
    wifi_config.ap.ssid_len = strlen(DEFAULT_AP_SSID);
    snprintf((char *)wifi_config.ap.password, 63, "%s", DEFAULT_AP_PASSWORD);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 100, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 100, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);
    return esp_wifi_start();
}
static void scan_task(void *pvParameters)
{
    wifi_scan_task_ctx_t *ctx = (wifi_scan_task_ctx_t *)pvParameters;
    wifi_scan_config_t scan_config = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);

    if (err == ESP_OK)
    {
        uint16_t ap_num = 0;
        err = esp_wifi_scan_get_ap_num(&ap_num);
        if (err == ESP_OK && ap_num > 0)
        {
            wifi_ap_record_t *ap_records = malloc(sizeof(*ap_records) * ap_num);
            if (ap_records)
            {
                uint16_t ap_count = ap_num;
                err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
                if (err == ESP_OK && ctx && ctx->cb)
                {
                    ctx->cb(ap_records, (int)ap_count);
                }
                ESP_LOGI(TAG, "scan_task: %d ap found", ap_count);
                free(ap_records);
            }
            else
            {
                if (ctx && ctx->cb)
                    ctx->cb(NULL, 0);
            }
        }
        else
        {
            if (ctx && ctx->cb)
                ctx->cb(NULL, 0);
        }
    }
    else
    {
        if (ctx && ctx->cb)
            ctx->cb(NULL, 0);
    }
    xSemaphoreGive(scan_semaphore);
    scan_task_handle = NULL;
    free(ctx);
    vTaskDelete(NULL);
}
esp_err_t wifi_manager_scan(p_wifi_scan_callback f)
{
    if (pdTRUE == xSemaphoreTake(scan_semaphore, 0))
    {
        esp_wifi_clear_ap_list();
        if (scan_task_handle)
            return ESP_ERR_INVALID_STATE;

        wifi_scan_task_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx)
            return ESP_ERR_NO_MEM;

        ctx->cb = f;

        BaseType_t ok = xTaskCreatePinnedToCore(scan_task, "scan_task", 4096, ctx, 5, &scan_task_handle, 1);
        if (ok != pdPASS)
        {
            free(ctx);
            scan_task_handle = NULL;
            return ESP_FAIL;
        }
        return ok == pdPASS ? ESP_OK : ESP_FAIL;
    }
    else
    {
        return ESP_ERR_INVALID_STATE;
    }
}
