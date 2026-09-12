# 设计文档目录

本文档集合是对 `esp-ml307` 库的深入分析以及设计一个新的多模组 AT 蜂窝网络库的规划。

## 文档列表

| 文档 | 说明 |
|---|---|
| [01_library_analysis.md](01_library_analysis.md) | **库分析报告** — 对 esp-ml307 v3.7.2 的全面分析，包括架构、优点、缺点和改进方向 |
| [02_multi_module_architecture.md](02_multi_module_architecture.md) | **架构设计** — 新多模组库的整体架构设计，核心是 Module HAL 模式 |
| [03_api_design_spec.md](03_api_design_spec.md) | **API 设计规范** — 详细的接口定义、使用示例和与原库的对比 |

## 核心设计思路

### 从原库继承的优点

1. **结构化错误体系**：`NetworkError` / `NetworkResult`（基于 `std::expected`）设计优秀，直接沿用
2. **抽象工厂模式**：`NetworkInterface` 统一不同网络后端的思路保留并扩展
3. **自动模组检测**：`Detect()` 模式保留，增强为 HAL 注册机制
4. **RAII 设计**：`DtrGuard` 低功耗管理模式推广到更多场景
5. **DMA 串口架构**：基于 UHCI DMA 的双任务 AT 通信层保留

### 核心创新：Module HAL 模式

原库每新增一个模组需要实现 6+ 个类（AtModem + Tcp + Ssl + Udp + Mqtt + Http），代码重复率高。

新库引入 **Module HAL（模组硬件抽象层）**，将所有模组差异封装在一个 HAL 类中：
- 新增模组只需实现一个 HAL 类（500-800 行）
- 协议实现（HTTP/WebSocket/MQTT 等）与具体模组解耦
- 模组内置协议 vs 软件实现协议自动选择最优路径

### 架构分层

```
应用层
  ↓
NetworkInterface (抽象工厂)
  ↓
Protocol Engine (协议引擎：可选内置加速或软件实现)
  ↓
Module HAL (模组适配层 — 扩展模组的唯一接口)
  ↓
IAtChannel (AT 通道抽象)
  ↓
AtUart (DMA 串口 + 解析器 + URC 分发)
  ↓
Platform Abstraction (平台抽象：支持 ESP-IDF / POSIX / Mock)
```

## 适用的模组范围

| 厂商 | 型号 | 类型 |
|---|---|---|
| 中移物联 | ML307R/A | Cat.1 |
| 移远 | EC801E / EC600N | Cat.1 |
| 移远 | BG95/BG96 | Cat.M1/NB-IoT |
| 广和通 | L610 | Cat.1 |
| 有方 | N58/N75 | Cat.1 |
| 芯讯通 | A7670C/A7600C | Cat.4 |

## 下一步

1. 搭建项目骨架（Platform 抽象层 + AtUart）
2. 实现 Module HAL 基类和第一个模组（ML307）
3. 实现传输层协议（TCP/UDP）
4. 实现应用层协议（HTTP/MQTT/WebSocket）
5. 移植第二个模组（EC801E）验证 HAL 模式的扩展性
6. 完善测试和文档
