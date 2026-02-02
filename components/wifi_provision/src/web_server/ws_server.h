#ifndef _WS_SERVER_H_
#define _WS_SERVER_H_
#include "esp_err.h"
#include <stdint.h>
typedef void (*ws_server_receive_cb)(const char *data, int len);
typedef struct
{
    const char *html_code;
    ws_server_receive_cb cb;
} ws_server_config_t;

esp_err_t ws_server_start(ws_server_config_t *config);

esp_err_t ws_server_stop(void);

esp_err_t ws_server_send(uint8_t *data, int len);
#endif