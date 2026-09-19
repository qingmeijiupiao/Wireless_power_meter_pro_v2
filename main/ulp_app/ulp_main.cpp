#include "ulp_lp_core.h"
#include "ulp_lp_core_critical_section_shared.h"
#include "ulp_lp_core_utils.h"
#include <cstddef>
#include <cstdlib>
#include "ina228.hpp"
#include "ulp_state.h"
#include "ulp_Interp.hpp"
#include "../../components/app/current_calibration/include/CurrentCalib.h"

constexpr uint32_t LP_CPU_FREQ_HZ       = 20000000;
constexpr uint32_t current_dead_zone_uA = 5000; // 电流死区，单位uA

#define MS_TO_US(ms) ((ms) * 1000)

#define LP_VAR __attribute__((section(".rtc.bss")))
/* 定义共享变量，存放在RTC内存中（.rtc.bss段） */
volatile uint32_t ulp_state                          LP_VAR;
ulp_lp_core_spinlock_t shared_lock                   LP_VAR;
volatile uint32_t log_data                           LP_VAR;
volatile uint32_t voltage_uv                         LP_VAR;
volatile uint16_t voltage_register_raw               LP_VAR;
volatile int32_t current_uA                          LP_VAR;
volatile int16_t shunt_register_raw                  LP_VAR;
volatile uint16_t ina228_manufacturer_id             LP_VAR;
volatile int32_t Board_temperature                   LP_VAR; // 单位0.01℃
volatile int64_t meter_uah                           LP_VAR; // 单位uAh
volatile int64_t meter_uwh                           LP_VAR; // 单位uWh
volatile CurrentCalib::params_t current_calib_params LP_VAR;

volatile uint32_t                            now_time_ms = 0;
UlpNonEquidistantInterp<int16_t, int16_t, 6> current_interp;
ULP_CORE_STATE&                              ulp_state_p = *(ULP_CORE_STATE*)&(ulp_state);
// 计算过程使用 LP 本地副本，避免每次采样长时间占用跨核锁。
CurrentCalib::params_t                       local_current_calib_params;

/**
 * @brief 在 LP/HP 共享自旋锁保护下执行无返回值的短操作。
 *
 * @tparam F 可调用对象类型。
 * @param action 临界区内执行的操作。
 *
 * @note 用于原子更新共享状态位、测量值和 64 位累计量。
 */
template <typename F> void with_shared_lock_void(F&& action) {
    ulp_lp_core_enter_critical(&shared_lock);
    action();
    ulp_lp_core_exit_critical(&shared_lock);
}

/**
 * @brief 发布一组完整的 INA228 采样结果到 RTC 共享区。
 *
 * @param new_voltage_register_raw INA228 总线电压折算后的兼容值，1.25mV/单位。
 * @param new_shunt_register_raw INA228 分流电压折算后的兼容值，2.5uV/单位。
 * @param new_voltage_uv 换算后的总线电压，单位 uV。
 * @param new_current_uA 补偿后的电流，单位 uA。
 *
 * @note 电压、电流和原始寄存器值必须同批次提交，HP 核才不会读到混合样本。
 */
static void publish_sample(uint16_t new_voltage_register_raw, int16_t new_shunt_register_raw, uint32_t new_voltage_uv,
                           int32_t new_current_uA) {
    // I2C 读取和补偿计算已在锁外完成，这里只原子提交一组共享采样值。
    with_shared_lock_void([=]() {
        voltage_register_raw                               = new_voltage_register_raw;
        shunt_register_raw                                 = new_shunt_register_raw;
        voltage_uv                                         = new_voltage_uv;
        current_uA                                         = new_current_uA;
        ulp_state_p.ulp_state_bits.ulp_ina228_read_timeout = false;
    });
}

/**
 * @brief 从 RTC 共享区复制电流校准参数。
 *
 * @return 当前 HP 核写入的电流校准参数副本。
 *
 * @note `current_calib_params` 位于 RTC 共享内存且带 volatile，逐字段复制后再在锁外重建插值表。
 */
static CurrentCalib::params_t read_current_calib_params() {
    // RTC 参数带 volatile，逐字段复制后再在锁外重建插值表。
    CurrentCalib::params_t params = {};
    with_shared_lock_void([&]() {
        params.current_base_K = current_calib_params.current_base_K;
        for (size_t i = 0; i < sizeof(params.points) / sizeof(params.points[0]); ++i) {
            params.points[i].register_value       = current_calib_params.points[i].register_value;
            params.points[i].offset_current_100uA = current_calib_params.points[i].offset_current_100uA;
        }
        params.temperature_K = current_calib_params.temperature_K;
    });
    return params;
}

