#ifndef ULP_STATE_H
#define ULP_STATE_H
#include <stdint.h>

union ULP_CORE_STATE {
    uint32_t ulp_state_raw;
    struct {
        uint32_t ulp_have_log            : 1;
        uint32_t ulp_i2c_init_err        : 1;
        uint32_t ulp_ina228_init_ok      : 1;
        uint32_t ulp_ina228_read_timeout : 1;
        uint32_t ulp_run                 : 1;
        uint32_t ulp_reload_calib_params : 1;
        uint32_t ulp_reserved            : 26;
    } ulp_state_bits;
} __attribute__((packed, aligned(4)));

#endif
