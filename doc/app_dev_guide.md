# CortexLink APP 开发指南

## 概述

APP 通过 MQTT 协议与 CortexLink 主机通信，可进行以下操作：
- **SQL 查询**：查询设备、规则、用户等信息
- **规则生成**：提交自然语言提示词，由主机调用 LLM 自动生成规则
- **设备配置**：修改设备名称、位置等配置
- **文件上传**：上传人脸、声纹等生物特征数据
- **事件监听**：订阅设备事件通知

---

## MQTT 连接

| 项目 | 说明 |
|------|------|
| Broker | 远程 Mosquitto broker |
| 认证 | 用户名 + 密码（嵌入 APP 中） |
| 消息 ID | UUID 格式，以当前 UNIX 时间戳作为种子生成 |
| 编码 | JSON（UTF-8） |

---

## APP 相关 Topic 总览

| Topic | 方向 | 说明 |
|-------|------|------|
| `app/sql/trans` | APP→主机 | 数据库操作请求（cmd 驱动） |
| `app/sql/resp` | 主机→APP | 数据库查询回复 |
| `app/llm/trans` | APP→主机 | 提交规则生成提示词 |
| `app/llm/resp` | 主机→APP | 规则生成结果回复 |
| `app/face/trans` | APP→主机 | 人脸上传 |
| `app/face/resp` | 主机→APP | 人脸上传回复 |
| `app/voice/trans` | APP→主机 | 声纹上传 |
| `app/voice/resp` | 主机→APP | 声纹上传回复 |
| `broadcast/config` | APP→主机 | 设备配置更改 |

---

## 一、SQL 数据库查询

### 请求：`app/sql/trans`

APP 通过 `cmd` 操作码发起查询，主机侧使用策略模式将 `cmd` 映射到预定义的 SQL 模板。**APP 不直接发送 SQL 语句**，从架构上杜绝 SQL 注入。

