idf_component_register(
    SRCS "esp32c3_test.cpp"
    INCLUDE_DIRS "."
    # The component's name comes from its directory, which is esp-modem-link -
    # the C++ namespace esp_modem_link is not the name IDF knows it by, so the
    # underscore form here fails with "unknown name".
    REQUIRES esp-modem-link
    # esp_timer_get_time() is called directly here. It also arrives transitively
    # through the library's own requirements, but relying on that would break
    # this example the day the library stops needing it.
    PRIV_REQUIRES esp_timer
)
