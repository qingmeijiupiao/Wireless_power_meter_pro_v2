idf_component_register(
    SRCS "src/boot_diagnostics.cpp"
    REQUIRES
        app_update
        blackbox
        blackbox_service
        can_callback
        current_calibration
        esp_app_format
        esp_system
        global_state
        hardware
        protect
        spi_flash
        wifi_service
    INCLUDE_DIRS "include"
)
