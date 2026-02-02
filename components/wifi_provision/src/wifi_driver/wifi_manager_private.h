#ifndef WIFI_MANAGER_PRIVATE_H
#define WIFI_MANAGER_PRIVATE_H

#include "esp_netif.h"
#include "wifi_manager.h"

/**
 * @brief 内部配置结构体
 */
typedef struct
{
    char ap_ssid[32];
    char ap_password[64];
    char ap_ip[16];
    int max_retry;
} wifi_manager_config_internal_t;

/**
 * @brief 默认内部配置
 */
#define WIFI_MANAGER_DEFAULT_CONFIG() { \
    .ap_ssid = "ESP32_wifi",            \
    .ap_password = "12345678",          \
    .ap_ip = "192.168.100.1",           \
    .max_retry = 1}

#endif // WIFI_MANAGER_PRIVATE_H
