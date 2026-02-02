#ifndef WIFI_PROVISION_H
#define WIFI_PROVISION_H

#include "esp_err.h"

/**
 * @brief WiFi 状态枚举
 */
typedef enum {
    WIFI_PROVISION_STATE_CONNECTED,    // WiFi连接成功并获取IP
    WIFI_PROVISION_STATE_DISCONNECTED, // WiFi断开连接
    WIFI_PROVISION_STATE_CONNECT_FAIL  // WiFi连接失败
} wifi_provision_state_t;

/**
 * @brief WiFi 状态变化回调函数指针
 */
typedef void (*wifi_provision_cb_t)(wifi_provision_state_t state);

/**
 * @brief 初始化 WiFi 配网组件
 * 
 * @param callback WiFi 状态变化的回调函数
 */
void wifi_provision_init(wifi_provision_cb_t callback);

/**
 * @brief 启动 AP 配网模式
 * 开启 AP 热点并启动 Web 服务器供用户配网
 */
void wifi_provision_start_apcfg(void);

#endif // WIFI_PROVISION_H
