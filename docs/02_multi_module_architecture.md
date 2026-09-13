# 多模组 AT 蜂窝网络库 — 架构设计

> 版本：v0.1 草案 | 日期：2026-09-12

## 一、设计目标

### 1.1 核心目标

构建一个高度可扩展的 ESP32 蜂窝网络库，支持多种 AT 指令模组（Cat.1、Cat.4、NB-IoT 等），提供统一的网络编程接口。

### 1.2 设计原则

| 原则 | 说明 |
|---|---|
| **模组无关** | 应用代码不感知具体模组型号，只依赖抽象接口 |
| **易扩展** | 新增模组只需实现一个 HAL 适配层，而非重写所有协议 |
| **错误透明** | 所有失败路径都有明确的结构化错误，不静默失败 |
| **现代 C++** | 基于 C++23，充分利用 `std::expected`、智能指针、RAII |
| **高性能** | DMA 串口、零拷贝解析、命令流水线 |
| **可测试** | 平台抽象层支持在 PC 上单元测试 |

### 1.3 支持的模组（目标）

| 厂商 | 型号 | 类型 | 内置协议栈 |
|---|---|---|---|
| 中移物联 | ML307R/A | Cat.1 | TCP/UDP/HTTP/MQTT/SSL |
| 移远 | EC801E/EC600N | Cat.1 | TCP/UDP/MQTT/SSL |
| 移远 | BG95/BG96 | Cat.M1/NB-IoT | TCP/UDP/MQTT/SSL |
| 广和通 | L610 | Cat.1 | TCP/UDP/HTTP/MQTT/SSL |
| 有方 | N58/N75 | Cat.1 | TCP/UDP/HTTP/MQTT/SSL |
| 芯讯通 | A7670C/A7600C | Cat.4 | TCP/UDP/HTTP/MQTT/SSL |
| 乐鑫 | ESP-AT（WiFi） | WiFi | 参考实现 |

## 二、整体架构

### 2.1 架构分层

```
┌──────────────────────────────────────────────────────┐
│                   应用层 (Application)                │
├──────────────────────────────────────────────────────┤
│              Network Interface (抽象工厂)             │
│  ┌──────┐ ┌─────┐ ┌─────┐ ┌──────┐ ┌──────────┐    │
│  │ HTTP │ │ TCP │ │ UDP │ │ MQTT │ │ WebSocket│    │
│  └──────┘ └─────┘ └─────┘ └──────┘ └──────────┘    │
├──────────────────────────────────────────────────────┤
│               Protocol Engine (协议引擎)              │
│  ┌─────────┐ ┌──────────┐ ┌─────────────────────┐   │
│  │ MqttEngine│ │HttpEngine│ │ WebSocketEngine    │   │
│  │ (可选)   │ │ (可选)   │ │ (必选，基于TCP)     │   │
│  └─────────┘ └──────────┘ └─────────────────────┘   │
├──────────────────────────────────────────────────────┤
│               Module HAL (模组适配层)                 │
│  ┌───────────────────────────────────────────────┐   │
│  │  IAtChannel (AT 通道接口)                      │   │
│  │  ├── 命令发送/响应接收                         │   │
│  │  ├── URC 订阅/取消订阅                         │   │
│  │  └── 数据流收发 (透传模式)                     │   │
│  └───────────────────────────────────────────────┘   │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────┐   │
│  │ Ml307Hal │ │ Ec800EHal│ │  Bg95Hal │ │ ...   │   │
│  └──────────┘ └──────────┘ └──────────┘ └───────┘   │
├──────────────────────────────────────────────────────┤
│               AtUart (AT 串口传输层)                  │
│  ┌───────────────────────────────────────────────┐   │
│  │  UART Driver + DMA + 响应解析 + URC 分发      │   │
│  └───────────────────────────────────────────────┘   │
├──────────────────────────────────────────────────────┤
│               Platform Abstraction (平台抽象)         │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────┐   │
│  │  Task/Queue│ │ Event/Flag│ │ Time/Timer│ │ GPIO  │   │
│  └──────────┘ └──────────┘ └──────────┘ └───────┘   │
└──────────────────────────────────────────────────────┘
```

