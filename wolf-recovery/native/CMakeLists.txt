cmake_minimum_required(VERSION 3.20)
project(wolf_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export compile_commands.json so clang-tidy / IDE tooling can resolve flags.
# Configure with: cmake -S native -B native/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
set_source_files_properties(${SQLITE_SOURCES} PROPERTIES COMPILE_DEFINITIONS "SQLITE_ENABLE_FTS5")

set(SOURCES
  src/bridge/napi_bridge.cpp
  src/bridge/bridge_drives.cpp
  src/bridge/bridge_scan.cpp
  src/bridge/bridge_imager.cpp
  src/bridge/bridge_wipe.cpp
  src/bridge/bridge_ops.cpp
  src/wolf_engine.cpp
  src/io/byte_source.cpp
  src/imager/ewf_writer.cpp
  src/imager/ewf_reader.cpp
  src/carver/content_classifier.cpp
  src/db/metadata_store.cpp
  src/db/metadata_store_case.cpp
  src/db/metadata_store_content.cpp
  src/db/runs_codec.cpp
  src/fs/ntfs_parser.cpp
    src/fs/fat_parser.cpp
    src/fs/ext4_parser.cpp
    src/fs/hfs_parser.cpp
    src/fs/apfs_parser.cpp
    src/fs/apfs_container.cpp
    src/fs/refs_parser.cpp
    src/fs/refs_integrity.cpp
    src/fs/hfs_catalog.cpp
    src/fs/ntfs_logfile.cpp
    src/fs/vss_scanner.cpp
    src/fs/volume_identity.cpp
    src/fs/bitlocker_fve.cpp
    src/fs/bitlocker_unlock.cpp
    src/search/content_search.cpp
    src/fs/partition_scanner.cpp
    src/fs/unallocated_map.cpp
    src/fs/ntfs_path_rebuild.cpp
    src/fs/virtual_raid.cpp
    src/fs/raid6_math.cpp
    src/fs/fat_chain.cpp
    src/fs/raid_layout.cpp
    src/imager/disk_imager.cpp
    src/crypto/md5.cpp
    src/crypto/aes_xts.cpp
    src/crypto/aes_ccm.cpp
    src/crypto/sha256.cpp
    src/carver/signature_engine.cpp
    src/carver/bgc.cpp
    src/carver/parsers/zip_family.cpp
    src/carver/parsers/sqlite_carver.cpp
    src/carver/parsers/mp4_carver.cpp
    src/math/entropy.cpp
    src/smart/smart_monitor.cpp
    src/memory/memory_pool.cpp
    src/scan_coordinator.cpp
    src/scan/dedup_index.cpp
    src/forensic/audit_logger.cpp
    src/forensic/nsrl_lookup.cpp
  src/security/data_shredder.cpp
  src/recovery/recovery_engine.cpp
  src/recovery/path_util.cpp
  src/recovery/validation.cpp
  src/recovery/preview_reader.cpp
  ${SQLITE_SOURCES}
)

if(WIN32)
  list(APPEND SOURCES src/io/disk_reader_win.cpp src/io/volume_mapper_win.cpp)
else()
  list(APPEND SOURCES src/io/disk_reader_posix.cpp)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party)

