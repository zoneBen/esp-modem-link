# API 设计规范

> 版本：v0.1 草案 | 日期：2026-09-12

本文档定义多模组 AT 蜂窝网络库的 API 设计规范，确保接口一致性和可用性。

## 一、设计原则

### 1.1 命名约定

| 元素 | 风格 | 示例 |
|---|---|---|
| 类名 | PascalCase | `TcpClient`, `MqttClient` |
| 接口名 | I 前缀 + PascalCase | `IModuleHal`, `IAtChannel` |
| 枚举类 | PascalCase | `NetworkErrc`, `MqttQoS` |
| 枚举值 | PascalCase | `Timeout`, `QoS0` |
| 方法名 | PascalCase | `Connect()`, `SendData()` |
| 成员变量 | 后缀下划线 | `connected_`, `on_data_` |
| 常量 | k 前缀 + PascalCase | `kMaxConnections`, `kDefaultTimeout` |
| 回调类型 | Callback 后缀 | `DataCallback`, `ErrorCallback` |
| 类型别名 | 以类型语义命名 | `Result<T>`, `UrcHandle` |

### 1.2 方法设计准则

1. **返回值优先于输出参数**
   ```cpp
   // 好
   Result<int> Read(void* buf, size_t size);

   // 不好
   bool Read(void* buf, size_t size, int* out_bytes_read);
   ```

2. **失败通过 `Result<>` 表达，不用异常**
   ```cpp
   // 好
   Result<> Connect(std::string_view host, uint16_t port);

   // 不好
   void Connect(std::string_view host, uint16_t port);  // 失败抛异常
   ```

3. **字符串参数使用 `std::string_view`**（除非需要存储拷贝）

4. **布尔参数用枚举替代**（当有两个以上状态或语义不明显时）
   ```cpp
   // 好
   enum class TlsMode { Disabled, Enabled };
   Result<> Connect(host, port, TlsMode::Enabled);

   // 不好
   Result<> Connect(host, port, true);  // true 是什么意思？
   ```

### 1.3 回调设计准则

1. **回调注册方法以 `On` 开头**
   ```cpp
   void OnData(DataCallback callback);
   void OnError(ErrorCallback callback);
   ```

2. **每个回调独立 setter，不使用事件总线**

3. **回调参数按"主题 + 详情"顺序排列**
   ```cpp
   // MQTT 消息：topic 是主题，payload 是详情
   using MessageCallback = std::function<void(std::string_view topic,
                                              std::string_view payload)>;

   // TCP 数据：数据是主题，长度是详情（通过 string_view 一体表达）
   using DataCallback = std::function<void(std::string_view data)>;
   ```

4. **错误回调统一使用 `const NetworkError&`**
   ```cpp
   using ErrorCallback = std::function<void(const NetworkError& error)>;
   ```

### 1.4 资源生命周期

1. **客户端对象用 `std::unique_ptr` 管理**
   - 创建：`Result<std::unique_ptr<TcpClient>> CreateTcp()`
   - 销毁：对象析构时自动断开连接、注销回调

2. **共享资源用 `std::shared_ptr` 管理**
   - `IAtChannel` 在多个客户端之间共享
   - 引用计数确保最后一个使用者销毁时才释放

3. **RAII 管理临时状态**
   - `DtrGuard`：管理模组唤醒/休眠
   - `CommandLock`：管理 AT 命令互斥锁

## 二、通用类型定义

### 2.1 错误类型

```cpp
// 网络错误
enum class NetworkErrc { /* 见架构设计文档 */ };

class NetworkError {
public:
    NetworkErrc code;
    int native;
    std::string context;

    const char* Name() const;
    const char* Message() const;
    std::string ToString() const;

    // 便捷构造
    static NetworkError Timeout(std::string_view ctx = "");
    static NetworkError DnsFailed(int herr, std::string_view host = "");
    // ...
};

// 结果类型
template <typename T = void>
using Result = std::expected<T, NetworkError>;

// AT 层错误
enum class AtErrc { Timeout, CommandError, CmeError, CmsError,
                    TransmitFailed, NotInitialized };

struct AtError {
    AtErrc code;
    int cme = 0;
    int cms = 0;
    esp_err_t esp = ESP_OK;
    std::string context;

    NetworkError ToNetworkError() const;
    std::string ToString() const;
};

using AtResult = std::expected<void, AtError>;

template <typename T>
using AtValue = std::expected<T, AtError>;
```

