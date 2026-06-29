# 错误码参考

## 概述

CortexLink 系统中所有 MQTT 消息的回复均使用统一的错误码体系。错误码通过 `resp` 字段（整数）传递，`0` 表示成功。

---

## 设备端错误码（DeviceRespCode）

以下错误码在 `device/{uuid}/resp/m2s` 和 `device/{uuid}/resp/s2m` 中使用：

| 值 | 枚举 | 含义 | 说明 |
|----|------|------|------|
| 0 | `OK` | 成功 | 操作正常完成 |
| 1 | `DEVICE_NOT_FOUND` | 设备未注册 | 设备 UUID 不在数据库中，需先上报 `broadcast/online` |
| 2 | `DEVICE_OFFLINE` | 设备当前离线 | 设备心跳超时（>30s），已标记为 offline |
| 3 | `INTERNAL_ERROR` | 主机内部错误 | 数据库读写失败、内存分配失败等 |
| 4 | `INVALID_REQUEST` | 请求 JSON 解析失败 | 消息载荷不是合法 JSON 或结构不符合约定 |
| 5 | `MISSING_FIELD` | 必填字段缺失 | 如缺少 `msg_id`、`evt_id`、`act_id` 等 |
| 6 | `TIMEOUT` | 操作超时 | 主机等待设备响应超时 |
| 20 | `ACTION_NOT_FOUND` | 动作 ID 不识别 | 下发的 `act_id` 不在设备声明的 actions 列表中 |
| 21 | `INVALID_PARAMS` | 参数校验失败 | 参数类型/范围不符合设备声明的约束 |
| 22 | `ACTION_FAILED` | 动作执行失败 | 设备回复动作执行失败 |
| 23 | `DEVICE_BUSY` | 设备忙 | 设备正在执行其他操作，暂时无法响应 |
| 99 | `UNKNOWN_ERROR` | 未知错误 | 其他未分类的错误 |

---

## APP SQL 代理错误码

以下错误码在 `app/sql/resp` 中使用：

| 值 | 含义 | 说明 |
|----|------|------|
| 0 | 成功 | 查询正常完成 |
| 1 | 请求 JSON 解析失败 | `app/sql/trans` 载荷不是合法 JSON |
| 2 | 必填字段缺失 | 缺少 `msg_id`、`cmd` 或 `timestamp` |
| 3 | 未知 cmd | `cmd` 操作码未在主机注册 |
| 4 | params 参数不合法 | 参数类型/数量不符合策略要求 |
| 5 | SQL 执行错误 | 数据库查询异常 |

---

## Cron 设备错误码

Cron 抽象设备（UUID: `00000000-0000-0000-0000-000000000001`）的响应中也使用上述 `DeviceRespCode`：

| 响应码 | 含义 | 触发场景 |
|--------|------|----------|
| 0 | 成功 | 操作正常完成 |
| 4 | 请求无效 | action payload JSON 解析失败 |
| 5 | 缺少字段 | `add_cron` 缺少 `expr`、`add_relative_cron` 缺少 `offset`、`remove_cron` 缺少 `cron_id` |
| 20 | 动作未识别 | act_id 不是 `add_cron` / `add_relative_cron` / `remove_cron` / `list_crons` |
| 21 | 参数无效 | cron 表达式语法错误、offset 格式错误 |
| 3 | 内部错误 | crontab.txt 读写失败 |

---

## 设备端错误处理建议

### 收到非零 resp 时的处理

```
收到 resp/m2s
  │
  ├─ resp = 0 ───── 正常，继续
  │
  ├─ resp = 4, 5 ── 检查发送的 JSON 格式，修正后重试
  │
  ├─ resp = 1 ───── 设备未注册，重新发送 broadcast/online
  │
  ├─ resp = 2 ───── 设备被标记为 offline，发送心跳恢复
  │
  ├─ resp = 20, 21 ─ 动作/参数不匹配，检查固件中 act_id 和 params 定义
  │
  ├─ resp = 6 ───── 主机等待超时，无需特殊处理（主机可能已放弃本次操作）
  │
  ├─ resp = 3, 99 ─ 主机内部错误，记录日志，可选择性重试
  │
  └─ resp = 22 ──── 动作执行失败，记录日志
```

### 设备回复主机时

设备在 `device/{uuid}/resp/s2m` 中回复时，应根据实际执行结果设置正确的 `resp` 值：

- 动作执行成功 → `resp = 0`
- 动作 ID 不识别 → `resp = 20`
- 参数校验失败 → `resp = 21`
- 动作执行失败（硬件故障等） → `resp = 22`
- 设备忙 → `resp = 23`