constexpr uint32_t INA228_READ_TIMEOUT_MS = 1000;
// INA228 reset/config/首样本等待期间置位，避免 ina228_run() 在恢复流程内再次递归触发恢复。
static bool        ina228_configuring     = false;

bool ulp_ina228_init();
void timer_run(void);

/**
 * @brief 执行一次 INA228 采样轮询并发布成功样本。
 *
 * @note 读寄存器失败或转换未完成时先保留最后一次有效样本；连续失败超过
 *       INA228_READ_TIMEOUT_MS 后置位 `ulp_ina228_read_timeout`，并在非配置流程中
 *       阻塞执行 INA228 重新初始化，直到恢复首个有效样本。
 */
void ina228_run() {
    // 最近一次成功发布完整电压/电流样本的 LP 毫秒时间，用于判断数据是否陈旧。
    static uint32_t last_success_ms              = 0;
    const auto      handle_ina228_read_not_ready = [&]() {
        // 只在本函数内判断连续采样失败时长，避免把 INA228 私有状态暴露为文件级变量。
        if ((now_time_ms - last_success_ms) <= INA228_READ_TIMEOUT_MS) {
            return;
        }

        // 采样失效时保留最后一次有效样本，避免把通信异常伪装成 0V 欠压。
        with_shared_lock_void([]() { ulp_state_p.ulp_state_bits.ulp_ina228_read_timeout = true; });
        // 配置流程内部只标记超时，由外层 init 循环重新 reset/config，避免递归恢复。
        if (ina228_configuring) {
            return;
        }
        ulp_ina228_init();
        last_success_ms = now_time_ms;
    };

    uint16_t diagnostic = 0;
    if (INA228::read16(INA228::DIAG_ALRT, &diagnostic) != ESP_OK) {
        handle_ina228_read_not_ready();
        return;
    }
    if (!(diagnostic & (1 << 1))) { // CNVRF 位为 0 表示本轮转换未完成。
        handle_ina228_read_not_ready();
        return;
    }

    uint32_t vbus_register = 0;
    uint32_t vshunt_register = 0;
    /* 读取电压寄存器 */
    if (INA228::read24(INA228::VBUS, &vbus_register) != ESP_OK) {
        handle_ina228_read_not_ready();
        return;
    }

    /* 读取电流寄存器 */
    if (INA228::read24(INA228::VSHUNT, &vshunt_register) != ESP_OK) {
        handle_ina228_read_not_ready();
        return;
    }

    const uint32_t ina228_voltage_raw = INA228::decode_unsigned20(vbus_register);
    // VBUS: 195.3125uV/LSB. VSHUNT wide range: 312.5nV/LSB; divide by 8 to retain
    // the existing calibration domain of one raw unit per 2.5uV.
    const uint32_t new_voltage_uv = static_cast<uint32_t>((static_cast<uint64_t>(ina228_voltage_raw) * 3125U) / 16U);
    const uint16_t new_voltage_register_raw = static_cast<uint16_t>(new_voltage_uv / 1250U);
    const int16_t new_shunt_register_raw = static_cast<int16_t>(INA228::decode_signed20(vshunt_register) / 8);
    int32_t        new_current_uA    = 0;
    int32_t        board_temperature = 0;
    with_shared_lock_void([&]() { board_temperature = Board_temperature; });
    if (std::abs((int32_t)new_shunt_register_raw * local_current_calib_params.current_base_K) < current_dead_zone_uA) {
        new_current_uA = 0;
    } else {
        // 插值补偿映射
        int32_t no_temp_cali_current_uA = local_current_calib_params.current_base_K * new_shunt_register_raw +
                                          current_interp.interpolate(new_shunt_register_raw) * 100;

        // 温漂补偿
        int32_t delta_temp = (board_temperature - CurrentCalib::BASE_TEMPERATURE) / 100;
        int32_t temp_comp_uA =
            (no_temp_cali_current_uA / 1000) * local_current_calib_params.temperature_K * delta_temp / 1000;

        // 最终电流
        new_current_uA = no_temp_cali_current_uA - temp_comp_uA;
    }

    publish_sample(new_voltage_register_raw, new_shunt_register_raw, new_voltage_uv, new_current_uA);
    last_success_ms = now_time_ms;
}

/**
 * @brief 初始化 INA228 并等待首个有效电压样本。
 *
 * @return true 初始化、配置和首个样本读取成功；当前实现会一直重试，不返回 false。
 *
 * @note LP Core 的核心职责就是 INA228 采样，因此初始化和恢复阶段允许阻塞重试。
 *       恢复期间置位 `ulp_i2c_init_err` 与 `ulp_ina228_read_timeout`，HP 核保护逻辑会据此
 *       暂停 INA228 相关保护，避免用无效数据关断输出。
 */