### 2.2 核心设计决策

**1. Module HAL 模式（关键创新）**

不再为每个模组重写 TCP/UDP/MQTT 类，而是定义统一的 `IModuleHal` 接口，将所有模组差异封装在 HAL 层。

协议实现分为两种模式：
- **模组内置模式**：使用模组 AT 指令直接操作协议栈（速度快、省 MCU 资源）
- **软件协议模式**：基于 TCP/UDP 通道，在 MCU 上实现协议（功能完整、与模组无关）

协议引擎自动选择模式：如果模组内置支持则用内置，否则用软件实现。

**2. 能力声明系统**

每个模组 HAL 声明自己的能力矩阵，协议引擎根据能力选择最佳实现路径。

```cpp
struct ModuleCapabilities {
    bool tcp;           // 内置 TCP 客户端
    bool udp;           // 内置 UDP 客户端
    bool ssl_tcp;       // 内置 SSL/TLS 支持
    bool http;          // 内置 HTTP 客户端
    bool https;         // 内置 HTTPS 客户端
    bool mqtt;          // 内置 MQTT 客户端
    bool mqtts;         // 内置 MQTTS 支持
    bool file_system;   // 内置文件系统（用于证书存储等）
    bool low_power;     // 低功耗模式支持
    int max_connections; // 最大并发连接数
    int max_baud_rate;  // 最大波特率
};
```

**3. AT 通道抽象**

`IAtChannel` 接口定义了 AT 通信的原语操作，HAL 层基于此构建。

```cpp
class IAtChannel {
public:
    virtual ~IAtChannel() = default;

    // 同步命令
    virtual AtResult SendCommand(std::string_view cmd,
                                 std::chrono::milliseconds timeout) = 0;
    virtual AtResult SendCommandWithData(std::string_view cmd,
                                         const void* data, size_t len,
                                         std::chrono::milliseconds timeout) = 0;

    // 响应解析辅助
    virtual std::string_view GetResponse() const = 0;
    virtual std::vector<std::string> GetResponseLines() const = 0;

    // URC 订阅
    using UrcHandler = std::function<void(std::string_view cmd,
                                          std::string_view args)>;
    virtual UrcHandle SubscribeUrc(std::string_view prefix,
                                   UrcHandler handler) = 0;
    virtual void UnsubscribeUrc(UrcHandle handle) = 0;

    // 透传模式（用于数据直接传输）
    virtual AtResult EnterDataMode(int connect_id) = 0;
    virtual AtResult ExitDataMode() = 0;
    virtual int SendRaw(const void* data, size_t len) = 0;

    // 流控与状态
    virtual void SetBaudRate(int baud) = 0;
    virtual bool IsReady() const = 0;
};
```

## 三、模块详细设计

### 3.1 CellularDevice（设备管理）

设备的生命周期管理者，负责检测、初始化、网络注册等。

```cpp
class CellularDevice {
public:
    // 自动检测并创建设备
    static Result<std::unique_ptr<CellularDevice>> Detect(
        gpio_num_t tx, gpio_num_t rx,
        gpio_num_t dtr = GPIO_NUM_NC, gpio_num_t ri = GPIO_NUM_NC,
        int initial_baud = 115200);

    // 手动指定模组类型创建
    static Result<std::unique_ptr<CellularDevice>> Create(
        ModuleType type, const DeviceConfig& config);

    // 网络管理
    NetworkStatus WaitForNetwork(std::chrono::milliseconds timeout);
    void OnNetworkState(NetworkStateCallback callback);

    // 设备控制
    void Reboot();
    void SetFlightMode(bool enable);
    Result<> SetSleepMode(bool enable, const SleepConfig& config);

    // 设备信息
    std::string GetImei() const;
    std::string GetIccid() const;
    std::string GetModuleRevision() const;
    std::string GetCarrierName() const;
    int GetSignalStrength() const;   // CSQ 0-31
    RegistrationState GetRegistrationState() const;

    // 能力查询
    const ModuleCapabilities& GetCapabilities() const;

    // 获取网络接口（用于创建连接）
    NetworkInterface& GetNetwork();

    // 直接访问 AT 通道（高级用法）
    IAtChannel& GetAtChannel();
};
```

