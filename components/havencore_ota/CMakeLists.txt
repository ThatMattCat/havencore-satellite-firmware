idf_component_register(
    SRCS "havencore_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server app_update esp_https_ota esp_http_client json
)
