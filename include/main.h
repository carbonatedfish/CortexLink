#pragma once

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <cxxopts/cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "app/app_manager.h"
#include "app/app_sql_proxy.h"
#include "db/db_table.h"
#include "llm/llm_sql_proxy.h"
#include "llm/open_claw_client.h"
#include "db/device_data_table.h"
#include "db/device_property_table.h"
#include "db/event_record_table.h"
#include "db/event_rule_table.h"
#include "db/event_table.h"
#include "db/rule_table.h"
#include "db/user_profile_table.h"
#include "device/device_manager.h"
#include "mqtt/mqtt_client.h"
#include "rule_engine/rule_engine.h"
#include "util/log_util.h"