### 2.2 回调类型

```cpp
using DataCallback = std::function<void(std::string_view data)>;
using ErrorCallback = std::function<void(const NetworkError& error)>;
using EventCallback = std::function<void()>;
using CloseCallback = std::function<void(int code, std::string_view reason)>;
```

### 2.3 配置结构体

每个模块的配置使用独立的结构体，所有字段有合理默认值。

```cpp
struct TlsConfig {
    bool verify_certificate = true;
    bool verify_hostname = true;
    std::string ca_cert;            // PEM 格式 CA 证书
    std::string client_cert;        // PEM 格式客户端证书
    std::string client_key;         // PEM 格式客户端私钥
    std::string alpn_protocols;     // 逗号分隔，如 "h2,http/1.1"
    std::chrono::seconds handshake_timeout{10};
};

struct MqttWill {
    std::string topic;
    std::string payload;
    MqttQoS qos = MqttQoS::QoS0;
    bool retain = false;
};
```

## 三、核心接口设计

### 3.1 CellularDevice（设备级）

```cpp
class CellularDevice {
public:
    // 静态工厂：自动检测
    static Result<std::unique_ptr<CellularDevice>> Detect(
        gpio_num_t tx, gpio_num_t rx,
        gpio_num_t dtr = GPIO_NUM_NC,
        gpio_num_t ri = GPIO_NUM_NC,
        int initial_baud = 115200);

    // 静态工厂：指定模组类型
    static Result<std::unique_ptr<CellularDevice>> Create(
        ModuleType type, const DeviceConfig& config);

    // 网络管理
    NetworkStatus WaitForNetwork(std::chrono::milliseconds timeout);
    void OnNetworkState(std::function<void(bool ready)> callback);

    // 设备控制
    Result<> Reboot();
    Result<> SetFlightMode(bool enable);
    Result<> SetSleepMode(bool enable, const SleepConfig& config);

    // 设备信息
    Result<std::string> GetImei();
    Result<std::string> GetIccid();
    Result<std::string> GetModuleRevision();
    Result<std::string> GetCarrierName();
    Result<int> GetSignalStrength();          // 0-31, 99=未知
    Result<RegistrationState> GetRegistrationState();

    // 能力查询
    const ModuleCapabilities& GetCapabilities() const;

    // 网络接口
    NetworkInterface& GetNetwork();

    // AT 通道（高级用法）
    IAtChannel& GetAtChannel();

    // 不可拷贝
    CellularDevice(const CellularDevice&) = delete;
    CellularDevice& operator=(const CellularDevice&) = delete;
};
```

**设计说明**：
- 所有查询方法返回 `Result<T>` 而非 `T + GetLastError()`
- `GetCapabilities()` 是 const 的，能力在初始化时确定，运行时不变
- 设备是不可拷贝的（语义上只有一个物理设备）

### 3.2 NetworkInterface（网络工厂）

```cpp
class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;

    // 传输层
    virtual Result<std::unique_ptr<TcpClient>> CreateTcp() = 0;
    virtual Result<std::unique_ptr<TcpClient>> CreateSsl(const TlsConfig& config = {}) = 0;
    virtual Result<std::unique_ptr<UdpClient>> CreateUdp() = 0;

    // 应用层
    virtual Result<std::unique_ptr<HttpClient>> CreateHttp() = 0;
    virtual Result<std::unique_ptr<MqttClient>> CreateMqtt() = 0;
    virtual Result<std::unique_ptr<WebSocketClient>> CreateWebSocket() = 0;

    // 能力查询
    virtual bool Supports(NetworkProtocol proto) const = 0;
    virtual int MaxConnections(NetworkProtocol proto) const = 0;
};
```

**设计说明**：
- `CreateSsl()` 接受 `TlsConfig` 参数，而不是单独的 `CreateSsl()` 加 `SetTlsConfig()`
- `CreateXxx()` 失败时返回错误（不支持、连接数已满等），而非返回 nullptr

### 3.3 TcpClient

