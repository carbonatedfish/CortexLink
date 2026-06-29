# Cron 抽象设备

## 概述

Cron 定时器是 CortexLink 主机内部维护的**虚拟设备**，遵循与其他物理设备完全相同的 MQTT 接口规范。用户可通过 Lua 脚本调用 `do_action` 来创建、删除、查看定时任务。当 cron 表达式匹配时，Cron 设备触发 `cron_trigger` 事件，经规则引擎执行对应的 Lua 脚本。

## 设备属性

| 属性 | 值 |
|------|-----|
| UUID | `00000000-0000-0000-0000-000000000001` |
| dev_type | `composite` |
| dev_name | `Cron Timer` |
| dev_state | `online`（主机运行时始终在线） |

## 事件

### cron_trigger

cron 任务到期时触发。

| 参数 | 类型 | 说明 |
|------|------|------|
| `cron_id` | string | 触发任务的 UUID |
| `expr` | string | 匹配的 cron 表达式 |
| `user_id` | string | 可选，用户 ID |

此外，创建任务时传入的自定义 `params` 中的所有键值对也会作为事件参数注入。

## 动作

所有动作通过 `do_action(dev_uuid, act_id, params_table)` 调用，dev_uuid 固定为 `"00000000-0000-0000-0000-000000000001"`。

响应通过 `device/{dev_uuid}/resp/s2m` topic 返回（设备→主机方向），格式：
```json
{"msg_id": "...", "resp": 0, "data": {...}}
```

### add_cron — 添加 cron 任务

| 参数 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `expr` | string | 是 | — | 标准 5 字段 cron 表达式 |
| `params` | table | 否 | `{}` | 触发时注入事件的自定义参数 |
| `trigger_count` | int | 否 | `-1` | 触发次数限制，`-1` 表示无限 |

**响应 data：** `{"cron_id": "<uuid>"}`

### add_relative_cron — 添加相对时间 cron 任务

| 参数 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `offset` | string | 是 | — | 时间偏移量，如 `"30m"` `"2h"` `"1d"` `"1h30m"` |
| `params` | table | 否 | `{}` | 触发时注入事件的自定义参数 |
| `trigger_count` | int | 否 | `1` | 触发次数限制，默认 1（一次性） |

程序内部计算 `当前时间 + offset`，生成绝对 cron 表达式后存储。

**偏移量格式：**
- `s` — 秒，`m` — 分钟，`h` — 小时，`d` — 天
- 支持组合：`"1h30m"` `"2d6h"` `"90s"`
- 单位可不按顺序、可重复

**响应 data：** `{"cron_id": "<uuid>", "expr": "<计算得到的cron表达式>"}`

### remove_cron — 删除 cron 任务

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `cron_id` | string | 是 | 任务 UUID（由 add_cron / add_relative_cron 返回） |

幂等操作 — 删除不存在的任务也返回 OK。

### list_crons — 列出所有 cron 任务

无参数。

**响应 data：**
```json
{
    "jobs": [
        {
            "cron_id": "a1b2c3d4-...",
            "expr": "0 7 * * *",
            "params": {"reminder": "早安提醒"},
            "trigger_count": 5
        }
    ]
}
```

---

## Cron 表达式格式

标准 5 字段格式：

```
字段:    minute   hour   day-of-month   month   day-of-week
范围:    0-59     0-23   1-31           1-12    0-6 (0=Sunday)
```

**支持语法：**

| 语法 | 示例 | 说明 |
|------|------|------|
| `*` | `* * * * *` | 每分钟 |
| 具体值 | `0 7 * * *` | 每天 7:00 |
| 范围 | `0 9-17 * * *` | 每天 9:00-17:00 整点 |
| 步长 | `*/5 * * * *` | 每 5 分钟 |
| 列表 | `0 7,12,18 * * *` | 每天 7:00, 12:00, 18:00 |
| 组合 | `0 9-17/2 * * 1-5` | 工作日 9:00-17:00 每 2 小时 |

**常见示例：**

| 表达式 | 含义 |
|--------|------|
| `0 7 * * *` | 每天早上 7:00 |
| `*/30 * * * *` | 每 30 分钟 |
| `0 8 * * 1-5` | 工作日早上 8:00 |
| `0 0 1 * *` | 每月 1 号 0:00 |
| `30 14 24 6 *` | 6 月 24 日 14:30（一次性，使用绝对时间） |

---

## 触发次数限制

每个 cron 任务有一个 `trigger_count` 字段：

| 值 | 行为 |
|----|------|
| `-1` | 无限次触发 |
| `N (>0)` | 每次触发后减 1，减到 0 后**自动删除该任务** |

