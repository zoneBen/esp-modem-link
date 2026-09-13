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
| 枚举值 | k 前缀 + PascalCase | `kTimeout`, `kQoS0` |
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
   // UDP 数据报：来源地址是主题，载荷是详情
   using UdpMessageCallback = std::function<void(std::string_view host,
                                                  uint16_t port,
                                                  std::string_view data)>;

   // TCP 数据：数据是主题，长度是详情（通过 string_view 一体表达）
   using DataCallback = std::function<void(std::string_view data)>;
   ```
   这条准则的例外是 MQTT：它的"详情"不止 payload 一样——qos、retain、
   message_id 都是随消息一起到达的投递属性，平铺成参数会让签名随协议长出
   新字段而改动，也会让调用方无法把"这条消息"当作一个东西传给别处。所以
   那里传的是按值的 `MqttMessage`（见 3.6），主题 + 详情的顺序仍然成立，
   只是都装在同一个结构体里。

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
enum class AtErrc { kTimeout, kCommandError, kCmeError, kCmsError,
                    kTransmitFailed, kNotInitialized, kNotSupported };

// cme 和 cms 只有一个有效，由 code 决定是哪个：构造时按 code 归位，所以
// 调用方不必自己判断该填哪个字段，也不会出现两个都填了而其中一个没意义
// 的情况。这里没有 esp_err_t 成员——AT 层不该依赖 ESP-IDF 的平台类型。
struct AtError {
    AtErrc code;
    int cme = 0;
    int cms = 0;
    std::string context;

    AtError(AtErrc code, std::string ctx = "");
    AtError(AtErrc code, int cme_or_cms, std::string ctx = "");

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
```

这里没有 WebSocket 的关闭回调：它要 `WebSocketCloseCode`，而那个类型属于
WebSocket 而不是这个头文件，所以别名定义在 `websocket_client.h` 里
（`WebSocketCloseCallback`）。同理 MQTT 的 `MqttMessageCallback` 定义在
`mqtt_client.h`——本头文件是一份"别人类型的别名清单"，不该反过来依赖它们。

### 2.3 配置结构体

每个模块的配置使用独立的结构体，所有字段有合理默认值。

```cpp
struct TlsConfig {
    // 默认不校验：目标模组的证书是模组自己持有的文件（ML307 的
    // AT+MSSLCFG="cert"），本库没有写入证书的通道，而验证过的
    // ML307R-DL-MBRH0S01 固件里一份 CA 都没有，auth=1 在任何主机上
    // 都握手失败。默认加密但不认证，与原 esp-ml307 无条件的行为一致。
    // 要求校验服务器需要同时置位两个标志——模组只有一条设置同时覆盖
    // 证书链与主机名，只给一个会被 HAL 显式报错而非替调用方猜。
    bool verify_certificate = false;
    bool verify_hostname = false;
    std::string ca_cert;            // PEM 格式 CA 证书
    std::string client_cert;        // PEM 格式客户端证书
    std::string client_key;         // PEM 格式客户端私钥
    std::string alpn_protocols;     // 逗号分隔，如 "h2,http/1.1"
    std::chrono::seconds handshake_timeout{10};
};

struct MqttWill {
    std::string topic;
    std::string payload;
    // int 而不是 MqttQoS：MqttQoS 定义在 mqtt_client.h，而这个头文件被它
    // include，反向依赖会成环。
    int qos = 0;
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

**接收回调**：源地址是回调的参数而不是一个 `ReceivedPacket` 结构体。一个
数据结构意味着一次分配或一次引用，而这里要传的三样东西——地址、端口、载荷
——都是调用方在这一个瞬间才需要的东西，且载荷是 `string_view`，指向的缓冲
在回调返回后就不再有效。收进结构体只会让它看起来像能留存。

```cpp
class UdpClient {
public:
    virtual ~UdpClient() = default;

    // 连接模式（固定远端地址）
    virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
    // 无连接模式：本地绑定
    virtual Result<> Bind(uint16_t port) = 0;
    virtual void Disconnect() = 0;

    // 发送
    virtual Result<int> Send(const void* data, size_t len) = 0;
    virtual Result<int> SendTo(const void* data, size_t len,
                               std::string_view host, uint16_t port) = 0;