```cpp
class TcpClient {
public:
    virtual ~TcpClient() = default;

    virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
    virtual void Disconnect() = 0;

    // 发送
    virtual Result<int> Send(const void* data, size_t len) = 0;
    Result<int> Send(std::string_view data) {
        return Send(data.data(), data.size());
    }

    // 回调
    void OnData(DataCallback callback) { on_data_ = std::move(callback); }
    void OnDisconnected(EventCallback callback) { on_disconnected_ = std::move(callback); }
    void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }

    // 状态
    bool IsConnected() const { return connected_; }
    virtual size_t GetSendBufferFree() const { return SIZE_MAX; }

protected:
    DataCallback on_data_;
    EventCallback on_disconnected_;
    ErrorCallback on_error_;
    bool connected_ = false;
};
```

**设计说明**：
- `Send()` 有两个重载：指针+长度（底层） 和 string_view（便捷）
- `IsConnected()` 和 `GetSendBufferFree()` 是非虚的有默认实现，子类可覆盖
- 回调 setter 是非虚的（Template Method 模式），子类只触发回调

### 3.4 UdpClient

```cpp
class UdpClient {
public:
    virtual ~UdpClient() = default;

    // 连接模式（固定远端地址）
    virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
    virtual void Disconnect() = 0;

    // 发送（连接模式）
    virtual Result<int> Send(const void* data, size_t len) = 0;
    Result<int> Send(std::string_view data) { return Send(data.data(), data.size()); }

    // 无连接模式：向指定地址发送
    virtual Result<int> SendTo(const void* data, size_t len,
                               std::string_view host, uint16_t port) = 0;

    // 接收回调（携带源地址）
    struct ReceivedPacket {
        std::string_view data;
        std::string remote_host;
        uint16_t remote_port;
    };
    using PacketCallback = std::function<void(const ReceivedPacket& pkt)>;
    void OnPacket(PacketCallback callback) { on_packet_ = std::move(callback); }

    // 错误
    void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }

    bool IsConnected() const { return connected_; }

protected:
    PacketCallback on_packet_;
    ErrorCallback on_error_;
    bool connected_ = false;
};
```

**设计说明**：
- 区分"连接模式"和"无连接模式"，两者可共存
- 接收回调携带源地址信息（原库缺失）
- `ReceivedPacket` 结构体组织相关数据

### 3.5 HttpClient

```cpp
class HttpClient {
public:
    virtual ~HttpClient() = default;

    // 配置（请求前设置）
    virtual void SetTimeout(std::chrono::milliseconds timeout) = 0;
    virtual void SetHeader(std::string_view key, std::string_view value) = 0;
    virtual void SetBody(std::string body) = 0;
    virtual void SetKeepAlive(bool enable) = 0;
    virtual void SetTlsConfig(const TlsConfig& config) = 0;
    virtual void SetFollowRedirects(bool enable) = 0;
    virtual void SetMaxRedirects(int max) = 0;

    // 简单请求（一步完成，适合小响应）
    struct SimpleResponse {
        int status_code;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    virtual Result<SimpleResponse> Execute(std::string_view method,
                                            std::string_view url) = 0;

    // 流式请求（适合大文件/OTA）
    virtual Result<> Open(std::string_view method, std::string_view url) = 0;
    virtual Result<int> Read(void* buffer, size_t size) = 0;
    virtual Result<int> Write(const void* buffer, size_t size) = 0;
    virtual void Close() = 0;

    // 流式模式下的响应信息
    virtual Result<int> GetStatusCode() = 0;
    virtual std::string GetResponseHeader(std::string_view key) const = 0;
    virtual size_t GetContentLength() const = 0;
    virtual bool IsChunked() const = 0;
};
```

**设计说明**：
- 提供两种使用模式：`Execute()` 简单请求 + `Open/Read/Write` 流式请求
- `SimpleResponse` 结构体封装完整响应
- 支持重定向跟随（原库缺失）
- 响应头查询不返回错误（没找到就是空字符串）

### 3.6 MqttClient

