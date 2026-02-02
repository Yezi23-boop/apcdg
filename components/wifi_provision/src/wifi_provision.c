/**
 * @file wifi_provision.c
 * @brief WiFi 配网核心逻辑 - 协调 WiFi 驱动和 Web 服务器
 */

#include "wifi_provision.h"
#include "wifi_manager.h" // 内部模块
#include "ws_server.h"    // 内部模块
#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <string.h>

#define TAG "wifi_prov"

/* 引用嵌入的 HTML 资源 */
extern const uint8_t apcfg_html_start[] asm("_binary_apcfg_html_start");
extern const uint8_t apcfg_html_end[]   asm("_binary_apcfg_html_end");

/* 事件组定义 */
static EventGroupHandle_t prov_ev_group;
#define PROV_WIFI_CONNECTED_BIT BIT0
#define PROV_WIFI_FAIL_BIT      BIT1
#define PROV_WIFI_SUCCESS_BIT   BIT2

static char current_ssid[33] = {0};
static char current_password[65] = {0};
static bool is_configuring = false;
static wifi_provision_cb_t user_callback = NULL;

/**
 * @brief 发送状态到网页
 */
static void send_status_to_web(const char *status, const char *ssid, const char *ip)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "ssid", ssid);
    if (ip) {
        cJSON_AddStringToObject(root, "ip", ip);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    ws_server_send((uint8_t *)json_str, strlen(json_str));
    cJSON_free(json_str);
    cJSON_Delete(root);
}

/**
 * @brief 配网处理任务
 */
static void wifi_provision_task(void *arg)
{
    EventBits_t bits;
    const EventBits_t ALL_BITS = PROV_WIFI_CONNECTED_BIT | PROV_WIFI_FAIL_BIT | PROV_WIFI_SUCCESS_BIT;

    while (1) {
        bits = xEventGroupWaitBits(prov_ev_group, ALL_BITS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & PROV_WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "开始连接 WiFi: %s", current_ssid);
            wifi_manager_connect(current_ssid, current_password);
        }

        if (bits & PROV_WIFI_FAIL_BIT) {
            ESP_LOGW(TAG, "WiFi 连接失败");
            send_status_to_web("failed", current_ssid, NULL);
            is_configuring = false;
        }

        if (bits & PROV_WIFI_SUCCESS_BIT) {
            char ip_str[16] = {0};
            wifi_manager_get_ip(ip_str);
            ESP_LOGI(TAG, "WiFi 连接成功, IP: %s", ip_str);
            send_status_to_web("connected", current_ssid, ip_str);
            is_configuring = false;

            vTaskDelay(pdMS_TO_TICKS(2000));
            ws_server_stop();
            wifi_manager_stop_ap();
        }
    }
}

/**
 * @brief 内部 WiFi 状态回调
 */
static void internal_wifi_cb(WIFI_STATE state)
{
    switch (state) {
        case WIFI_STATE_CONNECTED:
            if (is_configuring) xEventGroupSetBits(prov_ev_group, PROV_WIFI_SUCCESS_BIT);
            if (user_callback) user_callback(WIFI_PROVISION_STATE_CONNECTED);
            break;
        case WIFI_STATE_DISCONNECTED:
            if (user_callback) user_callback(WIFI_PROVISION_STATE_DISCONNECTED);
            break;
        case WIFI_STATE_CONNECT_FAIL:
            if (is_configuring) xEventGroupSetBits(prov_ev_group, PROV_WIFI_FAIL_BIT);
            if (user_callback) user_callback(WIFI_PROVISION_STATE_CONNECT_FAIL);
            break;
    }
}

/**
 * @brief WiFi 扫描结果处理
 */
void wifi_scan_handle(wifi_ap_record_t *ap, int ap_count)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi_list = cJSON_AddArrayToObject(root, "wifi_list");
    for (int i = 0; i < ap_count; i++) {
        cJSON *ap_item = cJSON_CreateObject();
        cJSON_AddStringToObject(ap_item, "ssid", (const char *)ap[i].ssid);
        cJSON_AddNumberToObject(ap_item, "rssi", ap[i].rssi);
        cJSON_AddBoolToObject(ap_item, "encrypted", (ap[i].authmode != WIFI_AUTH_OPEN));
        cJSON_AddItemToArray(wifi_list, ap_item);
    }
    char *json_str = cJSON_PrintUnformatted(root);
    ws_server_send((uint8_t *)json_str, strlen(json_str));
    cJSON_free(json_str);
    cJSON_Delete(root);
}

/**
 * @brief WebSocket 消息接收处理
 */
void ws_receive_handle(const char *data, int len)
{
    cJSON *root = cJSON_Parse(data);
    if (!root) return;

    cJSON *scan_js = cJSON_GetObjectItem(root, "scan");
    if (scan_js && cJSON_IsString(scan_js) && strcmp(scan_js->valuestring, "start") == 0) {
        wifi_manager_scan(wifi_scan_handle);
    }

    cJSON *ssid_js = cJSON_GetObjectItem(root, "ssid");
    cJSON *pwd_js = cJSON_GetObjectItem(root, "password");
    if (ssid_js && pwd_js && cJSON_IsString(ssid_js) && cJSON_IsString(pwd_js)) {
        snprintf(current_ssid, sizeof(current_ssid), "%s", ssid_js->valuestring);
        snprintf(current_password, sizeof(current_password), "%s", pwd_js->valuestring);
        is_configuring = true;
        xEventGroupSetBits(prov_ev_group, PROV_WIFI_CONNECTED_BIT);
    }

    cJSON_Delete(root);
}

void wifi_provision_init(wifi_provision_cb_t callback)
{
    user_callback = callback;
    wifi_manager_init(internal_wifi_cb);
    prov_ev_group = xEventGroupCreate();
    xTaskCreatePinnedToCore(wifi_provision_task, "prov_task", 4096, NULL, 3, NULL, 1);
}

void wifi_provision_start_apcfg(void)
{
    ESP_LOGI(TAG, "启动 AP 配网模式...");
    wifi_manager_ap();

    ws_server_config_t config = {
        .html_code = (const char *)apcfg_html_start, // 直接使用嵌入的 HTML
        .cb = ws_receive_handle,
    };
    ws_server_start(&config);
}