    // 接收回调（携带源地址）
    void OnMessage(UdpMessageCallback callback) {
        on_message_ = std::move(callback);
    }

    // 错误
    void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }

    bool IsConnected() const { return connected_; }

protected:
    UdpMessageCallback on_message_;
    ErrorCallback on_error_;
    bool connected_ = false;
};
```

`UdpMessageCallback` 的形状就是"主题 + 详情"：地址和端口是主题（这条数据是
谁发来的），载荷是详情。

```cpp
using UdpMessageCallback = std::function<void(std::string_view host,
                                               uint16_t port,
                                               std::string_view data)>;
```

**设计说明**：
- 区分"连接模式"和"无连接模式"，两者可共存
- 接收回调携带源地址信息（原库缺失）
- 源地址是回调参数，不是一个 `ReceivedPacket` 结构体（见上）

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
    virtual Result<HttpResponse> Execute(std::string_view method,
                                         std::string_view url) = 0;

    // 流式请求（适合大文件/OTA）
    virtual Result<> Open(std::string_view method, std::string_view url) = 0;
    virtual Result<int> Read(void* buffer, size_t size) = 0;
    virtual Result<int> Write(const void* buffer, size_t size) = 0;
    virtual void Close() = 0;

    // 流式请求体（分块上传）。默认关。
    //
    // 与上面那条流式路径的关系，是本次设计里最容易读错的一处：Open() 在
    // 普通模式下等到响应头才返回，而服务端不会回答一个还没读完请求体的请
    // 求——所以开了分块上传之后 Open() 只发请求头就返回，响应头改由
    // EndBody() 等待。EndBody() 之后的用法与普通 Open() 之后完全相同。
    //
    //   SetBody("")           // 清掉先前的 body，否则 Open() 拒绝
    //   SetChunkedUpload(true)
    //   Open("POST", url)     // 只发头，立即返回
    //   Write(...)  ×N        // 每段一个块
    //   EndBody()             // 终止块 + 等响应头
    //   GetStatusCode() / Read() ×N
    //   Close()
    //
    // Write() 的 size 为 0 是合法输入且什么都不发：零长度块就是终止块本身，
    // 只有 EndBody() 能产出它。
    //
    // 与 SetBody() 互斥：调用方用 SetBody("") 清掉先前的 body，否则 Open() 报
    // kInvalidArgument——先量好的 body 和流式上传不可能同时是这次请求的。
    //
    // 两条限制：重定向无法跟随（请求体已经送出，无法重放，会显式报错而不是
    // 把 3xx 当作跟随成功交回）；HTTP/1.0 服务端不认识分块编码（症状是 4xx
    // 或挂到 EndBody() 超时）。另外 Write() 只受传输层约束，不受 SetTimeout()
    // 的期限约束——一次 Write() 在 AT 模组上是一串 AT+MIPSEND。
    virtual void SetChunkedUpload(bool enable) { (void)enable; }

    // 有默认实现且默认是失败的：返回 NotSupported，不是空实现。空实现返回的
    // 是成功，会把没发生的上传报成完成。固件自带 HTTP 的引擎正是靠这一条兜
    // 住——它的 SetChunkedUpload 只能是无操作。
    virtual Result<> EndBody() {
        return std::unexpected(NetworkError::NotSupported(...));
    }

    // 流式模式下的响应信息
    virtual Result<int> GetStatusCode() = 0;
    virtual std::string GetResponseHeader(std::string_view key) const = 0;
    virtual size_t GetContentLength() const = 0;
    // 说的是**响应**的分块，与 SetChunkedUpload() 的**请求**模式无关。
    virtual bool IsChunked() const = 0;
};

// 顶层类型，不是 HttpClient 的嵌套类型：Execute() 的返回值要被调用方存
// 起来、被传出去，嵌套会把这个名字绑在客户端上。
struct HttpResponse {
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};
```

**设计说明**：
- 提供两种使用模式：`Execute()` 简单请求 + `Open/Read/Write` 流式请求
- `HttpResponse` 结构体封装完整响应（顶层 `HttpResponse`，不是嵌套的
  `SimpleResponse`）