```cpp
enum class MqttQoS { QoS0 = 0, QoS1 = 1, QoS2 = 2 };

struct MqttMessage {
    std::string topic;
    std::string payload;
    MqttQoS qos = MqttQoS::QoS0;
    bool retain = false;
    int message_id = 0;
};

class MqttClient {
public:
    virtual ~MqttClient() = default;

    // 配置
    virtual void SetClientId(std::string_view client_id) = 0;
    virtual void SetCredentials(std::string_view user,
                                std::string_view pass) = 0;
    virtual void SetKeepAlive(int seconds) = 0;
    virtual void SetCleanSession(bool clean) = 0;
    virtual void SetWill(const MqttWill& will) = 0;
    virtual void SetTlsConfig(const TlsConfig& config) = 0;

    // 连接
    virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
    virtual void Disconnect() = 0;

    // 发布
    virtual Result<int> Publish(std::string_view topic,
                                std::string_view payload,
                                MqttQoS qos = MqttQoS::QoS0,
                                bool retain = false) = 0;

    // 订阅
    virtual Result<> Subscribe(std::string_view topic,
                               MqttQoS qos = MqttQoS::QoS0) = 0;
    virtual Result<> Unsubscribe(std::string_view topic) = 0;

    // 回调
    void OnConnected(EventCallback callback) { on_connected_ = std::move(callback); }
    void OnDisconnected(EventCallback callback) { on_disconnected_ = std::move(callback); }
    void OnMessage(std::function<void(const MqttMessage& msg)> callback) {
        on_message_ = std::move(callback);
    }
    void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
    void OnPublishAck(std::function<void(int message_id)> callback) {
        on_publish_ack_ = std::move(callback);
    }

    bool IsConnected() const { return connected_; }

protected:
    EventCallback on_connected_;
    EventCallback on_disconnected_;
    std::function<void(const MqttMessage&)> on_message_;
    ErrorCallback on_error_;
    std::function<void(int)> on_publish_ack_;
    bool connected_ = false;
};
```

**设计说明**：
- `Publish()` 返回消息 ID（QoS 1/2 时用于确认追踪）
- `OnMessage` 传递 `MqttMessage` 结构体，包含 topic/payload/qos/retain/msg_id
- 新增 `OnPublishAck` 回调（QoS 1/2 发布确认）
- 配置方法在 Connect 前调用，不返回错误（参数错误在 Connect 时报）

### 3.7 WebSocketClient

```cpp
enum class WsCloseCode {
    Normal = 1000,
    GoingAway = 1001,
    ProtocolError = 1002,
    UnsupportedData = 1003,
    NoStatus = 1005,
    Abnormal = 1006,
    InvalidPayload = 1007,
    PolicyViolation = 1008,
    MessageTooBig = 1009,
    InternalError = 1011,
};

class WebSocketClient {
public:
    virtual ~WebSocketClient() = default;

    // 配置
    virtual void SetHeader(std::string_view key, std::string_view value) = 0;
    virtual void SetTlsConfig(const TlsConfig& config) = 0;

    // 心跳
    struct HeartbeatConfig {
        std::chrono::seconds interval{30};
        std::chrono::seconds timeout{10};
        bool enabled = false;
    };
    virtual void SetHeartbeat(const HeartbeatConfig& config) = 0;

    // 自动重连
    struct ReconnectConfig {
        bool enabled = false;
        int max_retries = -1;  // -1 = 无限
        std::chrono::milliseconds initial_delay{1000};
        float backoff_factor = 2.0f;
        std::chrono::milliseconds max_delay{30000};
    };
    virtual void SetAutoReconnect(const ReconnectConfig& config) = 0;

    // 连接
    virtual Result<> Connect(std::string_view url) = 0;
    virtual void Close(WsCloseCode code = WsCloseCode::Normal,
                       std::string_view reason = "") = 0;

    // 发送
    virtual Result<> Send(std::string_view data, bool binary = false) = 0;
    virtual void Ping(std::string_view payload = "") = 0;

    // 回调
    void OnConnected(EventCallback callback) { on_connected_ = std::move(callback); }
    void OnDisconnected(std::function<void(WsCloseCode code,
                                           std::string_view reason)> callback) {
        on_disconnected_ = std::move(callback);
    }
    void OnMessage(std::function<void(std::string_view data,
                                      bool binary)> callback) {
        on_message_ = std::move(callback);
    }
    void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
    void OnPong(DataCallback callback) { on_pong_ = std::move(callback); }

    bool IsConnected() const { return connected_; }

protected:
    EventCallback on_connected_;
    std::function<void(WsCloseCode, std::string_view)> on_disconnected_;
    std::function<void(std::string_view, bool)> on_message_;
    ErrorCallback on_error_;
    DataCallback on_pong_;
    bool connected_ = false;
};
```

