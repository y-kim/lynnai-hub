idf_component_register(SRC_DIRS "."
                       EMBED_TXTFILES "cloud_ca.crt" "gas_ca.pem" "room_wiring.js" "../../../images/icon.svg"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES vfs nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client esp-tls esp_timer mbedtls lwip wolfssl led_strip esp_driver_i2c esp_driver_gpio esp_driver_uart esp_app_format efuse nvs_sec_provider bootloader_support app_update)

# 이미지 표시(hub_ota.c 의 hub_img_desc)는 코드에서 직접 가리키지 않아도 이미지에 남아야 한다(OTA 가 올라온 파일에서 읽는다)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u hub_img_desc")