- `IsChunked()` 回答的是响应体的**框架**问题：分块响应不带 Content-Length，
  所以 `GetContentLength()` 返回 0 而后面仍有响应体。`Read()` 两种框架都
  会解码——这个方法只是给想知道"响应是怎么装的"的调用方看的
- 支持重定向跟随（原库缺失）
- 响应头查询不返回错误（没找到就是空字符串）

### 3.6 MqttClient

```cpp
enum class MqttQoS { kQoS0 = 0, kQoS1 = 1, kQoS2 = 2 };

// 一条消息连同代理给它的投递属性。属性属于消息而不是属于通知：调用方按
// 一个 QoS 订阅、却收到另一个 QoS 的消息时，没有别的地方能看出这条消息
// 是在哪个保证下到达的；而置了 retain 的发布和只是匹配了过滤器的发布也
// 不是一回事。
struct MqttMessage {
    std::string topic;
    std::string payload;
    MqttQoS qos = MqttQoS::kQoS0;
    // 只在订阅刚刚建立、代理因此送来这条消息时为 true。协议要求从既有订阅
    // 转发的消息上它必须是 clear，所以一般是 false——但不是一定。
    bool retain = false;
    // 它到达时那条 PUBLISH 的报文标识；QoS 0 时为 0，因为线上没有标识。
    // 回调运行时它已经不能当句柄用：相关的确认早就发出去了。
    int message_id = 0;
};

// 按值传递而不是按引用，两个理由指向同一个方向：消息自己持有那些字符串，
// 想留存的回调应该能直接拿走而不必再拷一次——而指向引擎局部变量的
// `const MqttMessage&` 在一个把它存下来的调用方眼里同样安全。引擎反正都要
// 构造这个对象，move 不花代价。
using MqttMessageCallback = std::function<void(MqttMessage message)>;

class MqttClient {
public:
    virtual ~MqttClient() = default;

    // 配置
    virtual void SetClientId(std::string_view client_id) = 0;
    virtual void SetCredentials(std::string_view user,
                                std::string_view pass) = 0;
    virtual void SetKeepAlive(int seconds) = 0;
    virtual void SetWill(const MqttWill& will) = 0;
    virtual void SetTlsConfig(const TlsConfig& config) = 0;

    // 连接。clean_session 是参数而不是 setter：它是 CONNECT 报文的一个字段，
    // 而 CONNECT 只发一次——setter 唯一能做的事就是改变下一次 Connect() 的
    // 含义，而这里已经在做出调用的同一句话里说清了。
    virtual Result<> Connect(std::string_view host,
                             uint16_t port,
                             bool clean_session = true) = 0;
    virtual void Disconnect() = 0;

    // 发布
    virtual Result<int> Publish(std::string_view topic,
                                std::string_view payload,
                                MqttQoS qos = MqttQoS::kQoS0,
                                bool retain = false) = 0;

    // 订阅
    virtual Result<int> Subscribe(std::string_view topic,
                                  MqttQoS qos = MqttQoS::kQoS0) = 0;
    virtual Result<> Unsubscribe(std::string_view topic) = 0;

    // 回调
    void OnConnected(EventCallback callback) { on_connected_ = std::move(callback); }
    void OnDisconnected(EventCallback callback) { on_disconnected_ = std::move(callback); }
    void OnMessage(MqttMessageCallback callback) {
        on_message_ = std::move(callback);
    }
    void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
    // 代理收下一条消息时触发：QoS 1 发布的 PUBACK，或 QoS 2 的 PUBCOMP。
    // 命名指向握手的结束而不是其中任何一半，且 QoS 0 永不触发——它没有
    // 任何东西回过来。
    void OnPublishComplete(PublishAckCallback callback) {
        on_publish_ack_ = std::move(callback);
    }

    bool IsConnected() const { return connected_; }

protected:
    EventCallback on_connected_;
    EventCallback on_disconnected_;
    MqttMessageCallback on_message_;
    ErrorCallback on_error_;
    PublishAckCallback on_publish_ack_;
    bool connected_ = false;
};
```

**设计说明**：
- `Publish()` 返回消息 ID（QoS 1/2 时用于确认追踪）
- `OnMessage` 传递 `MqttMessage` 结构体（按值），包含
  topic/payload/qos/retain/message_id