bool ulp_ina228_init() {
    ina228_configuring = true;
    with_shared_lock_void([]() {
        ulp_state_p.ulp_state_bits.ulp_i2c_init_err        = true;
        ulp_state_p.ulp_state_bits.ulp_ina228_read_timeout = true;
    });

    while (true) {
        // 恢复循环中仍维护 LP 毫秒计数，避免超时判断长期停滞。
        timer_run();
        if (INA228::reset() != ESP_OK) {
            ulp_lp_core_delay_us(MS_TO_US(20));
            continue;
        }

        ulp_lp_core_delay_us(MS_TO_US(5));
        timer_run();

        uint16_t new_ina228_manufacturer_id = 0;
        if (INA228::read16(INA228::MANUFACTURER, &new_ina228_manufacturer_id) != ESP_OK ||
            new_ina228_manufacturer_id != INA228::MANUFACTURER_ID) {
            ulp_lp_core_delay_us(MS_TO_US(20));
            continue;
        }
        with_shared_lock_void([=]() { ina228_manufacturer_id = new_ina228_manufacturer_id; });

        uint16_t device_id = 0;
        if (INA228::read16(INA228::DEVICE, &device_id) != ESP_OK ||
            (device_id & INA228::DEVICE_ID_MASK) != INA228::DEVICE_ID) {
            ulp_lp_core_delay_us(MS_TO_US(20));
            continue;
        }
        if (INA228::configure() != ESP_OK) {
            ulp_lp_core_delay_us(MS_TO_US(20));
            continue;
        }

        const uint32_t sample_wait_start_ms = now_time_ms;
        while (true) {
            timer_run();
            ina228_run();
            bool sample_ready = false;
            with_shared_lock_void(
                [&]() { sample_ready = voltage_uv != 0 && !ulp_state_p.ulp_state_bits.ulp_ina228_read_timeout; });
            if (sample_ready) {
                // 首个有效样本发布后，测量链路重新变为可靠，HP 核可恢复 INA228 相关保护。
                with_shared_lock_void([]() {
                    ulp_state_p.ulp_state_bits.ulp_i2c_init_err        = false;
                    ulp_state_p.ulp_state_bits.ulp_ina228_init_ok      = true;
                    ulp_state_p.ulp_state_bits.ulp_ina228_read_timeout = false;
                });
                ina228_configuring = false;
                return true;
            }
            if ((now_time_ms - sample_wait_start_ms) > INA228_READ_TIMEOUT_MS) {
                // 配置成功但首样本迟迟不可用，重新 reset/config，处理 INA228 卡死或总线瞬断。
                break;
            }
            ulp_lp_core_delay_us(100);
        }
    }
};

/**
 * @brief 加载电流校准参数并重建 LP 本地插值表。
 *
 * @note 校准参数从 RTC 共享区复制到 LP 本地副本，避免运行期采样频繁持锁访问共享参数。
 */
void load_current_calib_params() {
    local_current_calib_params = read_current_calib_params();
    for (int i = 0; i < sizeof(local_current_calib_params.points) / sizeof(local_current_calib_params.points[0]); i++) {
        current_interp.set_point(i, local_current_calib_params.points[i].register_value,
                                 local_current_calib_params.points[i].offset_current_100uA);
    }
    current_interp.finish_load();
}

/**
 * @brief 更新 LP Core 内部毫秒计数。
 *
 * @note `ulp_lp_core_get_cpu_cycles()` 返回 CPU 周期计数，约 215 秒回绕一次；
 *       本函数将原始周期计数展开为持续递增的 `now_time_ms`。
 */
void timer_run(void) {
    constexpr uint32_t CYCLES_PER_MS = LP_CPU_FREQ_HZ / 1000;
    constexpr uint32_t MAX_MS        = 0xFFFFFFFF / CYCLES_PER_MS;
    static uint32_t    last_raw_ms   = 0;

    int32_t current_ms = ulp_lp_core_get_cpu_cycles() / CYCLES_PER_MS;
    int32_t diff       = current_ms - last_raw_ms;

    if (now_time_ms == 0) {
        now_time_ms = current_ms;
        last_raw_ms = current_ms;
        return;
    }

    if (diff < 0) { // 发生回绕
        now_time_ms += (MAX_MS - last_raw_ms) + current_ms;
    } else {
        now_time_ms += diff;
    }
    last_raw_ms = current_ms;
}

