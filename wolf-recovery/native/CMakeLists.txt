cmake_minimum_required(VERSION 3.20)
project(wolf_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# cmake-js injects these variables
include_directories(${CMAKE_JS_INC})

# node-addon-api headers
execute_process(
  COMMAND node -p "require('node-addon-api').include"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
  OUTPUT_VARIABLE NODE_ADDON_API_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "\"" "" NODE_ADDON_API_DIR ${NODE_ADDON_API_DIR})
include_directories(${NODE_ADDON_API_DIR})

# Define NAPI_VERSION
add_definitions(-DNAPI_VERSION=8)

# Source files
set(SQLITE_SOURCES third_party/sqlite3.c)
set_source_files_properties(${SQLITE_SOURCES} PROPERTIES LANGUAGE C)

set(SOURCES
  src/bridge/napi_bridge.cpp
  src/wolf_engine.cpp
  src/io/disk_reader_win.cpp
  src/db/metadata_store.cpp
  src/fs/ntfs_parser.cpp
    src/fs/fat_parser.cpp
    src/fs/ext4_parser.cpp
    src/fs/exfat_parser.cpp
    src/carver/signature_engine.cpp
    src/scan_coordinator.cpp
  ${SQLITE_SOURCES}
)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party)

# Build the addon
add_library(${PROJECT_NAME} SHARED ${SOURCES} ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")
target_link_libraries(${PROJECT_NAME} ${CMAKE_JS_LIB})

# Windows-specific libraries
if(WIN32)
  target_link_libraries(${PROJECT_NAME} setupapi)
endif()




