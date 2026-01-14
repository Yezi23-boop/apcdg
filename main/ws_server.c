#include "ws_server.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "soc/gpio_sig_map.h"

#define TAG "ws_server"
static const char *http_html = NULL;
static ws_server_receive_cb ws_server_cb = NULL;
static httpd_handle_t server_handle = NULL;
esp_err_t get_hyyp_req(httpd_req_t *r)
{
    char buf[1024];
    int ret = httpd_req_recv(r, buf, sizeof(buf));
    if (ret <= 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t ws_server_start(ws_server_config_t *config)
{
    if (config == NULL)
    {
        return ESP_FAIL;
    }
    http_html = config->html_code;
    ws_server_cb = config->cb;
    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server_handle, &httpd_config);
    httpd_uri_t uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_hyyp_req,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server_handle, &uri);
    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = get_hyyp_req,
        .user_ctx = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server_handle, &uri_ws);
    return ESP_OK;
}

esp_err_t ws_server_stop(void)
{
    return ESP_OK;
}

esp_err_t ws_server_send(uint8_t *data, int len)
{
    return ESP_OK;
}