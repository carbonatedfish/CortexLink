cmake_minimum_required(VERSION 3.12)
project(CortexLink)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

#config mosquitto
find_library(MOSQUITTOPP_LIB mosquittopp REQUIRED)
find_path(MOSQUITTOPP_INCLUDE_DIR mosquittopp.h REQUIRED)
#end

add_executable(CortexLink
    src/main.cpp
    src/mqtt/mqtt_client.cpp
)
target_include_directories(CortexLink PRIVATE include include/3rdparty ${MOSQUITTOPP_INCLUDE_DIR})
target_link_libraries(CortexLink PRIVATE ${MOSQUITTOPP_LIB} Threads::Threads SQLite::SQLite3)