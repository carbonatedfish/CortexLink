# CortexLink 设备开发文档

## 文档索引

| 文档 | 说明 | 目标读者 |
|------|------|----------|
| [mqtt-protocol.md](./mqtt-protocol.md) | MQTT Topic 定义、载荷格式、通信时序 | 设备固件开发者 |
| [device-onboarding.md](./device-onboarding.md) | 设备上线注册流程、属性定义指南 | 设备固件开发者 |
| [error-codes.md](./error-codes.md) | 系统错误码参考与错误处理建议 | 设备固件开发者 |
| [cron.md](./cron.md) | Cron 抽象设备（虚拟定时器）接口 | 规则脚本开发者 |

## 快速入门

设备端开发的典型流程：

1. **阅读 [mqtt-protocol.md](./mqtt-protocol.md)** — 了解 MQTT 通信协议和消息格式
2. **阅读 [device-onboarding.md](./device-onboarding.md)** — 了解设备如何注册上线
3. **实现固件** — 按文档实现 MQTT 连接、心跳、数据/事件上报、动作响应
4. **参考 [error-codes.md](./error-codes.md)** — 处理各种错误场景

## 关键约定速查

- **msg_id**：每条消息必须携带，使用 UUID 格式（UNIX 时间戳种子生成）
- **心跳间隔**：建议 10~15 秒，超过 30 秒无心跳主机标记 offline
- **数据值**：`data_val` 统一使用字符串传输，主机按 `type` 转换
- **设备 UUID**：出厂写入固件，MQTT Topic 中使用标准小写 UUID 格式
- **Topic 通配**：设备订阅 `device/{uuid}/action/#` 接收所有动作
