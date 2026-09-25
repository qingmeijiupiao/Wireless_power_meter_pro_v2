#!/usr/bin/env python3
"""Compile real output/detector sources against threaded host hardware/RTOS fakes.

Exercises cancellation and GPIO ordering; does not replace physical board tests.
Generated headers and executable are confined to build_output_check/.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    args = parser.parse_args()
    out = ROOT / 'build_output_check'
    out.mkdir(exist_ok=True)
    for name in ('esp_err.h', 'esp_log.h', 'esp_timer.h', 'diagnostic_log.h',
                 'global_state.h', 'protect.h', 'hardware.h', 'HXC_NVS.h',
                 'driver/gpio.h', 'freertos/FreeRTOS.h', 'freertos/task.h', 'freertos/semphr.h'):
        header = out / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "output_host_runtime.h"\n', encoding='utf-8')
    command = [args.cxx, '-std=c++20', '-pthread', '-Wall', '-Wextra',
               '-Wno-unused-parameter', '-Wno-unused-variable', '-I', str(out),
               '-I', str(ROOT / 'test/output'),
               '-I', str(ROOT / 'components/app/power_output/include'),
               '-I', str(ROOT / 'components/middleware/short_circuit_detect/include'),
               str(ROOT / 'test/output/output_transactions.cpp'),
               str(ROOT / 'components/app/power_output/src/power_output.cpp'),
               str(ROOT / 'components/middleware/short_circuit_detect/src/short_circuit_detect.cpp'),
               '-o', str(out / 'output_check.exe')]
    subprocess.run(command, check=True)
    subprocess.run([str(out / 'output_check.exe')], check=True, timeout=120)
    for name in ('Button.h', 'screen.h', 'freertos/queue.h', 'st7789.h',
                 'config/display_config.h', 'pages/curve/curve_history.h', 'widgets/ui_chrome.h'):
        header = out / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "ui_host_runtime.h"\n', encoding='utf-8')
    # task.h is also used by the actual UI manager for ticks.
    (out / 'freertos/task.h').write_text('#include "ui_host_runtime.h"\n', encoding='utf-8')
    subprocess.run([args.cxx, '-std=c++20', '-pthread', '-DUI_SCHEDULER_HOST_TEST', '-I', str(out),
                    '-I', str(ROOT / 'test/output'),
                    '-I', str(ROOT / 'components/app/power_output/include'),
                    '-I', str(ROOT / 'components/app/screen/private_include'),
                    str(ROOT / 'test/output/ui_events.cpp'),
                    str(ROOT / 'components/app/screen/src/core/ui_manager.cpp'),
                    '-o', str(out / 'ui_events.exe')], check=True)
    subprocess.run([str(out / 'ui_events.exe')], check=True, timeout=10)


if __name__ == '__main__':
    main()
