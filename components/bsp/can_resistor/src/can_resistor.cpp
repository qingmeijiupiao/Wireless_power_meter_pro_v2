#include "can_resistor.h"

#include "nvs_gpio_output.h"
#include "esp_log.h"

#include <utility>

namespace {

constexpr const char* TAG     = "CanResistor";
constexpr const char* NVS_KEY = "can_term";

} // namespace

CanResistor& CanResistor::instance() {
    static CanResistor controller;
    return controller;
}

CanResistor::CanResistor() = default;
CanResistor::~CanResistor() = default;

esp_err_t CanResistor::init(gpio_num_t gpio_num) {
    if (impl_) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }
    if (gpio_num == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "invalid GPIO");
        return ESP_ERR_INVALID_ARG;
    }

    impl_ = std::make_unique<NvsGpioOutput>(NvsGpioOutput::Config{gpio_num, NVS_KEY, false, true});
    if (on_change_) {
        impl_->set_on_change_callback(on_change_);
    }

    esp_err_t err = impl_->init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed on GPIO %d: %s", gpio_num, esp_err_to_name(err));
        impl_.reset();
        return err;
    }
    return ESP_OK;
}

esp_err_t CanResistor::set(bool enabled) {
    return impl_ ? impl_->set(enabled) : ESP_ERR_INVALID_STATE;
}

esp_err_t CanResistor::toggle() {
    return impl_ ? impl_->toggle() : ESP_ERR_INVALID_STATE;
}

bool CanResistor::get() const {
    return impl_ ? impl_->get() : false;
}

esp_err_t CanResistor::add_on_change_callback(std::function<void(bool)> callback) {
    on_change_ = std::move(callback);
    if (impl_) {
        impl_->set_on_change_callback(on_change_);
    }
    return ESP_OK;
}