**设计说明**：
- 内置心跳和自动重连（原库缺失，用户需自己实现）
- 关闭码使用枚举类（`WsCloseCode`），提高可读性
- `OnDisconnected` 携带关闭码和原因

## 四、AT 通道接口

### 4.1 IAtChannel

```cpp
class IAtChannel {
public:
    virtual ~IAtChannel() = default;

    // 同步命令
    virtual AtResult SendCommand(std::string_view cmd,
                                 std::chrono::milliseconds timeout) = 0;

    virtual AtResult SendCommandWithData(
        std::string_view cmd_prefix,
        const void* data, size_t len,
        std::chrono::milliseconds timeout,
        std::string_view cmd_suffix = "") = 0;

    // 响应访问
    virtual std::string_view GetResponse() const = 0;
    virtual std::vector<std::string_view> GetResponseLines() const = 0;

    // URC 订阅
    using UrcHandler = std::function<void(std::string_view command,
                                          std::string_view arguments)>;
    using UrcHandle = uint64_t;

    virtual UrcHandle SubscribeUrc(std::string_view prefix,
                                   UrcHandler handler) = 0;
    virtual void UnsubscribeUrc(UrcHandle handle) = 0;

    // 透传模式
    virtual AtResult EnterDataMode(int connect_id,
                                   std::chrono::milliseconds timeout) = 0;
    virtual AtResult ExitDataMode(std::chrono::milliseconds timeout) = 0;
    virtual Result<int> SendRaw(const void* data, size_t len) = 0;

    // 波特率
    virtual int GetBaudRate() const = 0;
    virtual AtResult SetBaudRate(int baud) = 0;

    // 调试
    virtual void SetDebugLog(bool enable) = 0;
    virtual bool IsDebugLogEnabled() const = 0;
};
```

**设计说明**：
- `UrcHandle` 是不透明句柄（uint64_t），隐藏实现细节
- `SubscribeUrc` 使用前缀匹配（如 `"+CEREG"` 匹配所有 `+CEREG:` 开头的 URC）
- `EnterDataMode` / `ExitDataMode` 支持透传模式（部分模组支持）

### 4.2 响应解析辅助

```cpp
namespace at_parser {

// 分割 CSV 行（处理引号）
std::vector<std::string_view> SplitCsv(std::string_view line);

// 去除引号
std::string_view StripQuotes(std::string_view s);

// 解析数字
std::optional<int> ParseInt(std::string_view s);
std::optional<double> ParseDouble(std::string_view s);

// 解析键值对：+KEY: value
bool ParseKeyValue(std::string_view line,
                   std::string_view& key,
                   std::string_view& value);

// 解析十六进制
std::vector<uint8_t> ParseHex(std::string_view hex);

// 解析整型列表：1,2,3,4
std::vector<int> ParseIntList(std::string_view line);

}  // namespace at_parser
```

## 五、模块 HAL 接口

