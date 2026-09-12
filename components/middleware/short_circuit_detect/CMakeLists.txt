idf_component_register(
    SRCS "src/short_circuit_detect.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hardware HXC_NVS
    PRIV_REQUIRES freertos log
)
