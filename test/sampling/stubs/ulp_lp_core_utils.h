#pragma once
#include <cstdint>
extern uint32_t simulated_cycles;
inline uint32_t ulp_lp_core_get_cpu_cycles() { return simulated_cycles += 20000; }
inline void ulp_lp_core_delay_us(uint32_t us) { simulated_cycles += us * 20; }
