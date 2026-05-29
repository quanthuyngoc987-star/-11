idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_http_server esp_http_client esp-tls mbedtls nvs_flash esp_timer esp_netif driver json lwip)

target_link_libraries(${COMPONENT_LIB} INTERFACE m)