```json
{
    "msg_id": "<uuid>",
    "cmd": "<操作码>",
    "params": {},
    "timestamp": ""
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `msg_id` | string | 消息唯一标识（UUID 格式） |
| `cmd` | string | 操作码 |
| `params` | object | 命名参数，键值对 |
| `timestamp` | string | 发送时间戳 |

### 支持的操作码

| cmd | 说明 | params |
|-----|------|--------|
| `get_device_list` | 获取所有设备列表 | 无 `{}` |
| `get_device_detail` | 获取单个设备详情 | `{"dev_id": "<uuid>"}` |
| `get_device_data` | 获取设备当前数据 | `{"dev_id": "<uuid>"}` |
| `get_rules` | 获取所有规则列表 | 无 `{}` |
| `get_rule_detail` | 获取单条规则详情 | `{"rule_id": <int>}` |
| `get_user_profiles` | 获取所有用户信息 | 无 `{}` |

### 响应：`app/sql/resp`

```json
{
    "msg_id": "<对应请求的 msg_id>",
    "resp": 0,
    "rows": [],
    "timestamp": ""
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `msg_id` | string | 对应请求的 msg_id |
| `resp` | int | 响应码：`0` = 成功 |
| `rows` | array | 查询结果，每行为一个 JSON 对象（列名→值） |
| `timestamp` | string | 响应时间戳 |

### 查询结果示例

**get_device_list 返回：**
```json
{
    "msg_id": "a1b2c3d4-...",
    "resp": 0,
    "rows": [
        {
            "dev_id": "d4e5f6...",
            "dev_name": "客厅温度传感器",
            "dev_type": "sensor",
            "dev_subtype": "temperature",
            "dev_state": "online",
            "location": "客厅"
        }
    ],
    "timestamp": ""
}
```

**get_rule_detail 返回：**
```json
{
    "msg_id": "a1b2c3d4-...",
    "resp": 0,
    "rows": [
        {
            "rule_id": 1,
            "rule_name": "高温开风扇",
            "rule_type": "automation",
            "enable": 1,
            "count": 5,
            "limit": null,
            "cond_expr": "a1b2c3d4...({event.temperature} > 35)",
            "action": "rule_1_high_temp.lua"
        }
    ],
    "timestamp": ""
}
```

### SQL 响应错误码

| 值 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 请求 JSON 解析失败 |
| 2 | 必填字段缺失（如缺少 `cmd` 或 `msg_id`） |
| 3 | 未知 cmd（未注册的操作码） |
| 4 | params 参数不合法（如缺少必填参数） |
| 5 | SQL 执行错误 |

---

## 二、LLM 规则生成

### 请求：`app/llm/trans`

APP 向主机提交自然语言提示词，主机将其转发给 MCP Server，由 MCP Server 调用 LLM 生成规则和 Lua 脚本。

```json
{
    "msg_id": "<uuid>",
    "prompt": "<自然语言规则描述>",
    "timestamp": ""
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `msg_id` | string | 消息唯一标识（UUID 格式） |
| `prompt` | string | 用户输入的自然语言提示词，描述期望的自动化规则 |
| `timestamp` | string | 发送时间戳 |

**示例：**
```json
{
    "msg_id": "f1e2d3c4-...",
    "prompt": "温度超过 35 度时开风扇，低于 30 度时关风扇",
    "timestamp": ""
}
```

### 响应：`app/llm/resp`

主机完成规则生成后，通过此 topic 回复结果。

```json
{
    "msg_id": "<对应请求的 msg_id>",
    "resp": 0,
    "timestamp": ""
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `msg_id` | string | 对应请求的 msg_id |
| `resp` | int | 响应码（见下方错误码表） |
| `timestamp` | string | 响应时间戳 |

**成功示例：**
```json
{
    "msg_id": "f1e2d3c4-...",
    "resp": 0,
    "timestamp": ""
}
```

**失败示例：**
```json
{
    "msg_id": "f1e2d3c4-...",
    "resp": 3,
    "timestamp": ""
}
```

### LLM 响应错误码

| 值 | 含义 | 说明 |
|----|------|------|
| 0 | 规则生成成功 | 规则已写入数据库，Lua 脚本已就位 |
| 1 | 请求 JSON 解析失败 | `msg_id` 或 `prompt` 格式错误 |
| 2 | 必填字段缺失 | 缺少 `prompt` 字段 |
| 3 | LLM 服务不可用 | OpenClaw 服务无法连接 |
| 4 | 规则生成失败 | LLM 返回的结果无效/不合法 |
| 5 | 数据库写入失败 | 规则写入 SQLite 失败 |
| 6 | Lua 脚本写入失败 | 脚本文件写入文件系统失败 |

### 规则生成流程

```
APP → app/llm/trans → 主机
                          ↓
                     主机转发到 MCP Server
                          ↓
           MCP Server 调用 LLM 生成规则
           - 查询 device_property / event（了解可用设备）
           - 生成 rule（insert_rule）
           - 生成 event_rule 映射
           - 写入 Lua 脚本到 ~/.cortexlink/scripts/
                          ↓
                     规则就绪，RuleEngine 可正常执行
                          ↓
                app/llm/resp ← 主机回复结果
```

> **注意**：规则生成是异步操作，APP 发送请求后应等待 `app/llm/resp` 响应，不要阻塞 UI。生成时间取决于 LLM 响应速度，通常 2~10 秒。

### 设备侧提示词提交（APP 无需关注）

设备可将用户提示词集成在事件上报中（`device/{uuid}/event/{type}`），通过事件参数 `prompt` 字段携带。此路径与 APP 无关，APP 仅使用 `app/llm/trans` 和 `app/llm/resp` 即可。

---

## 三、设备配置更改

### 请求：`broadcast/config`

APP 可修改设备的名称、位置或用户参数。

```json
{
    "msg_id": "<uuid>",
    "dev_id": "<设备 UUID>",
    "dev_name": "<新名称>"
}
```

可选的配置字段（至少提供一个）：
| 字段 | 说明 |
|------|------|
| `dev_name` | 设备显示名称 |
| `location` | 设备位置 |
| `user_param` | 用户自定义参数（辅助 LLM 理解设备意图） |

**示例 — 修改设备名称和位置：**
```json
{
    "msg_id": "a1b2c3d4-...",
    "dev_id": "d4e5f6...",
    "dev_name": "卧室温度传感器",
    "location": "主卧"
}
```

> **注意**：当前版本 `broadcast/config` 无应答 topic，APP 需通过 `get_device_detail` 查询确认更改是否生效。

---

## 四、文件上传（人脸 / 声纹）

### 请求：`app/face/trans` 或 `app/voice/trans`

两种上传使用相同的载荷格式，主机根据 topic 决定文件存放位置。

```json
{
    "frag_id": "<当前分片序号>",
    "total_frags": "<总分片数>",
    "checksum": "<文件 MD5>",
    "file_name": "<文件名>",
    "data": "<Base64 编码的分片数据>"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `frag_id` | string | 当前分片序号（从 1 开始） |
| `total_frags` | string | 总分片数 |
| `checksum` | string | 完整文件的 MD5 校验和 |
| `file_name` | string | 文件名（含扩展名） |
| `data` | string | 当前分片的 Base64 编码数据 |

### 响应：`app/face/resp` 或 `app/voice/resp`

```json
{
    "frag_id": "<对应分片序号>",
    "resp": 0
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `frag_id` | string | 对应分片的序号 |
| `resp` | int | `0` = 该分片接收成功，非 0 = 失败 |

### 上传流程

1. 将文件分割为分片（建议每片 ≤ 64KB Base64 编码前）。
2. 按顺序发送每个分片，`frag_id` 从 1 递增。
3. 每发送一个分片，等待对应的 `resp` 回复 `resp=0` 后再发下一片。
4. 收到最后一片的 `resp=0` 后上传完成。

---

## 五、错误处理建议

1. **超时重试**：SQL 查询和规则生成请求建议设置 10s 超时，超时后重试一次。
2. **重连机制**：MQTT 断线后自动重连，重连后重新订阅 `app/sql/resp`、`app/llm/resp`、`app/face/resp`、`app/voice/resp`。
3. **msg_id 匹配**：收到响应后通过 `msg_id` 匹配原始请求，区分不同请求的回复。
4. **规则生成幂等**：如果 `app/llm/trans` 发送后超时未收到 `app/llm/resp`，重试前应提示用户确认（避免重复生成相同规则）。

---

## 六、通用约定

- UUID 在 JSON 中使用标准 UUID 字符串格式（如 `a1b2c3d4-e5f6-7890-abcd-ef1234567890`）。
- 所有 `msg_id` 由发送方生成，响应中回传以匹配请求。
