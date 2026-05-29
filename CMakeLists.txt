cmake_minimum_required(VERSION 3.10)
project(bwe_publisher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(AGORA_SDK_DIR "${CMAKE_SOURCE_DIR}/agora_sdk")

if(NOT EXISTS "${AGORA_SDK_DIR}/libagora_rtc_sdk.so")
  message(FATAL_ERROR "Agora SDK not found at ${AGORA_SDK_DIR}. See README.md (copy or symlink the SDK).")
endif()

include_directories(${AGORA_SDK_DIR}/include)
link_directories(${AGORA_SDK_DIR})

add_executable(bwe_publisher
  src/main.cpp
  src/abr_controller.cpp
  src/content_player.cpp
  src/yuv_content_player.cpp
  src/audio_player.cpp
  src/h264_reader.cpp
  src/config.cpp
  src/json.cpp
  src/logging.cpp
)

target_compile_options(bwe_publisher PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(bwe_publisher
  agora_rtc_sdk
  pthread
  dl
)

# So the binary finds libagora_rtc_sdk.so (and its transitive deps, e.g.
# libagora-ffmpeg.so) relative to its own location at runtime. --disable-new-dtags
# emits DT_RPATH instead of DT_RUNPATH so the search path also applies to the
# SDK's own transitive dependencies (DT_RUNPATH does not propagate).
set_target_properties(bwe_publisher PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN/../agora_sdk"
)
target_link_options(bwe_publisher PRIVATE "-Wl,--disable-new-dtags")