```cpp
class IModuleHal {
public:
    virtual ~IModuleHal() = default;

    // === 基本信息 ===
    virtual ModuleType GetType() const = 0;
    virtual std::string_view GetName() const = 0;
    virtual const ModuleCapabilities& GetCapabilities() const = 0;

    // === 生命周期 ===
    virtual Result<> Initialize(IAtChannel& channel) = 0;
    virtual Result<> Reset(HardResetFunc reset_fn = nullptr) = 0;

    // === SIM 与网络注册 ===
    virtual Result<SimState> GetSimState() = 0;
    virtual Result<RegistrationState> GetRegistrationState() = 0;
    virtual Result<SignalInfo> GetSignalInfo() = 0;
    virtual Result<> SetApn(const ApnConfig& config) = 0;

    // === 设备信息 ===
    virtual Result<std::string> GetImei() = 0;
    virtual Result<std::string> GetIccid() = 0;
    virtual Result<std::string> GetFirmwareVersion() = 0;
    virtual Result<std::string> GetCarrierName() = 0;

    // === 低功耗 ===
    virtual bool SupportsLowPower() const { return false; }
    virtual Result<> EnterSleep(const SleepConfig& config) {
        return MakeError(NetworkErrc::NotSupported);
    }
    virtual Result<> ExitSleep() {
        return MakeError(NetworkErrc::NotSupported);
    }
    virtual void OnDtrStateChanged(bool asserted) {}

    // === 连接管理（基础传输） ===
    virtual Result<int> TcpConnect(const TcpConnectParams& params) = 0;
    virtual Result<> TcpClose(int connect_id) = 0;
    virtual Result<int> TcpSend(int connect_id, const void* data,
                                size_t len) = 0;

    virtual Result<int> UdpOpen(const UdpOpenParams& params) = 0;
    virtual Result<> UdpClose(int connect_id) = 0;
    virtual Result<int> UdpSend(int connect_id, const void* data,
                                size_t len) = 0;
    virtual Result<int> UdpSendTo(int connect_id, const void* data,
                                  size_t len, std::string_view host,
                                  uint16_t port) = 0;

    // === 可选：内置协议加速 ===
    virtual bool HasBuiltinHttp() const { return false; }
    virtual Result<std::unique_ptr<HttpClient>> CreateBuiltinHttp() {
        return MakeError(NetworkErrc::NotSupported);
    }

    virtual bool HasBuiltinMqtt() const { return false; }
    virtual Result<std::unique_ptr<MqttClient>> CreateBuiltinMqtt() {
        return MakeError(NetworkErrc::NotSupported);
    }

    // === URC 路由 ===
    // 模组 HAL 注册自己的 URC 前缀，由 IAtChannel 分发
    virtual void RegisterUrcHandlers(IAtChannel& channel) = 0;

protected:
    template <typename T = void>
    Result<T> MakeError(NetworkErrc code, std::string_view ctx = "") {
        return std::unexpected(NetworkError{code, 0, std::string(ctx)});
    }
};
```

**设计说明**：
- HAL 是"模组驱动"，提供模组的原始能力
- 可选功能有默认实现（返回 NotSupported 错误），新增模组时只需实现支持的功能
- `RegisterUrcHandlers` 让 HAL 自己注册关心的 URC，实现解耦

## 六、使用示例

### 6.1 基础使用

```cpp
#include "cellular_device.h"

static const char* TAG = "APP";

extern "C" void app_main() {
    auto device_result = CellularDevice::Detect(
        GPIO_NUM_13, GPIO_NUM_14,  // TX, RX
        GPIO_NUM_15,               // DTR
        GPIO_NUM_NC,               // RI
        115200                     // 初始波特率
    );
    if (!device_result) {
        ESP_LOGE(TAG, "Device detection failed: %s",
                 device_result.error().ToString().c_str());
        return;
    }
    auto device = std::move(*device_result);

    device->OnNetworkState([](bool ready) {
        ESP_LOGI(TAG, "Network: %s", ready ? "ready" : "down");
    });

    auto status = device->WaitForNetwork(std::chrono::seconds{30});
    if (status != NetworkStatus::Ready) {
        ESP_LOGE(TAG, "Network not ready: %d", static_cast<int>(status));
        return;
    }

    ESP_LOGI(TAG, "IMEI: %s", device->GetImei().value_or("unknown").c_str());
    ESP_LOGI(TAG, "CSQ: %d", device->GetSignalStrength().value_or(-1));
}
```

### 6.2 HTTP 请求

```cpp
void http_example(NetworkInterface& net) {
    auto http_result = net.CreateHttp();
    if (!http_result) {
        ESP_LOGE(TAG, "Create HTTP failed: %s",
                 http_result.error().ToString().c_str());
        return;
    }
    auto http = std::move(*http_result);

    http->SetTimeout(std::chrono::seconds{10});
    http->SetFollowRedirects(true);

    auto response = http->Execute("GET", "https://httpbin.org/get");
    if (!response) {
        ESP_LOGE(TAG, "HTTP failed: %s", response.error().ToString().c_str());
        return;
    }

    ESP_LOGI(TAG, "Status: %d, Body: %zu bytes",
             response->status_code, response->body.size());
    ESP_LOGI(TAG, "Body: %.*s", (int)response->body.size(),
             response->body.data());
}
```

### 6.3 MQTT 发布订阅