/**
 * @brief 检查 HP 核是否请求重新加载电流校准参数。
 *
 * @note 读取并清除 `ulp_reload_calib_params` 必须在同一临界区完成，避免重复处理或丢失请求。
 */
void check_reload_current_calib_params() {
    bool reload = false;
    with_shared_lock_void([&]() {
        reload                                             = ulp_state_p.ulp_state_bits.ulp_reload_calib_params;
        ulp_state_p.ulp_state_bits.ulp_reload_calib_params = false;
    });
    if (reload) {
        load_current_calib_params();
    }
}

/**
 * @brief 按当前采样值积分电量和能量累计值。
 *
 * @note 电荷单位为 uAh，能量单位为 uWh；内部保留带符号余数，避免低电流下积分量被截断。
 */
static int64_t charge_accum_uAms  = 0; // 电荷累加器，单位 uA·ms
static int64_t energy_accum_uvAms = 0; // 能量乘积累加器，单位 uA·uV·ms
void           update_meter() {
    static uint32_t last_run_ms = 0;
    uint32_t        delta_ms    = now_time_ms - last_run_ms; // 不处理回绕，直接减
    if (delta_ms == 0)
        return;

    // ----- 电量积分 (uAh) -----
    int32_t  sample_current_uA = 0;
    uint32_t sample_voltage_uv = 0;
    bool     sample_valid      = false;
    // 电流和电压必须在同一临界区读取，避免电量积分组合到不同批次样本。
    with_shared_lock_void([&]() {
        sample_current_uA = current_uA;
        sample_voltage_uv = voltage_uv;
        sample_valid      = !ulp_state_p.ulp_state_bits.ulp_ina228_read_timeout &&
                       ulp_state_p.ulp_state_bits.ulp_ina228_init_ok && !ulp_state_p.ulp_state_bits.ulp_i2c_init_err;
    });
    if (!sample_valid) {
        // 测量降级时保留最后显示样本，但不继续用陈旧电流积分电量。
        last_run_ms = now_time_ms;
        return;
    }

    charge_accum_uAms           += (int64_t)sample_current_uA * delta_ms;
    const int64_t UAH_THRESHOLD  = 3600000LL; // 1 uAh = 3,600,000 uA·ms
    const int64_t whole_uah      = charge_accum_uAms / UAH_THRESHOLD;
    charge_accum_uAms           %= UAH_THRESHOLD; // 保留带符号余数

    // ----- 能量积分 (uWh) -----
    // 1 uWh = 3.6e12 uA·uV·ms
    energy_accum_uvAms          += (int64_t)sample_current_uA * sample_voltage_uv * delta_ms;
    const int64_t UWH_THRESHOLD  = 3600000000000LL; // 3.6e12
    const int64_t whole_uwh      = energy_accum_uvAms / UWH_THRESHOLD;
    energy_accum_uvAms          %= UWH_THRESHOLD;

    if (whole_uah != 0 || whole_uwh != 0) {
        // 两个 64 位累计值需要在同一临界区提交，供 HP 核读取一致快照。
        with_shared_lock_void([=]() {
            meter_uah += whole_uah;
            meter_uwh += whole_uwh;
        });
    }

    last_run_ms = now_time_ms;
}

/**
 * @brief 按固定毫秒间隔执行一次回调。
 *
 * @tparam F 可调用对象类型；每个模板实例拥有独立的 `last_run_ms`。
 * @param interval_ms 执行间隔，单位 ms。
 * @param action 到期后执行的回调。
 *
 * @note 当前实现使用 `>` 判断，因此实际间隔会略大于 `interval_ms`。
 */
template <typename F> void app_loop_every_ms(uint32_t interval_ms, F&& action) {
    static uint32_t last_run_ms = 0;
    if ((now_time_ms - last_run_ms) > interval_ms) {
        last_run_ms = now_time_ms;
        action();
    }
}

/**
 * @brief LP Core 应用入口。
 *
 * @return 不返回；INA228 初始化会阻塞重试直到成功，随后进入主循环。
 *
 *
 * @note 主循环持续轮询 INA228、维护毫秒计数、处理校准重载并执行电量积分。
 */
int main(void) {
    load_current_calib_params();
    ulp_ina228_init();
    with_shared_lock_void([]() { ulp_state_p.ulp_state_bits.ulp_run = true; });
    while (1) {
        timer_run();
        ina228_run();

        // 检查是否需要重新加载校准参数
        app_loop_every_ms(20, check_reload_current_calib_params);

        // 电量积分
        app_loop_every_ms(10, update_meter);
    }
}
