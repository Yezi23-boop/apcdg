## 目标
将 WiFi 配网功能重构为一个高度模块化、易于移植的组件 `wifi_provision`。

## 模块化目录结构设计
为了满足您“方便改写”和“各个模块分开”的要求，我们将 `src` 内部进行细分，将私有逻辑和公共接口分离：

```text
components/wifi_provision/
├── CMakeLists.txt
├── idf_component.yml
├── include/
│   └── wifi_provision.h        // 公共 API，供 main.c 调用
├── src/
│   ├── wifi_driver/            // WiFi 驱动模块 (原 wifi_manager)
│   │   ├── wifi_manager.c
│   │   └── wifi_manager.h      // 内部头文件
│   ├── web_server/             // 网页/WebSocket 服务模块 (原 ws_server)
│   │   ├── ws_server.c
│   │   └── ws_server.h         // 内部头文件
│   └── wifi_provision.c        // 核心配网逻辑 (原 ap_wifi)，负责协调驱动和网页
└── html/
    └── apcfg.html              // 网页资源
```

## 核心重构策略

### 1. 深度模块化
*   **私有化**：将 `wifi_manager.h` 和 `ws_server.h` 放入 `src` 子目录中，作为组件的内部私有头文件。外部（如 `main.c`）只需要包含 `include/wifi_provision.h` 即可。
*   **解耦**：每个模块目录下的代码只负责自己的功能，通过定义的内部接口进行通信，方便您未来单独替换某个模块（例如将 WebSocket 换成纯 HTTP）。

### 2. 资源嵌入 (Embedding)
*   **零配置移植**：利用 `EMBED_TXTFILES` 将 `apcfg.html` 嵌入二进制。
*   **原理说明**：在 `wifi_provision.c` 中通过 `extern const uint8_t apcfg_html_start[]` 直接访问网页数据，彻底摆脱 SPIFFS 分区依赖。

### 3. 组件 CMake 脚本
在 `components/wifi_provision/CMakeLists.txt` 中：
```cmake
idf_component_register(
    SRCS 
        "src/wifi_provision.c"
        "src/wifi_driver/wifi_manager.c"
        "src/web_server/ws_server.c"
    INCLUDE_DIRS "include"                     # 对外公开的接口目录
    PRIV_INCLUDE_DIRS                          # 组件内部私有的包含目录
        "src/wifi_driver" 
        "src/web_server"
    EMBED_TXTFILES "html/apcfg.html"           # 嵌入网页文件
)
```

## 实施步骤
1. **迁移与拆分**：按照上述结构创建目录并移动文件。
2. **重构代码**：
    - 在 `wifi_provision.c` 中删除 SPIFFS 相关代码，改用内存指针访问嵌入的 HTML。
    - 统一 API 前缀（例如全部改为 `wifi_provision_` 开头）。
3. **适配主程序**：修改 `main/main.c`，仅保留对 `wifi_provision.h` 的调用。
4. **编译验证**：确保在不配置 SPIFFS 分区的情况下，配网功能依然正常。

这个结构是否符合您的模块化要求？如果您确认，我将开始为您生成代码。