### 3.2 NetworkInterface（网络抽象工厂）

保留原库的优秀设计，略作扩展。

```cpp
class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;

    // 基础传输层
    virtual Result<std::unique_ptr<TcpClient>> CreateTcp() = 0;
    virtual Result<std::unique_ptr<TcpClient>> CreateSsl() = 0;
    virtual Result<std::unique_ptr<UdpClient>> CreateUdp() = 0;

    // 应用层协议
    virtual Result<std::unique_ptr<HttpClient>> CreateHttp() = 0;
    virtual Result<std::unique_ptr<MqttClient>> CreateMqtt() = 0;
    virtual Result<std::unique_ptr<WebSocketClient>> CreateWebSocket() = 0;

    // 能力查询
    virtual bool HasCapability(NetworkCapability cap) const = 0;
    virtual int GetMaxConnections(NetworkProtocol proto) const = 0;
};
```

**变更点**：
- `CreateXxx()` 返回 `Result<unique_ptr<T>>` 而非直接返回指针
- 失败时明确告知原因（不支持、资源不足等）
- 增加能力查询方法

### 3.3 协议接口设计

#### 3.3.1 TcpClient

```cpp
class TcpClient {
public:
    virtual ~TcpClient() = default;

    virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
    virtual void Disconnect() = 0;
    virtual Result<int> Send(const void* data, size_t len) = 0;

    // 回调
    void OnData(DataCallback callback);
    void OnDisconnected(EventCallback callback);
    void OnError(ErrorCallback callback);

    // 状态
    bool IsConnected() const;
    virtual size_t GetSendBufferFree() const { return SIZE_MAX; }

protected:
    DataCallback on_data_;
    EventCallback on_disconnected_;
    ErrorCallback on_error_;
    bool connected_ = false;
};
```

**改进**：
- `Send()` 返回 `Result<int>` 而非 `int`，失败原因明确
- 新增 `OnError` 回调（原库 TCP 没有错误回调）
- 新增 `GetSendBufferFree()` 用于流控

#### 3.3.2 UdpClient

```cpp
class UdpClient {
public:
    virtual ~UdpClient() = default;

    virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
    virtual Result<> Bind(uint16_t port) = 0;  // 新增：本地绑定
    virtual void Disconnect() = 0;
    virtual Result<int> Send(const void* data, size_t len) = 0;
    virtual Result<int> SendTo(const void* data, size_t len,
                               std::string_view host, uint16_t port) = 0;

    // 回调
    void OnMessage(UdpMessageCallback callback);   // 携带源地址
    void OnError(ErrorCallback callback);

    bool IsConnected() const;
};
```

**改进**：
- 新增 `Bind()` 支持 UDP 服务端模式
- 新增 `SendTo()` 支持向不同地址发送
- `OnMessage` 携带源地址信息
- 新增 `OnError` 回调

#### 3.3.3 HttpClient

将原库的"流式 HTTP"和"模组 HTTP"统一为一个接口。