- `OnPublishComplete` 回调（QoS 1/2 发布确认；原名 `OnPublishAck`，因为它
  覆盖 QoS 1 的 PUBACK 和 QoS 2 的 PUBCOMP 两半）
- `clean_session` 是 `Connect()` 的参数，不是 `SetCleanSession()` setter
- 配置方法在 Connect 前调用，不返回错误（参数错误在 Connect 时报）

### 3.7 WebSocketClient

```cpp
// 本库点得出名字的关闭码。这不是一个封闭集合：对端可以发协议允许范围内
// 的任意码——留给应用的那一段，以及本文件写成之后才注册的——所以没有名字
// 的码按它本来的数字上报，而不会被就近归类或丢掉。
enum class WebSocketCloseCode : uint16_t {
    kNormal = 1000,
    kGoingAway = 1001,
    kProtocolError = 1002,
    kUnsupportedData = 1003,
    kNoStatus = 1005,
    kAbnormalClosure = 1006,
    kInvalidPayloadData = 1007,
    kPolicyViolation = 1008,
    kMessageTooBig = 1009,
    kMandatoryExtension = 1010,
    kInternalError = 1011,
};

// 连接是怎么结束的：对端发了码就是它那个，没发就是本端选的那个。1005 和
// 1006 在这里上报，且从不上线——那就是它们的含义。
using WebSocketCloseCallback =
    std::function<void(WebSocketCloseCode code, std::string_view reason)>;

// 一条消息，连同发送方给它的类型。这个标志是文本帧和二进制帧离开它就无法
// 区分的唯一依据，而 Send()/SendFragment() 都让发送方说明自己在发哪种，
// 所以收不到它的接收方就成了这一对里不对称的那一半。分片到达的消息带的是
// 开启它的那一帧的类型——后面的续帧不携带。
using WebSocketMessageCallback =
    std::function<void(std::string_view data, bool binary)>;

class WebSocketClient {
public:
    virtual ~WebSocketClient() = default;

    // 配置
    virtual void SetHeader(std::string_view key, std::string_view value) = 0;
    // wss:// 握手的设置。与 MQTT 不同，本客户端从 URL（ws:// 与 wss://）
    // 读调用方的意图，所以这里是配置一条 scheme 已经要求了的 TLS 连接，
    // 而不是打开它。也与 HTTP 不同——HTTP 可以跟随重定向跨过这条边界，
    // 因此每个请求都重读配置：WebSocket 连接由一次 upgrade 打开，URL 是
    // 事先知道的。连接建立时读取，所以改动从下一次 Connect() 生效，对一条
    // 已经打开的 socket 无效——它早就过了握手。ws:// 连接完全忽略它。
    virtual void SetTlsConfig(const TlsConfig& config) = 0;

    // 心跳。不置 enabled 就是关的，间隔为 0 时也是关的——每零秒一次 ping
    // 是循环而不是心跳。timeout 是对端回答一个 ping 的期限，超时即认为连接
    // 已死；timeout 为 0 表示改用 interval，所以只给了间隔的调用方仍然有
    // 一个截止时间。
    //
    // 每次调用替换整个配置：没写到的字段回到下面的默认值，enabled 也是。
    // 用指定初始化器点名字段是"只改一样、其余不动"的写法——
    // SetHeartbeat({.interval = 5s, .enabled = true})——而只点要改的那个会
    // 把心跳关掉，而不是调节它。
    struct HeartbeatConfig {
        std::chrono::seconds interval{30};
        std::chrono::seconds timeout{10};
        bool enabled = false;
    };
    virtual void SetHeartbeat(const HeartbeatConfig& config) = 0;

    // 自动重连。不置 enabled 就是关的。连接掉线或心跳失败的连接随后按这个
    // 计划重建：等待从 initial_delay 开始，每次失败后乘以 backoff_factor，
    // 直到 max_delay。系数小于等于 1 时等待保持不变而不是缩短，起始为 0
    // 表示立刻重试。max_retries 限制连续尝试次数，-1 表示不限。
    //
    // 与 SetHeartbeat 一样，每次调用替换整个配置。
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
    // 发一个关闭帧并短暂等待对端的，然后通过 OnDisconnected 上报连接是怎么
    // 结束的：对端回答了就是它给的码，没回答就是这里要的那个。
    virtual void Close(WebSocketCloseCode code = WebSocketCloseCode::kNormal,
                       std::string_view reason = "") = 0;

    // 发送
    virtual Result<> Send(std::string_view data, bool binary = false) = 0;
    // 发送调用方自己在切分的一条消息中的一帧。一条消息的第一帧带文本或
    // 二进制类型且 fin 为 false，其余是续帧——这就是消息开始之后不再看
    // `binary` 的原因。一串分片是一次成序的调用：两个线程同时分片会把各自
    // 的消息交错在一起。
    virtual Result<> SendFragment(const void* data, size_t len,
                                  bool binary, bool fin) = 0;
    // 发一个 ping 并为它的 pong 开始计时。每个 pong 都会触发 OnPong，
    // 包括心跳的那个。
    virtual void Ping(std::string_view payload = "") = 0;

    // 回调
    void OnConnected(EventCallback callback) { on_connected_ = std::move(callback); }
    void OnDisconnected(WebSocketCloseCallback callback) {
        on_disconnected_ = std::move(callback);
    }
    void OnMessage(WebSocketMessageCallback callback) {
        on_message_ = std::move(callback);
    }
    void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
    void OnPong(DataCallback callback) { on_pong_ = std::move(callback); }

    bool IsConnected() const { return connected_.load(); }

protected:
    EventCallback on_connected_;
    WebSocketCloseCallback on_disconnected_;
    WebSocketMessageCallback on_message_;
    ErrorCallback on_error_;
    DataCallback on_pong_;
    // atomic：引擎在传输层的接收线程上写它，应用在自己的线程上读。
    std::atomic<bool> connected_{false};
};
```

