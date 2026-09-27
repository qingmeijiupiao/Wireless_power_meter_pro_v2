#pragma once
using ulp_lp_core_spinlock_t = int;
inline void ulp_lp_core_enter_critical(ulp_lp_core_spinlock_t*) {}
inline void ulp_lp_core_exit_critical(ulp_lp_core_spinlock_t*) {}