```cpp
class HttpClient {
public:
    virtual ~HttpClient() = default;

    // 配置
    virtual void SetTimeout(std::chrono::milliseconds timeout) = 0;
    virtual void SetHeader(std::string_view key, std::string_view value) = 0;
    virtual void SetBody(std::string body) = 0;
    virtual void SetKeepAlive(bool enable) = 0;
    virtual void SetTlsConfig(const TlsConfig& config) = 0;  // 新增

    // 重定向跟随，默认开启且限额很小。不是每个引擎都能兑现：把 HTTP 交给
    // 模组固件的引擎只有那个固件的重定向行为，调用方在它上面设置这两项，
    // 是在要求一件它做不到的事。所以这里给了默认实现（不生效）而不是纯虚。
    virtual void SetFollowRedirects(bool enable) { (void)enable; }
    virtual void SetMaxRedirects(int max) { (void)max; }

    // 执行请求
    virtual Result<HttpResponse> Execute(std::string_view method,
                                         std::string_view url) = 0;

    // 流式请求（大文件上传下载）
    virtual Result<> Open(std::string_view method,
                          std::string_view url) = 0;
    virtual Result<int> Read(void* buffer, size_t size) = 0;
    virtual Result<int> Write(const void* buffer, size_t size) = 0;
    virtual void Close() = 0;

    // 响应信息（Open 后可用）
    virtual Result<int> GetStatusCode() = 0;
    virtual std::string GetResponseHeader(std::string_view key) const = 0;
    virtual size_t GetContentLength() const = 0;
    // 响应体是否为分块框架。分块响应不带 Content-Length，所以上面那个在
    // 这种情况下返回 0 而后面仍有响应体；Read() 两种框架都会解码，这个
    // 方法是给想知道响应是怎么装的调用方看的。
    virtual bool IsChunked() const = 0;
};

struct HttpResponse {
    int status_code;
    std::map<std::string, std::string> headers;
    std::string body;
};
```

**改进**：
- 新增 `Execute()` 简单请求接口（请求-响应一步完成）
- 新增 `TlsConfig` 完整 TLS 配置
- `HttpResponse` 结构体封装完整响应
- 保留流式接口用于大文件场景

#### 3.3.4 MqttClient

```cpp
class MqttClient {
public:
    virtual ~MqttClient() = default;

    // 配置
    virtual void SetKeepAlive(int seconds) = 0;
    virtual void SetClientId(std::string_view client_id) = 0;
    virtual void SetCredentials(std::string_view user,
                                std::string_view pass) = 0;
    virtual void SetWill(const MqttWill& will) = 0;  // 新增：遗嘱
    virtual void SetTlsConfig(const TlsConfig& config) = 0;  // 新增

    // 连接
    virtual Result<> Connect(std::string_view host, uint16_t port,
                             bool clean_session = true) = 0;
    virtual void Disconnect() = 0;

    // 发布/订阅
    virtual Result<int> Publish(std::string_view topic,
                                std::string_view payload,
                                MqttQoS qos = MqttQoS::kQoS0,
                                bool retain = false) = 0;  // 返回 message_id
    virtual Result<int> Subscribe(std::string_view topic,
                                  MqttQoS qos = MqttQoS::kQoS0) = 0;
    virtual Result<> Unsubscribe(std::string_view topic) = 0;

    // 回调
    void OnConnected(EventCallback callback);
    void OnDisconnected(EventCallback callback);
    void OnMessage(MqttMessageCallback callback);   // 按值传递 MqttMessage
    void OnError(ErrorCallback callback);
    void OnPublishComplete(PublishAckCallback callback);  // 新增：QoS 确认

    bool IsConnected() const;
};
```

**改进**：
- 完整 QoS 支持（返回 message_id + OnPublishComplete 回调）
- 遗嘱消息支持
- 完整 TLS 配置
- 所有方法返回 `Result<>`

#### 3.3.5 WebSocketClient

```cpp
class WebSocketClient {
public:
    virtual ~WebSocketClient() = default;

    // 配置
    virtual void SetHeader(std::string_view key,
                           std::string_view value) = 0;
    virtual void SetTlsConfig(const TlsConfig& config) = 0;  // 新增
    virtual void SetHeartbeat(const HeartbeatConfig& config) = 0;      // 新增
    virtual void SetAutoReconnect(const ReconnectConfig& config) = 0;  // 新增

    // 连接
    virtual Result<> Connect(std::string_view url) = 0;
    virtual void Close(WebSocketCloseCode code =
                       WebSocketCloseCode::kNormal,
                       std::string_view reason = "") = 0;

    // 发送
    virtual Result<> Send(std::string_view data,
                          bool binary = false) = 0;
    virtual Result<> SendFragment(const void* data, size_t len,
                                  bool binary, bool fin) = 0;
    virtual void Ping(std::string_view payload = "") = 0;

    // 回调
    void OnConnected(EventCallback callback);
    void OnDisconnected(WebSocketCloseCallback callback);  // 携带关闭码
    void OnMessage(WebSocketMessageCallback callback);     // 携带 binary 标志
    void OnError(ErrorCallback callback);
    void OnPong(DataCallback callback);

    bool IsConnected() const;
};
```

