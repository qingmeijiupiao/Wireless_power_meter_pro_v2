idf_component_register(
                    SRCS "src/screen.cpp"
                         "src/config/display_config.cpp"
                         "src/core/page_registry.cpp"
                         "src/core/ui_manager.cpp"
                         "src/pages/dashboard_page.cpp"
                         "src/pages/battery_page.cpp"
                         "src/pages/wireless_page.cpp"
                         "src/pages/curve/curve_history.cpp"
                         "src/pages/curve/curve_page.cpp"
                         "src/pages/settings/settings_page.cpp"
                         "src/widgets/ui_chrome.cpp"
                    REQUIRES Button
                    PRIV_REQUIRES st7789_driver hardware diagnostic_log global_state protect freertos log ui_resources Fonts HXC_NVS wifi_service wifi_manager espnow_link espnow_service power_output can_callback can_resistor energy_meter blackbox blackbox_service current_calibration ota_service
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "private_include")
