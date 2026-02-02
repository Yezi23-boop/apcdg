#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_
#include "esp_err.h"
#include "esp_wifi.h"

typedef enum
{
    WIFI_STATE_CONNECTED,    // WiFi连接成功并获取IP
    WIFI_STATE_DISCONNECTED, // WiFi断开连接
    WIFI_STATE_CONNECT_FAIL, // WiFi连接失败（密码错误、找不到热点等）
} WIFI_STATE;

// wifi状态变化回调函数
typedef void (*p_wifi_state_callback)(WIFI_STATE state);
typedef void (*p_wifi_scan_callback)(wifi_ap_record_t *ap, int ap_count);
/** 初始化wifi，默认进入STA模式
 * @param f wifi状态变化回调函数
 * @return 无
 */
void wifi_manager_init(p_wifi_state_callback f);

/** 连接wifi
 * @param ssid
 * @param password
 * @return 成功/失败
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
/** 开启AP模式
 * @return 成功/失败
 */
esp_err_t wifi_manager_ap(void);
/** 扫描wifi
 * @return 成功/失败
 */
esp_err_t wifi_manager_scan(p_wifi_scan_callback f);

/** 获取当前STA的IP地址字符串
 * @param ip_str 输出缓冲区（至少16字节）
 * @return ESP_OK成功，ESP_FAIL未连接
 */
esp_err_t wifi_manager_get_ip(char *ip_str);

/** 关闭AP模式，切换到纯STA模式
 * @return ESP_OK成功，ESP_FAIL失败
 * @note 配网成功后调用此函数关闭热点
 */
esp_err_t wifi_manager_stop_ap(void);
#endif