**改进**：
- 内置心跳机制（`HeartbeatConfig`，可配置间隔和超时）
- 内置自动重连（`ReconnectConfig`，可配置重试次数与退避）
- 两者都整体替换配置，不用两个参数分别传入——否则"只改超时"会说不出口，
  只能把间隔一起重复一遍，且加第三个字段就再改一次签名
- `Close` 支持关闭码和原因
- `OnDisconnected` 携带关闭信息

### 3.4 Module HAL 设计

#### 3.4.1 HAL 基类

```cpp
class IModuleHal {
public:
    virtual ~IModuleHal() = default;

    // 模组识别
    virtual ModuleType GetModuleType() const = 0;
    virtual std::string GetModuleName() const = 0;
    virtual const ModuleCapabilities& GetCapabilities() const = 0;

    // 初始化与重置
    virtual Result<> Initialize(IAtChannel& channel) = 0;
    virtual Result<> Reset() = 0;

    // 网络注册
    virtual Result<> ConfigureNetwork(const NetworkConfig& config) = 0;
    virtual RegistrationState GetRegistrationState() = 0;
    virtual SignalInfo GetSignalInfo() = 0;

    // 设备信息
    virtual Result<std::string> GetImei() = 0;
    virtual Result<std::string> GetIccid() = 0;
    virtual Result<std::string> GetRevision() = 0;
    virtual Result<std::string> GetCarrier() = 0;

    // 低功耗
    virtual bool SupportsLowPower() const { return false; }
    virtual Result<> EnterSleep(const SleepConfig& config) { return NotSupported(); }
    virtual Result<> ExitSleep() { return NotSupported(); }
    virtual void SetDtrWakeup(bool enable) {}

    // 连接管理（TCP/UDP 基础接口）
    virtual Result<int> TcpConnect(std::string_view host, uint16_t port,
                                   bool ssl = false) = 0;
    virtual Result<> TcpClose(int connect_id) = 0;
    virtual Result<int> TcpSend(int connect_id, const void* data,
                                size_t len) = 0;

    virtual Result<int> UdpOpen(std::string_view host, uint16_t port) = 0;
    virtual Result<> UdpClose(int connect_id) = 0;
    virtual Result<int> UdpSend(int connect_id, const void* data,
                                size_t len) = 0;

    // 可选：HTTP 加速
    virtual bool HasBuiltinHttp() const { return false; }
    virtual Result<std::unique_ptr<HttpClient>> CreateBuiltinHttp() {
        return NotSupported();
    }

    // 可选：MQTT 加速
    virtual bool HasBuiltinMqtt() const { return false; }
    virtual Result<std::unique_ptr<MqttClient>> CreateBuiltinMqtt() {
        return NotSupported();
    }

protected:
    Result<> NotSupported() const {
        return NetworkError::NotInitialized();
    }
};
```

#### 3.4.2 模组实现示例：ML307 HAL

```cpp
class Ml307Hal : public IModuleHal {
public:
    ModuleType GetModuleType() const override { return ModuleType::ML307; }
    std::string GetModuleName() const override { return "ML307"; }

    const ModuleCapabilities& GetCapabilities() const override {
        static const ModuleCapabilities caps = {
            .tcp = true,
            .udp = true,
            .ssl_tcp = true,
            .http = true,
            .https = true,
            .mqtt = true,
            .mqtts = true,
            .file_system = true,
            .low_power = true,
            .max_connections = 8,
            .max_baud_rate = 921600,
        };
        return caps;
    }

    Result<> Initialize(IAtChannel& ch) override;
    Result<> Reset() override;
    // ... 所有方法的 ML307 特定实现
};
```

