idf_component_register(
    SRCS "src/shell_command.cpp"
    PRIV_REQUIRES shell esp_system esp_timer diagnostic_log hardware st7789_driver can_callback current_calibration global_state power_output wifi_service web_backend espnow_link espnow_service energy_meter blackbox blackbox_service screen short_circuit_detect
    INCLUDE_DIRS "include"
)
