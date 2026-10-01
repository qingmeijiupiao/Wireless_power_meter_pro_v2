idf_component_register(
    SRCS "src/espnow_service_business.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        espnow_link
        espnow_service_proto
        power_output
        energy_meter
        global_state
        diagnostic_log
    PRIV_REQUIRES freertos log esp_system esp_timer
)
