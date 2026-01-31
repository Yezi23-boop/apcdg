/**
 * @file ws_server.c
 * @brief WebSocket服务器模块 - 提供HTTP+WebSocket服务
 *
 * 【模块功能】
 * 1. 提供HTTP服务（根路径"/"返回配网HTML页面）
 * 2. 提供WebSocket服务（"/ws"端点用于双向通信）
 * 3. 支持服务器主动推送消息到浏览器
 *
 * 【HTTP与WebSocket的区别】
 * HTTP:      请求-响应模式，客户端发请求，服务器返回响应，然后连接关闭
 * WebSocket: 全双工模式，连接建立后保持，双方可以随时发送消息
 *
 * 【WebSocket连接过程】
 * 1. 浏览器发送HTTP请求，头部包含Upgrade: websocket
 * 2. 服务器返回101 Switching Protocols
 * 3. TCP连接保持，切换到WebSocket协议
 * 4. 后续使用WebSocket帧格式通信
 */

#include "ws_server.h"
#include "esp_err.h"
#include "esp_http_server.h" // ESP-IDF HTTP服务器API
#include "esp_log.h"
#include "soc/gpio_sig_map.h"
#include "string.h"

#define TAG "ws_server"

/*============================================================================
 *                           模块静态变量
 *============================================================================*/

/** 配网HTML页面内容指针（来自SPIFFS） */
static const char *http_html = NULL;

/** WebSocket消息接收回调函数 */
static ws_server_receive_cb ws_server_cb = NULL;

/** HTTP服务器句柄（用于停止服务器） */
static httpd_handle_t server_handle = NULL;

/**
 * WebSocket客户端的socket描述符
 *
 * 【为什么需要保存socket_fd？】
 * WebSocket连接建立后，服务器想主动推送消息时
 * 需要知道发到哪个socket。这个值在WebSocket握手时获取。
 *
 * 【socket描述符】
 * 操作系统分配的整数，代表一个网络连接
 * 类似于文件句柄，用于读写网络数据
 */
static int socket_fd = -1;
/*============================================================================
 *                           HTTP请求处理
 *============================================================================*/

/**
 * @brief HTTP GET"/"请求处理 - 返回配网页面
 *
 * @param r HTTP请求对象，包含请求信息和响应发送接口
 * @return ESP_OK 成功
 *
 * 【调用时机】
 * 当浏览器访问 http://192.168.100.1/ 时调用
 *
 * 【请求咍处理器注册】
 * 需要在启动服务器后使用httpd_register_uri_handler注册
 */