```cpp
void mqtt_example(NetworkInterface& net) {
    auto mqtt_result = net.CreateMqtt();
    if (!mqtt_result) return;
    auto mqtt = std::move(*mqtt_result);

    mqtt->SetClientId("esp32-test");
    mqtt->SetKeepAlive(60);
    mqtt->SetWill({"status/offline", "device-offline", MqttQoS::QoS1, false});

    mqtt->OnConnected([]() { ESP_LOGI(TAG, "MQTT connected"); });
    mqtt->OnDisconnected([]() { ESP_LOGI(TAG, "MQTT disconnected"); });
    mqtt->OnMessage([](const MqttMessage& msg) {
        ESP_LOGI(TAG, "MQTT [%s] qos=%d: %.*s",
                 msg.topic.c_str(),
                 static_cast<int>(msg.qos),
                 (int)msg.payload.size(), msg.payload.data());
    });
    mqtt->OnError([](const NetworkError& err) {
        ESP_LOGE(TAG, "MQTT error: %s", err.ToString().c_str());
    });

    auto connected = mqtt->Connect("broker.emqx.io", 1883);
    if (!connected) {
        ESP_LOGE(TAG, "Connect failed: %s", connected.error().ToString().c_str());
        return;
    }

    mqtt->Subscribe("test/esp32/in", MqttQoS::QoS1);
    mqtt->Publish("test/esp32/out", "hello", MqttQoS::QoS0);
}
```

### 6.4 WebSocket

```cpp
void ws_example(NetworkInterface& net) {
    auto ws_result = net.CreateWebSocket();
    if (!ws_result) return;
    auto ws = std::move(*ws_result);

    ws->SetHeader("X-Client", "esp32");
    ws->SetHeartbeat({.interval = 30s, .timeout = 10s, .enabled = true});
    ws->SetAutoReconnect({.enabled = true, .max_retries = 5});

    ws->OnConnected([]() { ESP_LOGI(TAG, "WS connected"); });
    ws->OnMessage([](std::string_view data, bool binary) {
        ESP_LOGI(TAG, "WS recv: %.*s", (int)data.size(), data.data());
    });
    ws->OnDisconnected([](WsCloseCode code, std::string_view reason) {
        ESP_LOGI(TAG, "WS closed: %d, %.*s",
                 static_cast<int>(code),
                 (int)reason.size(), reason.data());
    });
    ws->OnError([](const NetworkError& err) {
        ESP_LOGE(TAG, "WS error: %s", err.ToString().c_str());
    });

    ws->Connect("wss://echo.websocket.org/");
    ws->Send("hello world");
}
```

## 七、与原库 API 的对比

| 类别 | 原库 API | 新库 API | 变更说明 |
|---|---|---|---|
| 设备创建 | `AtModem::Detect() -> AtValue<unique_ptr<AtModem>>` | `CellularDevice::Detect() -> Result<unique_ptr<CellularDevice>>` | 类重命名，语义不变 |
| 网络就绪 | `WaitForNetworkReady(30000)` | `WaitForNetwork(30s)` | 使用 std::chrono，方法名简化 |
| TCP 创建 | `CreateTcp(0) -> unique_ptr<Tcp>` | `CreateTcp() -> Result<unique_ptr<TcpClient>>` | 返回值带错误，移除 connect_id 参数 |
| TCP 发送 | `Send(data) -> int` | `Send(data) -> Result<int>` | 失败时返回错误而非 -1 |
| TCP 错误回调 | 无（只有 OnDisconnected） | `OnError(callback)` | 新增 |
| HTTP 简单请求 | 无 | `Execute(method, url) -> Result<SimpleResponse>` | 新增便捷方法 |
| HTTP 重定向 | 不支持 | `SetFollowRedirects(true)` | 新增 |
| MQTT 发布 | `Publish(topic, payload, qos) -> bool` | `Publish(topic, payload, qos, retain) -> Result<int>` | 返回 msg_id，支持 retain |
| MQTT 遗嘱 | 不支持 | `SetWill(will)` | 新增 |
| MQTT QoS 确认 | 不支持 | `OnPublishAck(callback)` | 新增 |
| WS 心跳 | 手动实现 | `SetHeartbeat(config)` | 内置 |
| WS 自动重连 | 手动实现 | `SetAutoReconnect(config)` | 内置 |
| 错误类型 | `NetworkError` | `NetworkError`（扩展） | 新增更多错误码 |
