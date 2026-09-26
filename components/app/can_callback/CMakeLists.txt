idf_component_register(
    SRCS "src/can_callback.cpp"
    REQUIRES protect HXC_TWAI HXC_NVS nvs_gpio_output
    PRIV_REQUIRES diagnostic_log hardware global_state power_output
    INCLUDE_DIRS "include"
)
