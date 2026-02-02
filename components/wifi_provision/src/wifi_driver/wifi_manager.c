/**
 * @file wifi_manager.c
 * @brief WiFi 管理器实现 - 模块化重构版本
 */

#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "wifi_manager_private.h"
#include <stdio.h>
#include <string.h>

#define TAG "wifi_mgr"

static wifi_manager_config_internal_t g_config = WIFI_MANAGER_DEFAULT_CONFIG();
static int sta_connect_count = 0;
static esp_netif_t *ap_netif = NULL;
static p_wifi_state_callback wifi_state_cb = NULL;
static bool is_sta_connected = false;

static SemaphoreHandle_t scan_semaphore = NULL;
static TaskHandle_t scan_task_handle = NULL;

typedef struct
{
    p_wifi_scan_callback cb;
} wifi_scan_task_ctx_t;

/**
 * @brief 事件处理逻辑 (重构后的核心)
 */
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已连接到 AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (is_sta_connected)
            {
                if (wifi_state_cb)
                    wifi_state_cb(WIFI_STATE_DISCONNECTED);
                is_sta_connected = false;
            }
            if (sta_connect_count < g_config.max_retry)
            {
                esp_wifi_connect();
                sta_connect_count++;
                ESP_LOGI(TAG, "重试连接... (%d/%d)", sta_connect_count, g_config.max_retry);
            }
            else
            {
                if (wifi_state_cb)
                    wifi_state_cb(WIFI_STATE_CONNECT_FAIL);
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "客户端已连接到热点");
            break;
        default:
            break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
            is_sta_connected = true;
            sta_connect_count = 0;
            if (wifi_state_cb)
                wifi_state_cb(WIFI_STATE_CONNECTED);
        }
    }
}

void wifi_manager_init(p_wifi_state_callback f)
{
    wifi_state_cb = f;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    scan_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(scan_semaphore);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi 管理器初始化成功");
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    sta_connect_count = 0;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode != WIFI_MODE_APSTA && mode != WIFI_MODE_STA)
    {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }

    // 在设置新配置和连接前，先主动断开当前连接并清除状态，消除警告
    esp_wifi_disconnect();

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_ap(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA)
    {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(g_config.ap_ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK},
    };
    strncpy((char *)wifi_config.ap.ssid, g_config.ap_ssid, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, g_config.ap_password, sizeof(wifi_config.ap.password));

    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);

    // 配置 IP
    esp_netif_ip_info_t ip_info;
    int ip[4];
    sscanf(g_config.ap_ip, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]);
    IP4_ADDR(&ip_info.ip, ip[0], ip[1], ip[2], ip[3]);
    IP4_ADDR(&ip_info.gw, ip[0], ip[1], ip[2], ip[3]);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    return esp_wifi_start();
}

esp_err_t wifi_manager_stop_ap(void)
{
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

/**
 * @brief 扫描任务实现
 */
static void scan_task(void *pvParameters)
{
    wifi_scan_task_ctx_t *ctx = (wifi_scan_task_ctx_t *)pvParameters;
    wifi_scan_config_t scan_config = {0};

    if (esp_wifi_scan_start(&scan_config, true) == ESP_OK)
    {
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num > 0)
        {
            wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_num);
            if (ap_records)
            {
                esp_wifi_scan_get_ap_records(&ap_num, ap_records);
                if (ctx->cb)
                    ctx->cb(ap_records, ap_num);
                free(ap_records);
            }
        }
        else
        {
            if (ctx->cb)
                ctx->cb(NULL, 0);
        }
    }

    xSemaphoreGive(scan_semaphore);
    scan_task_handle = NULL;
    free(ctx);
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_scan(p_wifi_scan_callback f)
{
    if (xSemaphoreTake(scan_semaphore, 0) == pdTRUE)
    {
        wifi_scan_task_ctx_t *ctx = malloc(sizeof(wifi_scan_task_ctx_t));
        ctx->cb = f;
        xTaskCreate(scan_task, "wifi_scan", 4096, ctx, 5, &scan_task_handle);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_manager_get_ip(char *ip_str)
{
    if (!is_sta_connected)
        return ESP_FAIL;
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK)
    {
        sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
        return ESP_OK;
    }
    return ESP_FAIL;
}
