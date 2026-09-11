#include "shell.h"
#include "esp_log.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <cstring>
#include "esp_vfs_dev.h"
#include "linenoise/linenoise.h"
#include "driver/usb_serial_jtag.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "esp_vfs_usb_serial_jtag.h"
#endif
constexpr char shell_header[] = "ESP@wireless_power_meter_pro_v2> ";

Shell::Shell()
    : mode_(Mode::ESP_LOG), listener_task_handle_(nullptr), original_log_level_(ESP_LOG_INFO), initialized_(false) {}

Shell::~Shell() {
    stop_interactive();
    if (initialized_) {
        esp_console_deinit();
    }
}

esp_err_t Shell::init() {
    if (initialized_) {
        return ESP_OK;
    }

    // 1. 初始化控制台核心
    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    console_config.max_cmdline_length   = 256;
    console_config.max_cmdline_args     = 16;
    console_config.hint_color           = 36; // 青色
    console_config.hint_bold            = 0;

    esp_err_t ret = esp_console_init(&console_config);
    if (ret != ESP_OK) {
        ESP_LOGE("Shell", "console init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // === 配置 linenoise 核心交互引擎 ===
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback*)&esp_console_get_hint);
    linenoiseHistorySetMaxLen(50);
    // ==========================================

    // 注册基础命令
    initialized_ = true;
    register_command(
        ShellCommand_t("help", "Print the summary of all registered commands", "[<command>]",
                       [](int argc, char** argv) -> int { return Shell::instance().help_command(argc, argv); }));
    register_command(ShellCommand_t("exit", "Exit interactive shell mode", "", [](int argc, char** argv) -> int {
        Shell::instance().stop_interactive();
        return 0;
    }));

    // 安装 USB 驱动并挂载 VFS
    usb_serial_jtag_driver_config_t usj_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t                       err        = usb_serial_jtag_driver_install(&usj_config);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        usb_serial_jtag_vfs_use_driver();
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#else
        esp_vfs_usb_serial_jtag_use_driver();
        esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
        esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#endif
    } else {
        ESP_LOGW("Shell", "USB serial JTAG driver unavailable: %s", esp_err_to_name(err));
    }

    if (xTaskCreate(listener_task, "listener_task", SHELL_TASK_STACK_SIZE, this, SHELL_TASK_PRIORITY,
                    &listener_task_handle_) != pdPASS) {
        ESP_LOGE("Shell", "listener task creation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("Shell", "Shell initialized successfully");
    return ESP_OK;
}

esp_err_t Shell::register_command(const ShellCommand_t& cmd) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    // 检查是否已存在同名命令
    for (const auto& existing_cmd : commands_) {
        if (existing_cmd->name() == cmd.name()) {
            ESP_LOGW("Shell", "Command '%s' already registered", cmd.name().c_str());
            return ESP_ERR_INVALID_ARG;
        }
    }
    auto wrapper = std::make_shared<ShellCommand_t>(cmd);
    commands_.push_back(wrapper);

    esp_console_cmd_t esp_cmd = {};
    esp_cmd.command           = wrapper->name().c_str();
    esp_cmd.help              = wrapper->help().empty() ? nullptr : wrapper->help().c_str();
    esp_cmd.hint              = wrapper->hint().empty() ? nullptr : wrapper->hint().c_str();
    esp_cmd.func_w_context    = &Shell::esp_console_wrapper_with_context;
    esp_cmd.context           = wrapper.get();
    esp_err_t ret             = esp_console_cmd_register(&esp_cmd);

    if (ret != ESP_OK) {
        ESP_LOGW("Shell", "Failed to register command '%s': %s", cmd.name().c_str(), esp_err_to_name(ret));
        commands_.pop_back();
        return ret;
    }

    ESP_LOGD("Shell", "Command '%s' registered successfully", cmd.name().c_str());

    return ESP_OK;
}

