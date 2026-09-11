idf_component_register(
    SRCS "src/web_backend.cpp"
         "src/api_handlers.cpp"
         "src/log_capture.cpp"
         "src/page_handlers.cpp"
         "src/request_json.cpp"
         "src/rtos_stats_api.cpp"
         "src/blackbox_api.cpp"
         "src/ota_handlers.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES WebServer web_file wifi_service espnow_link espnow_service diagnostic_log global_state power_output protect esp_timer esp_system esp_http_server st7789_driver can_callback current_calibration hardware esp_app_format jsmn energy_meter ota_manager ota_service blackbox_service screen
)
