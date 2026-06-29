# Cron Virtual Device

CortexLink 定时任务虚拟设备，替代 C++ 内置的 `CronScheduler`。

## 概述

Cron 设备以独立 Python 进程运行，通过 MQTT 与 CortexLink 主机通信。提供与旧版 C++ 实现完全兼容的接口：
创建、删除、查看 cron 定时任务；cron 表达式匹配时触发 `cron_trigger` 事件，由规则引擎执行对应 Lua 脚本。

## 设备属性

| 属性 | 值 |
|------|-----|
| UUID | `00000000-0000-0000-0000-000000000001` |
| dev_type | `composite` |
| dev_name | `Cron Timer` |
| 事件 UUID | `00000000-0000-0000-0000-000000000002` (`cron_trigger`) |

## 快速开始

```bash
cd devices/cron
./start_cron.sh              # 默认配置
./start_cron.sh --debug      # 调试模式
./start_cron.sh --config /path/to/config.json
```

## 配置

默认配置文件：`config.json`（首次运行自动生成）。

```json
{
    "mqtt": {
        "host": "localhost",
        "port": 1883,
        "username": "",
        "password": ""
    },
    "device": {
        "uuid": "00000000-0000-0000-0000-000000000001",
        "name": "Cron Timer"
    }
}
```

环境变量覆盖：`MQTT_HOST`、`MQTT_PORT`、`MQTT_USERNAME`、`MQTT_PASSWORD`。

## 依赖

- Python 3.9+
- paho-mqtt >= 2.0.0

## 架构

```
Lua 脚本
  │ do_action("0000...0001", "add_cron", {...})
  ▼
MQTT: device/0000...0001/action/add_cron
  │
  ▼
CronDevice._handle_action()
  ├─ 解析 action params
  ├─ 写入内存 entries
  ├─ 持久化到 ~/.cortexlink/cron/crontab.txt
  └─ 回复 s2m response

Scheduler 线程（每 30 秒）
  ├─ 获取当前 UTC+8 时间
  ├─ 遍历 entries 检查 cron 表达式匹配
  ├─ 匹配成功 → MQTT publish device/0000...0001/event/cron_trigger
  ├─ 递减 trigger_count（如 > 0）
  └─ trigger_count == 0 → 自动删除

规则引擎（C++ 主机）
  ├─ device/+/event/# 订阅收到 cron_trigger
  ├─ 查找绑定到 cron_trigger 的规则
  ├─ 评估条件表达式
  └─ 执行 Lua 脚本
```

## crontab.txt 持久化

路径：`~/.cortexlink/cron/crontab.txt`

格式（`|` 分隔）：
```
<uuid>|<cron_expr>|<params_json>|<trigger_count>
```

## 与 C++ 版的区别

| 项目 | C++ 版 | Python 版 |
|------|--------|-----------|
| 运行方式 | 主机进程内线程 | 独立进程 |
| 事件注入 | `RuleEngine::InjectEvent()` 直接调用 | MQTT publish 到 event topic |
| DB 注册 | 直接 SQLite Upsert | MQTT broadcast/online → DeviceManager |
| 启动方式 | 主机启动时自动 | 手动运行 start_cron.sh |

功能行为完全兼容，Lua 脚本无需任何修改。
