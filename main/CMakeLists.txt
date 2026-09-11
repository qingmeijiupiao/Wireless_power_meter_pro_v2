idf_component_register(
    SRCS "app_main.cpp" 
         "ulp_loader/ulp_loader.cpp"
    INCLUDE_DIRS "." 
                 "ulp_loader"
    REQUIRES
        blackbox
        blackbox_service
        boot_diagnostics
        Temperature
        HXC_TWAI
        global_state
        protect
        ulp
        soc
        log
        esp_app_format
        freertos
        Button
        HXC_NVS
        shell
        wifi_manager
        wifi_service
        hardware
        screen
        PWM
        shell_command
        can_callback
        power_output
        current_calibration
        diagnostic_log
        web_backend
        ota_manager
        ota_service
)

# LP核程序配置
set(ulp_app_name ulp_main)
set(ulp_sources "ulp_app/ulp_main.cpp")
set(ulp_exp_dep_srcs "ulp_loader/ulp_loader.cpp")

ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")

# 构建脚本目录
set(SCRIPT_DIR "${CMAKE_SOURCE_DIR}/scripts")

# 构建前处理
add_dependencies(${COMPONENT_LIB} run_pre_build)

# 构建后处理
add_custom_target(run_post_build ALL
    COMMAND ${CMAKE_COMMAND} -E env "PROJECT_BUILD_DIR=${CMAKE_BINARY_DIR}" ${PYTHON} ${SCRIPT_DIR}/post_build.py
    COMMENT "Running post-build Python script..."
    VERBATIM
)
add_dependencies(run_post_build app bootloader)
