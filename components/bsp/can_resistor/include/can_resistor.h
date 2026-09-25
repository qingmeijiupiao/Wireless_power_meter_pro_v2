/*
 * @version: no version
 * @Description: CAN 终端电阻控制（板级门面，基于 nvs_gpio_output）
 * @author: qingmeijiupiao
 */
#ifndef CAN_RESISTOR_H
#define CAN_RESISTOR_H

#include "driver/gpio.h"
#include "esp_err.h"
#include <functional>
#include <memory>

class NvsGpioOutput;

/**
 * @brief CAN 终端电阻控制器（板级单例门面）。
 *
 * 具体硬件相关：引脚由 `init()` 传入，状态通过通用组件 `nvs_gpio_output`
 * 持久化到 NVS Key `can_term`。
 */
class CanResistor {
  public:
    /**
     * @brief 获取 CAN 终端电阻控制器单例。
     * @return 控制器引用。
     */
    static CanResistor& instance();

    /**
     * @brief 初始化终端电阻 GPIO，并恢复 NVS 中保存的状态。
     * @param gpio_num 终端电阻使能 GPIO。
     * @return ESP_OK 成功，其他值表示 GPIO 初始化或状态恢复失败。
     */
    esp_err_t init(gpio_num_t gpio_num);

    /**
     * @brief 设置终端电阻状态并写入 NVS。
     * @param enabled true 表示接入终端电阻。
     * @return ESP_OK 成功，未初始化时返回 ESP_ERR_INVALID_STATE。
     */
    esp_err_t set(bool enabled);

    /**
     * @brief 切换终端电阻状态并写入 NVS。
     * @return ESP_OK 成功，未初始化时返回 ESP_ERR_INVALID_STATE。
     */
    esp_err_t toggle();

    /**
     * @brief 获取当前终端电阻状态。
     * @return true 表示终端电阻已接入。
     */
    bool get() const;

    /**
     * @brief 添加终端电阻状态改变回调函数。
     * @param callback 回调函数，参数为当前终端电阻状态。
     * @return ESP_OK 成功。
     */
    esp_err_t add_on_change_callback(std::function<void(bool)> callback);

  private:
    CanResistor();
    ~CanResistor();
    CanResistor(const CanResistor&)            = delete;
    CanResistor& operator=(const CanResistor&) = delete;

    std::unique_ptr<NvsGpioOutput> impl_;
    std::function<void(bool)>      on_change_;
};

#endif // CAN_RESISTOR_H
