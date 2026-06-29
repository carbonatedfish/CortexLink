# 设备上线流程

## 概述

设备通过 MQTT 连接到 CortexLink 主机，需要完成注册流程后才能正常通信。本文档描述设备从首次连接到正常运行的完整流程。

---

## 上线流程图

```
设备启动
  │
  ▼
连接 MQTT Broker ──────────── 使用固件中预设的凭证
  │
  ▼
订阅系统 Topic ────────────── device/{uuid}/action/#
  │                            device/{uuid}/config
  │                            device/{uuid}/resp/m2s
  ▼
发送 broadcast/online ─────── 上报设备属性
  │
  ▼
等待 resp/m2s 回复
  │
  ├─ resp = 0 (OK) ────────── 注册成功，开始正常通信
  │
  └─ resp ≠ 0 或超时 ──────── 重试（建议指数退避）
```

---

## 详细步骤

### 步骤 1：连接 MQTT Broker

设备启动后使用固件中预设的凭证连接远程 Mosquitto broker：

- **Client ID**：建议使用设备 UUID，确保唯一性
- **Clean Session**：建议 `false`（持久会话），避免离线期间丢失消息
- **Keep Alive**：建议 30~60 秒

### 步骤 2：订阅 Topic

连接成功后立即订阅以下 Topic：

| Topic | QoS | 说明 |
|-------|-----|------|
| `device/{uuid}/action/#` | 1 | 动作下发（通配订阅所有 action 类型） |
| `device/{uuid}/config` | 1 | 配置更改 |
| `device/{uuid}/resp/m2s` | 1 | 主机回复 |

> `{uuid}` 替换为设备的实际 UUID。

### 步骤 3：上报设备属性

向 `broadcast/online` 发送设备属性 JSON。这是设备注册的关键步骤。

```json
{
    "dev_id": "a1b2c3d4-e5f6-4789-abcd-ef0123456789",
    "dev_name": "客厅温度传感器",
    "dev_type": "sensor",
    "dev_subtype": "temperature",
    "location": "客厅",
    "user_param": "用于监测客厅环境温度，超过30度触发降温",
    "actions": [
        {
            "act_id": "act-reboot",
            "act_name": "reboot",
            "desc": "重启设备",
            "params": [],
            "pre_cond": ""
        }
    ],
    "event": [
        {
            "evt_id": "evt-temp-high",
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
        },
        {
            "evt_id": "evt-temp-low",
            "evt_name": "temp_low",
            "desc": "温度低于阈值",
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

#### 字段填写指南

**dev_type：**

| 值 | 说明 | 典型设备 |
|----|------|----------|
| `sensor` | 纯传感器，仅上报数据/事件 | 温度计、门磁、烟雾报警器 |
| `actuator` | 纯执行器，仅接收并执行动作 | 开关、电机、灯 |
| `composite` | 复合设备，既有传感器又有执行器 | 智能空调、智能窗帘 |

**user_param：**
- 用自然语言描述设备的用途和意图
- 供 LLM 生成规则时理解设备上下文
- 示例："用于监测客厅环境温度，超过30度时用户希望降温"

**actions 中每条动作：**
- `act_id`：动作唯一标识（固件中硬编码，与代码中处理逻辑一一对应）
- `act_name`：动作名称
- `desc`：动作描述
- `params`：动作需要的参数列表
  - `p_name`：参数名
  - `desc`：参数说明
  - `p_type`：`int` / `float` / `str`
  - `range`：`[min, max]`（数值类型可选）
  - `unit`：单位（可选）
- `pre_cond`：前置条件说明（可选）

**event 中每条事件：**
- `evt_id`：事件唯一标识（固件中硬编码，上报事件时引用）
- `evt_name`：事件名称
- `desc`：事件描述
- `params`：事件携带的参数列表（格式同 action params）

**data 中每条数据：**
- `d_name`：数据名称（固件中硬编码，上报数据时引用）
- `desc`：数据说明
- `type`：`int` / `float` / `str`
- `unit`：单位（可选）

### 步骤 4：等待注册确认

设备发送 `broadcast/online` 后，等待主机在 `device/{uuid}/resp/m2s` 上的回复：

- **`resp = 0`**：注册成功，设备可开始正常通信（发送心跳、上报数据/事件）
- **`resp = 3` (INTERNAL_ERROR)**：注册失败，主机内部错误，建议重试
- **超时（如 10 秒无回复）**：网络异常或主机未运行，建议重试

**重试策略建议：**
- 首次重试：等待 2 秒
- 二次重试：等待 4 秒
- 三次及以后：等待 8 秒
- 最大重试次数：建议至少 5 次

### 步骤 5：正常通信

注册成功后进入正常运行状态：

1. **心跳** — 每 10~15 秒向 `device/{uuid}/heartbeat` 发送心跳
2. **数据上报** — 按需或周期性向 `device/{uuid}/data/{type}` 上报数据
3. **事件上报** — 检测到事件时向 `device/{uuid}/event/{type}` 上报
4. **响应动作** — 收到 `device/{uuid}/action/{type}` 后执行并回复
5. **响应配置** — 收到 `device/{uuid}/config` 后更新配置

---

## 断线重连

设备与 MQTT broker 断开后：

1. 自动重连 broker
2. 重新订阅 Topic（如果 Clean Session = true）
3. 重新发送 `broadcast/online`（即使之前已注册）
4. 主机收到 `broadcast/online` 后，如果该设备已存在，更新其 `dev_state` 为 `online` 并更新属性

**注意：** 断线期间主机可能已将设备标记为 `offline`，此期间下发的动作将失败。重连后设备应立即上报心跳恢复在线状态。

---

## 设备 UUID 说明

- 设备 UUID 在出厂时写入固件，全局唯一。
- 在 MQTT Topic 中使用标准 UUID 字符串格式（小写，带连字符）：`a1b2c3d4-e5f6-4789-abcd-ef0123456789`
- 在 Lua 脚本中引用设备时同样使用此格式字符串。

---

## 检查清单

设备端开发完成后，确认以下事项：

- [ ] MQTT 连接参数（broker 地址、用户名、密码）已正确配置
- [ ] 设备 UUID 已写入固件且全局唯一
- [ ] `broadcast/online` 载荷中的 `actions`/`event`/`data` 定义完整准确
- [ ] 设备订阅了所有必需的 Topic
- [ ] 心跳定时器已启动（建议 10~15s 间隔）
- [ ] 动作处理逻辑与 `act_id` 一一对应
- [ ] 事件上报使用正确的 `evt_id`
- [ ] 数据上报使用正确的 `d_name`
- [ ] 断线重连逻辑已实现
- [ ] 收到 `resp/m2s` 后正确解析 `resp` 字段
