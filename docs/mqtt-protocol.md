# MQTT 协议参考 — 设备端

## 概述

CortexLink 系统中所有设备通过 MQTT 连接到远程 Mosquitto broker 与主机通信。本文档定义设备端需要使用的所有 MQTT Topic 及载荷格式。

Broker 连接凭证（用户名/密码）嵌入在设备固件中。

---

## 通用约定

### msg_id

每条消息必须携带 `msg_id` 字段，格式为 UUID（v4），由发送方以当前 UNIX 时间戳作为种子生成。

### 响应码

`resp` 字段为整数，对应 `DeviceRespCode` 枚举。`0` = 成功，其他值见 [错误码参考](./error-codes.md)。

### JSON 格式

所有消息载荷均为 JSON 格式，使用 UTF-8 编码。

---

## Topic 一览

| Topic | 方向 | 说明 |
|-------|------|------|
| `broadcast/online` | 设备→主机 | 设备上线上报属性 |
| `device/{uuid}/heartbeat` | 设备→主机 | 心跳 |
| `device/{uuid}/event/{type}` | 设备→主机 | 事件上报 |
| `device/{uuid}/data/{type}` | 设备→主机 | 数据上报 |
| `device/{uuid}/resp/s2m` | 设备→主机 | 设备回复 |
| `device/{uuid}/action/{type}` | 主机→设备 | 动作下发（设备订阅） |
| `device/{uuid}/config` | 主机→设备 | 配置更改（设备订阅） |
| `device/{uuid}/resp/m2s` | 主机→设备 | 主机回复（设备订阅） |
| `broadcast/llm/trans` | 设备→主机 | 提交规则生成提示词 |

> `{uuid}` 为设备的唯一标识符（小写 UUID 格式，如 `a1b2c3d4-e5f6-4789-abcd-ef0123456789`）。

### 设备必须订阅的 Topic

```
device/{uuid}/action/#
device/{uuid}/config
device/{uuid}/resp/m2s
```

---

## 各 Topic 载荷格式

### broadcast/online — 设备上线

设备首次连接或重新上线时向此 Topic 上报完整属性。