**新增模组的工作量**：只需实现一个 HAL 类（约 500-800 行），而不是原库的 6+ 个类（2000+ 行）。

### 3.5 AT 通道与解析器

#### 3.5.1 AtUart（传输层）

保留原库基于 DMA 的高性能设计，改进解析器。

```cpp
class AtUart : public IAtChannel {
public:
    AtUart(const UartConfig& config);
    ~AtUart() override;

    // IAtChannel 实现
    AtResult SendCommand(std::string_view cmd,
                         std::chrono::milliseconds timeout) override;
    AtResult SendCommandWithData(std::string_view cmd,
                                 const void* data, size_t len,
                                 std::chrono::milliseconds timeout) override;
    // ... 其他接口

private:
    // 改进的响应解析
    struct ParsedResponse {
        bool success;           // OK / ERROR
        AtError error;          // 错误详情
        std::vector<std::string> lines;  // 响应行（不含状态行）
        std::optional<int> cme_code;     // +CME ERROR
    };

    ParsedResponse ParseResponse(const std::string& raw);

    // 改进的 URC 分发
    std::unordered_map<std::string, std::vector<UrcHandler>> urc_handlers_;
    void DispatchUrc(std::string_view line);
};
```

**改进点**：
- 响应解析结果结构化
- URC 分发使用哈希表，O(1) 查找
- 支持"前缀匹配"注册（如 `"+CME ERROR:"` 匹配所有 CME 错误）

#### 3.5.2 AtResponseParser（响应解析辅助）

提供一组解析辅助函数，减少各模块重复代码。

```cpp
namespace at_parser {

// 解析 CSV 行，自动处理引号
std::vector<std::string_view> SplitCsv(std::string_view line);

// 解析带类型的值（自动识别 int/double/string）
AtArgumentValue ParseValue(std::string_view raw);

// 解析键值对：+KEY: value
bool ParseKeyValue(std::string_view line,
                   std::string_view& key,
                   std::string_view& value);

// 解析数字列表：1,2,3,4
std::vector<int> ParseIntList(std::string_view line);

// 解析十六进制字符串
std::vector<uint8_t> ParseHex(std::string_view hex);

// 提取带引号的字符串
std::string_view StripQuotes(std::string_view s);

}  // namespace at_parser
```

## 四、错误体系设计

沿用原库的优秀设计，略做扩展。

### 4.1 错误层次

```
NetworkError (通用网络错误)
├── TransportError (传输层错误)
│   ├── TcpError
│   ├── UdpError
│   └── TlsError
├── ProtocolError (协议层错误)
│   ├── HttpError
│   ├── MqttError
│   └── WsError
└── AtError (AT 指令层错误)
    ├── CmeError
    └── CmsError
```

### 4.2 错误码枚举扩展

```cpp
enum class NetworkErrc {
    // 通用
    InvalidArgument,
    NotInitialized,
    NotSupported,        // 新增：模组不支持此功能
    Timeout,
    Canceled,            // 新增：操作被取消
    ResourceBusy,        // 新增：资源忙（连接数已满等）

    // DNS
    DnsFailed,

    // 连接
    ConnectFailed,
    ConnectionRefused,
    ConnectionReset,
    ConnectionLost,

    // TLS/SSL
    TlsHandshakeFailed,
    TlsCertificateInvalid,
    TlsCertificateExpired,
    TlsHostnameMismatch,

    // 认证
    AuthRejected,
    AuthTimeout,

    // 数据传输
    TransmitFailed,
    ReceiveFailed,
    BufferOverflow,

    // 网络状态
    NetworkUnavailable,
    NetworkDetached,
    RoamingNotAllowed,

    // 协议
    ProtocolError,
    HttpErrorStatus,      // HTTP 非 2xx 响应
    MqttNotAuthorized,
    WsHandshakeFailed,

    // AT 指令
    AtCommandError,
    AtCmeError,
    AtCmsError,
    AtTimeout,

    // 模组
    ModemNotResponding,
    SimNotDetected,
    SimPinRequired,
    SimPukRequired,

    // 其他
    Unknown,
};
```