触发次数在每次 cron 表达式匹配并成功注入事件**之后**递减。

---

## Lua 使用示例

### 每天早上 7:00 早安提醒（无限次）

```lua
do_action("00000000-0000-0000-0000-000000000001", "add_cron", {
    expr = "0 7 * * *",
    params = {
        reminder = "早安提醒",
        action = "send_notification"
    }
})
```

### 30 分钟后一次性提醒

```lua
do_action("00000000-0000-0000-0000-000000000001", "add_relative_cron", {
    offset = "30m",
    params = {
        reminder = "30分钟到了"
    }
})
```

### 每 5 分钟检查温度，仅前 10 次

```lua
do_action("00000000-0000-0000-0000-000000000001", "add_cron", {
    expr = "*/5 * * * *",
    params = {
        check = "temperature"
    },
    trigger_count = 10
})
```

### 列出所有任务并删除指定任务

```lua
-- 列出所有 cron 任务（结果通过 resp/s2m topic 返回）
do_action("00000000-0000-0000-0000-000000000001", "list_crons", {})

-- 删除指定任务
do_action("00000000-0000-0000-0000-000000000001", "remove_cron", {
    cron_id = "a1b2c3d4-e5f6-4789-abcd-ef0123456789"
})
```

### 配合规则引擎使用

创建规则绑定 `cron_trigger` 事件，在条件表达式中可用 `{event.cron_id}` 区分不同任务：

规则条件表达式：
```
00000000-0000-0000-0000-000000000002({event.cron_id} == "a1b2c3d4-...")
```

Lua 脚本（规则 action）：
```lua
-- 根据事件参数执行不同逻辑
if event.params.action == "send_notification" then
    log("INFO", "Notification: " .. event.params.reminder)
end
```

---

## 内部架构

```
Lua 脚本
  │ do_action("0000...0001", "add_cron", {...})
  ▼
MQTT: device/0000...0001/action/add_cron
  │
  ▼
CronScheduler::OnAction()
  ├─ 解析 action params
  ├─ 写入内存 entries_
  ├─ 持久化到 ~/.cortexlink/cron/crontab.txt
  ├─ cv_.notify_one() 唤醒 scheduler 线程
  └─ 回复 M2S response

Scheduler 线程（每 30 秒）
  ├─ 获取当前 UTC+8 时间
  ├─ 遍历 entries_ 检查 cron 表达式匹配
  ├─ 匹配成功 → rule_engine_->InjectEvent(cron_trigger)
  ├─ 递减 trigger_count（如 > 0）
  └─ trigger_count == 0 → 自动删除

规则引擎
  ├─ 查找绑定到 cron_trigger 的规则
  ├─ 评估条件表达式
  └─ 执行 Lua 脚本
```

---

## crontab.txt 持久化

路径：`~/.cortexlink/cron/crontab.txt`

格式（`|` 分隔）：
```
<uuid>|<cron_expr>|<params_json>|<trigger_count>
```

示例：
```
a1b2c3d4-e5f6-4789-abcd-ef0123456789|0 7 * * *|{"reminder":"早安提醒"}|-1
b2c3d4e5-f6a7-4890-bcde-f01234567890|*/30 * * * *|{"check":"temp"}|10
c3d4e5f6-a7b8-4901-cdef-012345678901|30 14 24 6 *|{"reminder":"一次性"}|1
```

每次变更后全量写回文件。

---

## 错误码参考

| 响应码 | 含义 | 触发场景 |
|--------|------|----------|
| `0` | 成功 | 操作正常完成 |
| `4` | 请求无效 | action payload JSON 解析失败 |
| `5` | 缺少字段 | add_cron 缺少 `expr`、add_relative_cron 缺少 `offset`、remove_cron 缺少 `cron_id` |
| `20` | 动作未识别 | act_id 不是 add_cron / add_relative_cron / remove_cron / list_crons |
| `21` | 参数无效 | cron 表达式语法错误、offset 格式错误 |
| `3` | 内部错误 | crontab.txt 读写失败 |

---

## 技术说明

| 项目 | 说明 |
|------|------|
| 时区 | 固定 UTC+8（Asia/Shanghai），使用系统 `localtime` |
| 调度粒度 | 每 30 秒检查一次 cron 匹配；新增任务时立即唤醒 |
| 去重策略 | 同一分钟每个 job 最多触发一次（按 unix_minute 去重） |
| 并发安全 | `std::mutex` 保护 entries_；DB 操作有 `DBTable` 基类 mutex |
| 脚本超时 | 与普通 Lua 脚本一致：3s 超时，重试 1 次 |