```json
{
    "dev_id": "a1b2c3d4-e5f6-4789-abcd-ef0123456789",
    "dev_name": "客厅温度传感器",
    "dev_type": "sensor",
    "dev_subtype": "temperature",
    "location": "客厅",
    "user_param": "用于监测客厅环境温度",
    "actions": [
        {
            "act_id": "act-001",
            "act_name": "reboot",
            "desc": "重启设备",
            "params": [],
            "pre_cond": ""
        }
    ],
    "event": [
        {
            "evt_id": "evt-001",
            "evt_name": "temp_high",
            "desc": "温度超过阈值",
            "params": [
                {
                    "p_name": "temperature",
                    "desc": "当前温度值",
                    "p_type": "float",
                    "unit": "°C"
                }
            ]
        }
    ],
    "data": [
        {
            "d_name": "temperature",
            "desc": "当前温度",
            "type": "float",
            "unit": "°C"
        },
        {
            "d_name": "humidity",
            "desc": "当前湿度",
            "type": "float",
            "unit": "%"
        }
    ]
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `dev_id` | string(UUID) | 是 | 设备唯一标识 |
| `dev_name` | string | 是 | 设备显示名称 |
| `dev_type` | string | 是 | `sensor` / `actuator` / `composite` |
| `dev_subtype` | string | 否 | 设备细分类型 |
| `location` | string | 否 | 设备安装位置 |
| `user_param` | string | 否 | 用户配置参数，辅助 LLM 理解设备意图 |
| `actions` | array | 否 | 设备可执行的动作列表 |
| `event` | array | 否 | 设备可上报的事件列表 |
| `data` | array | 否 | 设备可上报的数据列表 |

### device/{uuid}/heartbeat — 心跳

设备周期性发送心跳，保持在线状态。主机超过 30 秒未收到心跳则将设备标记为 `offline`。

```json
{
    "msg_id": "b2c3d4e5-f6a7-4890-bcde-f01234567890"
}
```

**建议心跳间隔：** 10~15 秒。

### device/{uuid}/event/{type} — 事件上报

设备检测到事件发生时上报。

```json
{
    "msg_id": "b2c3d4e5-f6a7-4890-bcde-f01234567890",
    "evt_id": "evt-001",
    "params": [
        {
            "p_name": "temperature",
            "value": 36.5
        }
    ]
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msg_id` | string(UUID) | 是 | 消息 ID |
| `evt_id` | string | 是 | 事件 ID（与 broadcast/online 中声明的 `evt_id` 一致） |
| `params` | array | 否 | 事件参数，每项含 `p_name` + `value` |

### device/{uuid}/data/{type} — 数据上报

设备周期性或按需上报传感器数据。

```json
{
    "msg_id": "b2c3d4e5-f6a7-4890-bcde-f01234567890",
    "data_name": "temperature",
    "data_val": "25.5"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msg_id` | string(UUID) | 是 | 消息 ID |
| `data_name` | string | 是 | 数据名称（与 broadcast/online 中声明的 `d_name` 一致） |
| `data_val` | string | 是 | 实际数据值（统一用字符串传输，主机侧按类型转换） |

### device/{uuid}/action/{type} — 动作下发（主机→设备）

主机通过此 Topic 向设备下发动作指令。**设备必须订阅此 Topic。**

```json
{
    "msg_id": "c3d4e5f6-a7b8-4901-cdef-012345678901",
    "act_id": "act-001",
    "params": [
        {
            "p_name": "state",
            "value": "on"
        }
    ]
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msg_id` | string(UUID) | 是 | 消息 ID |
| `act_id` | string | 是 | 动作 ID（与 broadcast/online 中声明的 `act_id` 一致） |
| `params` | array | 否 | 动作参数，每项含 `p_name` + `value` |

设备执行完毕后应通过 `device/{uuid}/resp/s2m` 回复执行结果。

### device/{uuid}/config — 配置更改（主机→设备）

主机下发配置更新。**设备必须订阅此 Topic。**

```json
{
    "msg_id": "d4e5f6a7-b8c9-4012-defg-012345678902",
    "dev_name": "新设备名称",
    "location": "新位置",
    "user_param": "新的用户参数"
}
```

一次可下发任意组合的字段（不传的字段表示不修改）。

### device/{uuid}/resp/m2s — 主机回复（主机→设备）

主机对设备请求的回复。**设备必须订阅此 Topic。**

```json
{
    "msg_id": "e5f6a7b8-c9d0-4123-efgh-012345678903",
    "resp": 0
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msg_id` | string(UUID) | 是 | 对应请求的 msg_id |
| `resp` | int | 是 | 响应码，`0` = 成功 |

### device/{uuid}/resp/s2m — 设备回复（设备→主机）

设备对主机请求的回复。

```json
{
    "msg_id": "f6a7b8c9-d0e1-4234-fghi-012345678904",
    "resp": 0
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msg_id` | string(UUID) | 是 | 对应请求的 msg_id |
| `resp` | int | 是 | 响应码，`0` = 成功 |

### broadcast/llm/trans — 提交规则生成提示词

设备向主机提交用户的自然语言提示词，请求 AI 生成自动化规则。

```json
{
    "msg_id": "a7b8c9d0-e1f2-4345-ghij-012345678905",
    "prompt": "当温度超过30度时打开风扇",
    "timestamp": "2026-06-24T14:30:00+08:00"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msg_id` | string(UUID) | 是 | 消息 ID |
| `prompt` | string | 是 | 自然语言规则描述 |
| `timestamp` | string | 是 | ISO 8601 格式时间戳 |

主机不通过 MQTT 回复此请求，而是通过扬声器播报或通知 APP 规则更新。

---

## 通信时序示例

### 设备上线 → 数据上报 → 事件触发 → 动作执行

```
设备                               主机
 │                                  │
 │── broadcast/online ────────────→│  上线 + 属性上报
 │                                  │
 │←─ device/{uuid}/resp/m2s ──────│  注册成功 (resp=0)
 │                                  │
 │── device/{uuid}/heartbeat ────→│  周期性心跳
 │                                  │
 │── device/{uuid}/data/temp ────→│  数据上报
 │                                  │
 │── device/{uuid}/event/alert ──→│  事件上报
 │                                  │
 │←─ device/{uuid}/action/switch ─│  主机下发动作
 │                                  │
 │── device/{uuid}/resp/s2m ─────→│  动作执行结果
 │                                  │
```

---

## 注意事项

1. **msg_id 唯一性**：每次发送新消息必须生成新的 `msg_id`（UUID 格式），回复消息时使用对应请求的 `msg_id`。
2. **QoS 建议**：心跳和周期性数据使用 QoS 0，事件和动作相关消息使用 QoS 1。
3. **重连策略**：设备断开后应自动重连，并在重连成功后立即发送 `broadcast/online`。
4. **心跳超时**：主机 30 秒无心跳将设备标记为 `offline`，建议心跳间隔 10~15 秒。
5. **数据值格式**：`device/{uuid}/data/{type}` 中的 `data_val` 统一使用字符串类型，主机侧根据设备声明的 `type` 进行转换。