### 4.3 错误构造与转换

```cpp
class NetworkError {
public:
    NetworkErrc code;
    int native;                     // 原生错误码
    std::string context;            // 上下文信息（可选）

    // 便捷构造
    static NetworkError Timeout(std::string_view ctx = "");
    static NetworkError DnsFailed(int herr, std::string_view host = "");
    static NetworkError FromAtError(const AtError& at_err);
    static NetworkError FromErrno(int err);
    static NetworkError FromEspError(esp_err_t err);

    // 输出
    const char* Name() const;       // 短名称："dns_failed"
    const char* Message() const;    // 用户可读消息
    std::string ToString() const;   // 完整描述
};
```

## 五、平台抽象层

为了可测试性和未来的多平台支持，引入 Platform 抽象。

### 5.1 接口定义

```cpp
namespace platform {

// 任务/线程
class ITask {
public:
    virtual ~ITask() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
};

using TaskFunction = std::function<void()>;
std::unique_ptr<ITask> CreateTask(std::string_view name,
                                  TaskFunction func,
                                  size_t stack_size = 4096,
                                  int priority = 5);

// 队列
template <typename T>
class IQueue {
public:
    virtual ~IQueue() = default;
    virtual bool Send(const T& item, std::chrono::milliseconds timeout) = 0;
    virtual bool Receive(T& item, std::chrono::milliseconds timeout) = 0;
};

template <typename T>
std::unique_ptr<IQueue<T>> CreateQueue(size_t max_items);

// 事件组/标志
class IEventGroup {
public:
    virtual ~IEventGroup() = default;
    virtual void SetBits(uint32_t bits) = 0;
    virtual void ClearBits(uint32_t bits) = 0;
    virtual uint32_t WaitBits(uint32_t bits, bool clear_on_exit,
                              bool wait_all,
                              std::chrono::milliseconds timeout) = 0;
};

std::unique_ptr<IEventGroup> CreateEventGroup();

// 时间
std::chrono::milliseconds Now();
void Sleep(std::chrono::milliseconds duration);

// 定时器
class ITimer {
public:
    virtual ~ITimer() = default;
    virtual void Start(std::chrono::milliseconds period,
                       bool periodic = false) = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
};

using TimerCallback = std::function<void()>;
std::unique_ptr<ITimer> CreateTimer(TimerCallback callback);

// 互斥锁
using Mutex = std::mutex;  // 直接用 std::mutex
using Lock = std::unique_lock<std::mutex>;

// GPIO
enum class GpioLevel { Low, High };
using GpioInterruptHandler = std::function<void(GpioLevel)>;

class IGpio {
public:
    virtual ~IGpio() = default;
    virtual void SetLevel(GpioLevel level) = 0;
    virtual GpioLevel GetLevel() const = 0;
    virtual void SetDirection(bool output) = 0;
    virtual void EnableInterrupt(int edge, GpioInterruptHandler handler) = 0;
    virtual void DisableInterrupt() = 0;
};

std::unique_ptr<IGpio> CreateGpio(int pin);

// 串口（底层）
class IUart {
public:
    virtual ~IUart() = default;
    virtual void SetBaudRate(int baud) = 0;
    virtual int Send(const void* data, size_t len) = 0;
    virtual int Receive(void* buffer, size_t len,
                        std::chrono::milliseconds timeout) = 0;
    // DMA 支持（可选）
    virtual bool SupportsDma() const { return false; }
    virtual void StartDmaRx(size_t buffer_size,
                            DmaRxCallback callback) {}
    virtual void StopDmaRx() {}
};

std::unique_ptr<IUart> CreateUart(const UartConfig& config);

}  // namespace platform
```

