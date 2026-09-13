cmake_minimum_required(VERSION 3.20)

# ESP-IDF component mode: skip project() when built via idf_component_register
if(NOT DEFINED IDF_TARGET)
  project(esp-modem-link VERSION 0.1.0 LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  option(ESP_MODEM_LINK_BUILD_TESTS "Build tests" ON)
  option(ESP_MODEM_LINK_BUILD_EXAMPLES "Build examples" OFF)

  if(NOT ESP_MODEM_LINK_PLATFORM)
    set(ESP_MODEM_LINK_PLATFORM "POSIX")
  endif()

  message(STATUS "esp-modem-link platform: ${ESP_MODEM_LINK_PLATFORM}")
endif()

set(ESP_MODEM_LINK_PUBLIC_HEADERS
  include/esp_modem_link/network_error.h
  include/esp_modem_link/at_error.h
  include/esp_modem_link/common_types.h
  include/esp_modem_link/module_capabilities.h
  include/esp_modem_link/config_types.h
  include/esp_modem_link/callbacks.h
  include/esp_modem_link/tcp_client.h
  include/esp_modem_link/udp_client.h
  include/esp_modem_link/http_client.h
  include/esp_modem_link/mqtt_client.h
  include/esp_modem_link/websocket_client.h
  include/esp_modem_link/network_interface.h
  include/esp_modem_link/cellular_device.h
  include/esp_modem_link/uart_config.h
)

set(ESP_MODEM_LINK_PRIVATE_SOURCES
  src/error/network_error.cpp
  src/error/at_error.cpp
  src/at_parser/at_parser.cpp
  src/at_channel/response_parser.cpp
  src/at_channel/urc_dispatcher.cpp
  src/at_channel/at_uart.cpp
  src/at_channel/baud_alignment.cpp
  src/hal/module_registry.cpp
  src/hal/hal_tcp_client.cpp
  src/hal/hal_udp_client.cpp
  src/network/cellular_network.cpp
  src/modules/module_registrations.cpp
  src/modules/ml307/ml307_hal.cpp
  src/protocol/mqtt/mqtt_packet.cpp
  src/protocol/mqtt/software_mqtt_client.cpp
  src/protocol/http/http_parser.cpp
  src/protocol/http/http_request_builder.cpp
  src/protocol/http/http_redirect.cpp
  src/protocol/http/software_http_client.cpp
  src/protocol/websocket/ws_frame.cpp
  src/protocol/websocket/ws_handshake.cpp
  src/protocol/websocket/software_ws_client.cpp
  src/device/cellular_device.cpp
)

set(ESP_MODEM_LINK_PRIVATE_HEADERS
  src/at_parser/at_parser.h
  src/at_channel/iat_channel.h
  src/at_channel/response_parser.h
  src/at_channel/urc_dispatcher.h
  src/at_channel/at_uart.h
  src/at_channel/baud_alignment.h
  src/platform/itask.h
  src/platform/iqueue.h
  src/platform/ievent_group.h
  src/platform/itimer.h
  src/platform/igpio.h
  src/platform/iuart.h
  src/platform/serial_uart.h
  src/platform/time.h
  src/platform/random.h
  src/hal/imodule_hal.h
  src/hal/module_registry.h
  src/hal/hal_tcp_client.h
  src/hal/hal_udp_client.h
  src/network/cellular_network.h
  src/modules/module_registrations.h
  src/modules/ml307/ml307_hal.h
  src/protocol/mqtt/mqtt_packet.h
  src/protocol/mqtt/software_mqtt_client.h
  src/protocol/http/http_parser.h
  src/protocol/http/http_request_builder.h
  src/protocol/http/http_redirect.h
  src/protocol/http/software_http_client.h
  src/protocol/websocket/ws_frame.h
  src/protocol/websocket/ws_handshake.h
  src/protocol/websocket/software_ws_client.h
  src/device/cellular_device_impl.h
)

if(DEFINED IDF_TARGET)
  # ESP-IDF component mode
  idf_component_register(
    SRCS
      ${ESP_MODEM_LINK_PRIVATE_SOURCES}
      ${CMAKE_CURRENT_SOURCE_DIR}/platform/esp_idf/esp_idf_time.cpp
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS src
    REQUIRES driver
  )
  return()
endif()

# Standalone library
add_library(esp_modem_link STATIC
  ${ESP_MODEM_LINK_PUBLIC_HEADERS}
  ${ESP_MODEM_LINK_PRIVATE_HEADERS}
  ${ESP_MODEM_LINK_PRIVATE_SOURCES}
)

target_include_directories(esp_modem_link
  PUBLIC include
  PRIVATE src
)

target_compile_features(esp_modem_link PUBLIC cxx_std_23)

if(MSVC)
  target_compile_options(esp_modem_link PRIVATE /W4 /permissive-)
  target_compile_definitions(esp_modem_link PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(esp_modem_link PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ESP_MODEM_LINK_PLATFORM STREQUAL "POSIX")
  target_sources(esp_modem_link PRIVATE
    platform/posix/posix_time.cpp
    platform/posix/posix_random.cpp
    platform/posix/posix_task.cpp
    platform/posix/posix_event_group.cpp
    platform/posix/posix_timer.cpp
    platform/posix/posix_gpio.cpp
    platform/posix/posix_uart.cpp
    platform/posix/posix_serial_uart.cpp
  )
  target_include_directories(esp_modem_link PRIVATE platform/posix)
endif()

if(ESP_MODEM_LINK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Host tool for bringing up real hardware on a serial port.
if(ESP_MODEM_LINK_PLATFORM STREQUAL "POSIX" AND NOT DEFINED IDF_TARGET)
  add_executable(hw_smoke_test tools/hw_smoke_test.cpp)
  target_link_libraries(hw_smoke_test PRIVATE esp_modem_link)

  add_executable(serial_probe tools/serial_probe.cpp)
  target_link_libraries(serial_probe PRIVATE esp_modem_link)
  target_include_directories(serial_probe PRIVATE src)

  add_executable(at_trace tools/at_trace.cpp)
  target_link_libraries(at_trace PRIVATE esp_modem_link)
  target_include_directories(at_trace PRIVATE src)

  add_executable(tcp_smoke_test tools/tcp_smoke_test.cpp)
  target_link_libraries(tcp_smoke_test PRIVATE esp_modem_link)

  add_executable(udp_smoke_test tools/udp_smoke_test.cpp)
  target_link_libraries(udp_smoke_test PRIVATE esp_modem_link)

  add_executable(http_smoke_test tools/http_smoke_test.cpp)
  target_link_libraries(http_smoke_test PRIVATE esp_modem_link)
  target_include_directories(http_smoke_test PRIVATE src)

  add_executable(tcp_trace tools/tcp_trace.cpp)
  target_link_libraries(tcp_trace PRIVATE esp_modem_link)
  target_include_directories(tcp_trace PRIVATE src)
endif()

if(ESP_MODEM_LINK_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