esp_err_t Shell::deregister_command(const std::string& name) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    // 从ESP控制台注销
    esp_err_t ret = esp_console_cmd_deregister(name.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE("Shell", "Failed to deregister command '%s': %s", name.c_str(), esp_err_to_name(ret));
        return ret;
    }

    // 从内部列表中移除
    auto it = std::remove_if(commands_.begin(), commands_.end(),
                             [&name](const std::shared_ptr<ShellCommand_t>& cmd) { return cmd->name() == name; });

    if (it != commands_.end()) {
        commands_.erase(it, commands_.end());
        ESP_LOGI("Shell", "Command '%s' deregistered successfully", name.c_str());
    }

    return ESP_OK;
}

esp_err_t Shell::run_command(const std::string& cmdline, int* ret) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_console_run(cmdline.c_str(), ret);
}
esp_err_t Shell::start_interactive() {
    if (mode_ == Mode::INTERACTIVE)
        return ESP_OK;

    original_log_level_ = esp_log_level_get("*");
    set_log_level(ESP_LOG_NONE); // 关闭 Log
    mode_ = Mode::INTERACTIVE;   // 切换状态机

    printf("\r\n>>> Interactive shell started. Type 'help' to see commands. <<<\r\n");
    return ESP_OK;
}

esp_err_t Shell::stop_interactive() {
    if (mode_ != Mode::INTERACTIVE)
        return ESP_OK;

    set_log_level(original_log_level_); // 恢复 Log
    mode_ = Mode::ESP_LOG;              // 切换状态机

    printf("\r\n>>> Returning to LOG mode... <<<\r\n");
    return ESP_OK;
}

void Shell::set_log_level(esp_log_level_t level) {
    esp_log_level_set("*", level);
}

// === 核心魔法：将监听任务和终端逻辑完美融合 ===
void Shell::listener_task(void* arg) {
    Shell*         shell = static_cast<Shell*>(arg);
    struct timeval timeout;
    fd_set         read_fds;
    constexpr int  LISTENER_DELAY_MS = 50;

    while (1) {
        if (shell->get_mode() == Mode::ESP_LOG) {
            // [状态1] Log 模式：静默监听键盘中断
            FD_ZERO(&read_fds);
            FD_SET(STDIN_FILENO, &read_fds);
            timeout.tv_sec  = 0;
            timeout.tv_usec = LISTENER_TIMEOUT_MS * 1000;

            int ret = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);
            if (ret > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
                char ch;
                // 读走触发中断的字符(如回车)，防止污染命令行
                if (read(STDIN_FILENO, &ch, 1) == 1) {
                    shell->start_interactive();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(LISTENER_DELAY_MS));
        } else {
            // [状态2] 交互模式：直接接管终端交互
            // linenoise 会阻塞在这里等待用户输入并按下回车
            char* line = linenoise(shell_header);

            if (line == NULL) {
                continue; // 忽略空行或读取错误
            }

            if (strlen(line) > 0) {
                linenoiseHistoryAdd(line); // 加入历史记录(支持上下键)
            }

            // 将输入的命令丢给核心去解析执行
            int       ret_val;
            esp_err_t err = esp_console_run(line, &ret_val);
            if (err == ESP_ERR_NOT_FOUND) {
                printf("Unrecognized command\n");
            } else if (err == ESP_OK && ret_val != ESP_OK) {
                printf("Command returned non-zero error code: 0x%x\n", ret_val);
            }

            linenoiseFree(line); // 释放内存，准备下一次输入
        }
    }
}

int Shell::help_command(int argc, char** argv) {
    if (argc > 1) {
        for (const auto& cmd : commands_) {
            if (cmd->name() == argv[1]) {
                printf("%s %s\n  %s\n\n", cmd->name().c_str(), cmd->hint().empty() ? "" : cmd->hint().c_str(),
                       cmd->help().empty() ? "" : cmd->help().c_str());
                return 0;
            }
        }
        printf("Unknown command '%s'\n", argv[1]);
        return 1;
    }
    for (const auto& cmd : commands_) {
        printf("%-16s %s\n", cmd->name().c_str(), cmd->help().empty() ? "" : cmd->help().c_str());
    }
    return 0;
}

int Shell::esp_console_wrapper(int argc, char** argv) {
    return -1;
}
int Shell::esp_console_wrapper_with_context(void* context, int argc, char** argv) {
    ShellCommand_t* cmd = static_cast<ShellCommand_t*>(context);
    return cmd->execute(argc, argv);
}
