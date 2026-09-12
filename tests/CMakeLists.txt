# Standalone smoke test - no external dependencies
add_executable(esp_modem_link_smoke_test
  smoke_test.cpp
)

target_link_libraries(esp_modem_link_smoke_test
  PRIVATE
    esp_modem_link
)

target_include_directories(esp_modem_link_smoke_test
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

add_test(NAME smoke_test COMMAND esp_modem_link_smoke_test)

# GoogleTest-based unit tests (optional - skipped if download fails)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_GetProperties(googletest)
if(NOT googletest_POPULATED)
  FetchContent_Populate(googletest)
  if(EXISTS ${googletest_SOURCE_DIR}/CMakeLists.txt)
    add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR})

    add_executable(esp_modem_link_tests
      test_error_system.cpp
      test_at_parser.cpp
      test_response_parser.cpp
      test_urc_dispatcher.cpp
      test_at_uart.cpp
      test_module_registry.cpp
      test_hal_tcp_client.cpp
      test_hal_udp_client.cpp
      test_cellular_network.cpp
      test_cellular_device.cpp
      test_http_parser.cpp
      test_http_request_builder.cpp
      test_software_http_client.cpp
      test_ml307_hal.cpp
    )

    target_link_libraries(esp_modem_link_tests
      PRIVATE
        esp_modem_link
        gtest_main
        gmock
    )

    target_include_directories(esp_modem_link_tests
      PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    include(GoogleTest)
    gtest_discover_tests(esp_modem_link_tests)
  else()
    message(WARNING "GoogleTest not available - skipping gtest-based tests")
  endif()
endif()