**设计说明**：
- 内置心跳和自动重连（原库缺失，用户需自己实现）
- 关闭码使用枚举类（`WebSocketCloseCode`），提高可读性
- `OnDisconnected` 携带关闭码和原因
- `SendFragment()` 由调用方自己分片，与服务端分片到达时的重组相对应；
  两条路都保留，因为分片对发送方是"我不想一次拿出一整条消息"，对接收方
  是"对端没有那么做"

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
    virtual std::vector<std::string> GetResponseLines() const = 0;

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

    // 这个期限管的是等响应：Open() 等响应头、Read() 等 body、Execute() 等整
    // 个响应。它不管 Write()——一次 Write() 在 AT 模组上是一串 AT+MIPSEND，
    // 每条的期限由 AT 层自己定，所以上传大 body 的调用方不能靠这一个
    // SetTimeout() 兜住全部时间。
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
    mqtt->SetWill({"status/offline", "device-offline", 1, false});

    mqtt->OnConnected([]() { ESP_LOGI(TAG, "MQTT connected"); });
    mqtt->OnDisconnected([]() { ESP_LOGI(TAG, "MQTT disconnected"); });
    mqtt->OnMessage([](MqttMessage msg) {
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

    mqtt->Subscribe("test/esp32/in", MqttQoS::kQoS1);
    mqtt->Publish("test/esp32/out", "hello", MqttQoS::kQoS0);
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
    ws->OnDisconnected([](WebSocketCloseCode code, std::string_view reason) {
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
| HTTP 简单请求 | 无 | `Execute(method, url) -> Result<HttpResponse>` | 新增便捷方法 |
| HTTP 重定向 | 不支持 | `SetFollowRedirects(true)` | 新增 |
| MQTT 发布 | `Publish(topic, payload, qos) -> bool` | `Publish(topic, payload, qos, retain) -> Result<int>` | 返回 message_id，支持 retain |
| MQTT 遗嘱 | 不支持 | `SetWill(will)` | 新增 |
| MQTT QoS 确认 | 不支持 | `OnPublishComplete(callback)` | 新增（覆盖 PUBACK 与 PUBCOMP） |
| WS 心跳 | 手动实现 | `SetHeartbeat(config)` | 内置 |
| WS 自动重连 | 手动实现 | `SetAutoReconnect(config)` | 内置 |
| 错误类型 | `NetworkError` | `NetworkError`（扩展） | 新增更多错误码 |