### 5.2 实现策略

- **ESP-IDF 实现**：生产环境使用，封装 FreeRTOS + ESP-IDF API
- **POSIX 实现**：用于 PC 单元测试，封装 pthread + socket
- **Mock 实现**：用于单元测试，Google Mock 模拟

## 六、配置系统

### 6.1 配置层级

```
编译时配置 (compile-time)  ── Kconfig / CMake
    ↓
设备级配置 (device-level)   ── DeviceConfig 结构体
    ↓
协议级配置 (protocol-level) ── TcpConfig / MqttConfig 等
    ↓
连接级配置 (connection-level) ── 方法参数
```

### 6.2 配置结构体示例

```cpp
struct DeviceConfig {
    // 引脚配置
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    gpio_num_t dtr_pin = GPIO_NUM_NC;
    gpio_num_t ri_pin = GPIO_NUM_NC;
    gpio_num_t power_pin = GPIO_NUM_NC;
    gpio_num_t reset_pin = GPIO_NUM_NC;

    // 串口配置
    uart_port_t uart_port = UART_NUM_1;
    int initial_baud = 115200;
    int target_baud = 921600;

    // 网络配置
    std::string apn = "";
    std::string pdp_user = "";
    std::string pdp_pass = "";

    // 高级配置
    bool auto_detect_baud = true;
    bool auto_set_baud = true;
    bool auto_reboot_on_failure = true;
    int max_init_retries = 3;
};
```

## 七、扩展指南

### 7.1 新增模组步骤

1. 创建 `module_name_hal.h` 和 `module_name_hal.cc`
2. 实现 `IModuleHal` 接口
3. 在 `ModuleRegistry` 中注册检测方法
4. 编写单元测试

典型代码量：500-800 行（对比原库 2000+ 行）

### 7.2 新增协议步骤

1. 定义协议接口类（继承自对应基类）
2. 实现"软件模式"版本（基于 TCP/UDP）
3. 在有内置支持的模组 HAL 中添加"内置模式"版本
4. 在 `NetworkInterface` 中添加创建方法

### 7.3 新增平台步骤

1. 实现 `platform` 命名空间下的所有接口
2. 移植 `AtUart` 到新平台的串口
3. 运行单元测试验证

## 八、与原库的兼容性

### 8.1 兼容层（可选）

为了降低现有用户迁移成本，可以提供一个兼容包装层。

```cpp
// 兼容头文件：at_modem_compat.h
// 将新 API 包装为原库风格的 API

class AtModemCompat {
public:
    static AtValue<std::unique_ptr<AtModemCompat>> Detect(...) {
        // 内部调用 CellularDevice::Detect()
    }
    // ... 包装所有原 API
};
```

### 8.2 保留的设计

以下原库设计被直接保留或扩展：

| 原库设计 | 新库处理 |
|---|---|
| `NetworkError` / `NetworkResult` | 保留并扩展 |
| `AtError` / `AtResult` | 保留 |
| `NetworkInterface` 抽象工厂 | 保留并扩展能力查询 |
| `DtrGuard` RAII | 保留并推广 |
| 自动模组检测 | 保留，增强为 HAL 注册机制 |
| DMA 串口架构 | 保留，改进解析器 |
| URC 回调机制 | 保留，改用哈希表分发 |

### 8.3 不兼容的变更

| 变更 | 原因 |
|---|---|
| `CreateXxx()` 返回 `Result<unique_ptr>` | 失败时明确错误原因 |
| MQTT `Publish/Subscribe` 返回 `Result<>` | 统一错误处理 |
| TCP/UDP 新增 `OnError` 回调 | 统一错误传递 |
| HTTP 新增 `Execute()` 方法 | 简化常见用例 |
| 新增 `CellularDevice` 顶层类 | 分离设备管理与网络功能 |
| 头文件目录重构 | 模组头文件私有，只暴露公共接口 |