esp_err_t get_hyyp_req(httpd_req_t *r)
{
    /**
     * httpd_resp_send: 发送HTTP响应
     * @param r         请求对象
     * @param http_html 响应内容（HTML字符串）
     * @param HTTPD_RESP_USE_STRLEN 自动计算字符串长度
     */
    return httpd_resp_send(r, http_html, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief 处理favicon.ico请求 - 返回204无内容
 *
 * 浏览器会自动请求/favicon.ico作为网站图标
 * 返回204 No Content告诉浏览器没有图标，避免404警告
 */
static esp_err_t favicon_handler(httpd_req_t *r)
{
    // 返回204 No Content，浏览器会停止请求图标
    httpd_resp_set_status(r, "204 No Content");
    return httpd_resp_send(r, NULL, 0);
}

/*============================================================================
 *                           WebSocket请求处理
 *============================================================================*/

/**
 * @brief WebSocket"/ws"请求处理 - 处理握手和数据接收
 *
 * @param r HTTP请求对象
 * @return ESP_OK 成功，ESP_ERR_xxx 失败
 *
 * 【函数工作流程】
 * 首次调用（HTTP GET请求，即WebSocket握手）：
 *   - 保存socket_fd以便后续主动推送
 *   - 直接返回ESP_OK，服务器自动完成WebSocket握手
 *
 * 后续调用（收到WebSocket数据帧）：
 *   - 第一次httpd_ws_recv_frame: 只获取帧长度（len=0）
 *   - 分配内存缓冲区
 *   - 第二次httpd_ws_recv_frame: 实际读取数据
 *   - 调用上层回调处理数据
 */
esp_err_t handle_ws_req(httpd_req_t *r)
{
    /**
     * 【WebSocket握手检测】
     * 第一次WebSocket请求是HTTP GET（协议升级请求）
     * r->method == HTTP_GET 表示这是握手请求
     * 此时我们保存socket描述符，以便后续主动发送消息
     */
    if (r->method == HTTP_GET)
    {
        // httpd_req_to_sockfd: 从请求对象获取socket描述符
        socket_fd = httpd_req_to_sockfd(r);
        ESP_LOGI(TAG, "WebSocket连接建立, socket_fd=%d", socket_fd);
        return ESP_OK; // 服务器自动完成握手
    }

    /*------------------------------------------------------------------------
     * 接收WebSocket数据帧 - 两阶段读取模式
     *------------------------------------------------------------------------*/

    httpd_ws_frame_t ws_pkt; // WebSocket帧结构体
    esp_err_t err;

    // 清零结构体（很重要，特别是len字段）
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    /**
     * 【第一阶段】获取帧长度
     * 传入len=0，函数不读取数据，只填充ws_pkt.len告诉我们数据有多长
     * 这样我们才知道要分配多大的缓冲区
     */
    err = httpd_ws_recv_frame(r, &ws_pkt, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "获取帧长度失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WebSocket帧长度: %d字节", ws_pkt.len);

    // 分配缓冲区（+1用于字符串结束符）
    uint8_t *buf = malloc(ws_pkt.len + 1);
    if (buf == NULL)
    {
        ESP_LOGE(TAG, "内存分配失败");
        return ESP_ERR_NO_MEM;
    }

    // 将缓冲区挂载到帧结构体
    ws_pkt.payload = buf;

    /**
     * 【第二阶段】实际读取数据
     * 现在ws_pkt.payload已指向我们的缓冲区
     * 传入ws_pkt.len告诉函数最多读取多少字节
     */
    err = httpd_ws_recv_frame(r, &ws_pkt, ws_pkt.len);
    if (err == ESP_OK)
    {
        // 检查帧类型是否为文本（本项目只处理JSON文本）
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
        {
            buf[ws_pkt.len] = 0; // 添加字符串结束符
            ESP_LOGI(TAG, "WebSocket收到: %s", ws_pkt.payload);

            // 调用上层回调处理消息
            if (ws_server_cb)
            {
                ws_server_cb((const char *)buf, ws_pkt.len);
            }
        }
        else
        {
            ESP_LOGW(TAG, "收到非Text类型的WebSocket帧: %d", ws_pkt.type);
        }
    }
    else
    {
        ESP_LOGE(TAG, "读取帧数据失败: %s", esp_err_to_name(err));
    }

    // 释放缓冲区
    free(buf);
    return ESP_OK;
}

/*============================================================================
 *                           服务器控制函数
 *============================================================================*/

/**
 * @brief 启动HTTP+WebSocket服务器
 *
 * @param config 服务器配置（HTML内容和消息回调）
 * @return ESP_OK 成功，ESP_FAIL 参数错误
 *
 * 【注册的路由】
 * GET "/"   -> get_hyyp_req   -> 返回HTML网页
 * GET "/ws" -> handle_ws_req  -> WebSocket通信
 */
esp_err_t ws_server_start(ws_server_config_t *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "服务器配置为NULL");
        return ESP_FAIL;
    }

    // 保存配置到模块变量
    http_html = config->html_code;
    ws_server_cb = config->cb;

    /**
     * 【创建HTTP服务器】
     * HTTPD_DEFAULT_CONFIG() 提供默认配置：
     * - 端口: 80
     * - 最大连接数: 7
     * - 堆栈大小: 4096
     * - 核心: 0 (不绑定特定核心)
     */
    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();

    /**
     * 【增加缓冲区大小】
     * 浏览器发送的HTTP请求头可能很长（包含Cookie、User-Agent等）
     * 默认512字节不够用，增加到1024字节避免"431 Request Header Fields Too Large"错误
     */
    httpd_config.uri_match_fn = httpd_uri_match_wildcard; // 支持通配符匹配
    httpd_config.max_uri_handlers = 8;                    // 最大URI处理器数量
    httpd_config.stack_size = 8192;                       // 增加栈大小以处理WebSocket

    esp_err_t ret = httpd_start(&server_handle, &httpd_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动HTTP服务器失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "HTTP服务器启动成功，端口: %d", httpd_config.server_port);

    /**
     * 【注册HTTP路由】
     * httpd_uri_t结构体定义一个URL处理规则：
     * - uri: URL路径
     * - method: HTTP方法 (GET/POST/PUT/DELETE等)
     * - handler: 处理函数
     * - user_ctx: 用户上下文（传给handler）
     * - is_websocket: 是否WebSocket端点
     */

    // 注册根路径 - 返回HTML页面
    httpd_uri_t uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_hyyp_req,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server_handle, &uri);
    ESP_LOGI(TAG, "注册路由: GET /");

    // 注册WebSocket端点
    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .user_ctx = NULL,
        .is_websocket = true, // 启用WebSocket处理
    };
    httpd_register_uri_handler(server_handle, &uri_ws);
    ESP_LOGI(TAG, "注册路由: GET /ws (WebSocket)");

    // 注册favicon.ico处理 - 避免404警告
    httpd_uri_t uri_favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server_handle, &uri_favicon);
    ESP_LOGI(TAG, "注册路由: GET /ws (WebSocket)");

    return ESP_OK;
}

/**
 * @brief 停止HTTP服务器
 *
 * 【调用时机】
 * WiFi配置完成后，不再需要配网界面时调用
 *
 * @return ESP_OK 成功
 */
esp_err_t ws_server_stop(void)
{
    if (server_handle)
    {
        ESP_LOGI(TAG, "停止HTTP服务器");
        httpd_stop(server_handle);
        server_handle = NULL;
        socket_fd = -1; // 重置socket描述符
    }
    return ESP_OK;
}

/**
 * @brief 通过WebSocket主动发送数据到浏览器
 *
 * @param data 要发送的数据
 * @param len  数据长度
 * @return ESP_OK 成功，ESP_ERR_xxx 失败
 *
 * 【使用场景】
 * - WiFi扫描完成后，主动推送结果到网页
 * - 设备状态变化时，实时通知网页
 *
 * 【注意】
 * 必须在WebSocket连接建立后才能调用（socket_fd有效）
 */
esp_err_t ws_server_send(uint8_t *data, int len)
{
    if (socket_fd < 0)
    {
        ESP_LOGE(TAG, "WebSocket未连接，无法发送");
        return ESP_FAIL;
    }

    // 构建WebSocket帧
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = data;            // 数据内容
    ws_pkt.len = len;                 // 数据长度
    ws_pkt.type = HTTPD_WS_TYPE_TEXT; // 帧类型：文本

    /**
     * httpd_ws_send_data: 发送WebSocket帧
     * @param server_handle HTTP服务器句柄
     * @param socket_fd     目标客户端的socket描述符
     * @param ws_pkt        要发送的WebSocket帧
     */
    esp_err_t ret = httpd_ws_send_data(server_handle, socket_fd, &ws_pkt);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WebSocket发送失败: %s", esp_err_to_name(ret));
    }
    return ret;
}