# Build the addon
add_library(${PROJECT_NAME} SHARED ${SOURCES} ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")
target_link_libraries(${PROJECT_NAME} ${CMAKE_JS_LIB})

# Windows-specific libraries
if(WIN32)
  target_link_libraries(${PROJECT_NAME} setupapi winhttp)
endif()

# ----------------------------------------------------------------------------
# Native unit tests (GoogleTest)
#
# Gated behind WOLF_BUILD_TESTS so production builds and cmake-js (which does
# not pass this flag) never download or compile the test tree. Enable locally
# with:
#   cmake -S native -B native/build -DWOLF_BUILD_TESTS=ON
#   cmake --build native/build --config Release --target wolf_tests
#   ctest --test-dir native/build --output-on-failure
# ----------------------------------------------------------------------------
option(WOLF_BUILD_TESTS "Build the native GoogleTest suite" OFF)
if(WOLF_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  # On Windows prevent the shared CRT linkage mismatch between gtest and the app.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(wolf_tests
    tests/test_entropy.cpp
    tests/test_ntfs_util.cpp
    tests/test_lznt1.cpp
    tests/test_usn.cpp
    tests/test_validators.cpp
    tests/test_bgc.cpp
    tests/test_preview_reader.cpp
    tests/test_raid6_math.cpp
    tests/test_ewf.cpp
    tests/test_ewf_reader.cpp
    tests/test_content_classifier.cpp
    tests/test_audit_sha256.cpp
    tests/test_fat_chain.cpp
    tests/test_raid_layout.cpp
    tests/test_runs_codec.cpp
    tests/test_metadata_store.cpp
    tests/test_partition_probe.cpp
    tests/test_recovery_engine.cpp
    tests/test_path_util.cpp
    tests/test_disk_reader_mem.cpp
    tests/test_fat_parser.cpp
    tests/test_ext4_parser.cpp
    tests/test_ntfs_parser.cpp
    tests/test_ntfs_logfile.cpp
    tests/test_usn_summary.cpp
    tests/test_vss_scan.cpp
    tests/test_raid_scan.cpp
    tests/test_content_search.cpp
    tests/test_apple_fs.cpp
    tests/test_refs_parser.cpp
    tests/test_refs_integrity.cpp
    tests/test_scan_coordinator.cpp
    tests/test_ntfs_path_rebuild.cpp
    tests/test_volume_mapper.cpp
    tests/test_nsrl.cpp
    tests/test_case_info.cpp
    tests/test_fuzz_runs_codec.cpp
    tests/test_virtual_raid_io.cpp
    tests/test_signature_carve.cpp
    tests/test_signature_coverage.cpp
    tests/test_structural_parsers.cpp
    tests/test_carve_parallel.cpp
    tests/test_dedup_index.cpp
    tests/test_e2e_scan_recover.cpp
    tests/test_recovery_golden.cpp
    tests/bench_scan.cpp
    tests/test_ata_health.cpp
    tests/test_crypto_fve.cpp
    tests/test_shredder.cpp
    # Compile the units under test directly into the test binary. The engine
    # itself ships as a .node shared library whose symbols are not exported on
    # MSVC by default, so linking the test against the shared target would
    # leave symbols unresolved. Compiling the sources into the test gives full
    # coverage without forcing a public export surface on the addon.
    src/math/entropy.cpp
    src/carver/bgc.cpp
    src/fs/raid6_math.cpp
    src/fs/fat_chain.cpp
    src/fs/raid_layout.cpp
    src/forensic/audit_logger.cpp
    src/forensic/nsrl_lookup.cpp
    src/crypto/md5.cpp
    src/crypto/aes_xts.cpp
    src/crypto/aes_ccm.cpp
    src/crypto/sha256.cpp
    src/imager/ewf_writer.cpp
    src/imager/ewf_reader.cpp
    src/io/byte_source.cpp
    src/carver/content_classifier.cpp
    src/db/runs_codec.cpp
    src/recovery/path_util.cpp
    src/recovery/validation.cpp
    src/recovery/preview_reader.cpp
    src/recovery/recovery_engine.cpp
    src/db/metadata_store.cpp
    src/db/metadata_store_case.cpp
    src/db/metadata_store_content.cpp
    src/fs/partition_scanner.cpp
    src/fs/unallocated_map.cpp
    src/fs/ntfs_path_rebuild.cpp
    src/fs/fat_parser.cpp
    src/fs/ext4_parser.cpp
    src/fs/hfs_parser.cpp
    src/fs/apfs_parser.cpp
    src/fs/apfs_container.cpp
    src/fs/refs_parser.cpp
    src/fs/refs_integrity.cpp
    src/fs/hfs_catalog.cpp
    src/fs/ntfs_logfile.cpp
    src/fs/vss_scanner.cpp
    src/fs/volume_identity.cpp
    src/fs/bitlocker_fve.cpp
    src/fs/bitlocker_unlock.cpp
    src/search/content_search.cpp
    src/fs/ntfs_parser.cpp
    src/fs/virtual_raid.cpp
    src/scan_coordinator.cpp
    src/scan/dedup_index.cpp
    src/carver/signature_engine.cpp
    src/carver/parsers/zip_family.cpp
    src/carver/parsers/sqlite_carver.cpp
    src/carver/parsers/mp4_carver.cpp
    src/io/disk_reader_win.cpp
    src/io/volume_mapper_win.cpp
    src/memory/memory_pool.cpp
    src/security/data_shredder.cpp
    third_party/sqlite3.c
  )
  target_link_libraries(wolf_tests
    PRIVATE
      GTest::gtest_main
  )
  target_include_directories(wolf_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(WIN32)
    target_link_libraries(wolf_tests PRIVATE setupapi winhttp)
  endif()
  include(GoogleTest)
  gtest_discover_tests(wolf_tests)
endif()








