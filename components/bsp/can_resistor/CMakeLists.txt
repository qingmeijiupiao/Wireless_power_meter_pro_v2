idf_component_register(
    SRCS "src/can_resistor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio nvs_gpio_output
    PRIV_REQUIRES log
)
