#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>

#define TAG "MAIN"

#define DEFAULT_WIFI_SSID "testwifi"
#define DEFAULT_WIFI_PASSWORD "12345678"

void wifi_state_handler(WIFI_STATE state)
{
    if (state == WIFI_STATE_CONNECTED)
    {
        ESP_LOGI(TAG, "Wifi connect success!");
    }
    else
    {
        ESP_LOGI(TAG, "Wifi disconnect! ");
    }
}

void app_main(void)
{
    // NVS初始化（WIFI底层驱动有用到NVS，所以这里要初始化）
    nvs_flash_init();
    // wifi STA工作模式初始化
    wifi_manager_init(wifi_state_handler);
    wifi_manager_connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
