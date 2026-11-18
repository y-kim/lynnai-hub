idf_component_register(SRC_DIRS "."
                       EMBED_TXTFILES "srv.pem"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES vfs nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer mbedtls lwip wolfssl led_strip)
