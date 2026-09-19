idf_component_register(
    SRCS "src/power_output.cpp"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES diagnostic_log hardware global_state protect short_circuit_detect log freertos esp_timer
    INCLUDE_DIRS "include"
)
