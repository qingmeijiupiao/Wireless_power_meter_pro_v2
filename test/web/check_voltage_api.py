"""Compile the actual voltage handler with request/NVS stubs to check validation and writes.

JSON helpers are stubbed at their typed-field contract; this does not test JSON parsing.
Run: python test/web/check_voltage_api.py
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'components/app/web_backend/src/api_handlers.cpp').read_text(encoding='utf-8')
start = source.index('esp_err_t voltage_calibration_handler(')
end = source.index('/**\n * @brief GET/POST /api/calibration', start)
handler = source[start:end]
stub = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include "components/app/current_calibration/include/VoltageCalib.h"
using esp_err_t = int;
constexpr int ESP_OK = 0;
#define DEVICE_EVENT_I(...) ((void)0)
struct Field { bool boolean; uint32_t number; };
std::map<std::string, Field> fields;
std::string response;
int status;
unsigned writes;
uint32_t persisted;
namespace VoltageCalib {
Runtime runtime;
Runtime get_runtime() { return runtime; }
struct Store { int set(uint32_t k) { ++writes; persisted = k; return 0; } } ina226_k_data;
}
namespace WebServer {
struct Request { const char* body = "{}"; };
int load_body(Request*) { return 0; }
int send(Request*, int s, const char*, const char* data, size_t size) { status=s; response.assign(data,size); return 0; }
int send_json(Request*, const char* data) { status=200; response=data; return 0; }
}
char response_buffer[1024];
bool json_has_key(const char*, const char* key) { return fields.count(key); }
bool json_get_uint32(const char*, const char* key, uint32_t* value) {
    if (!fields.count(key) || fields.at(key).boolean) return false;
    *value=fields.at(key).number; return true;
}
bool json_get_bool(const char*, const char* key, bool* value) {
    if (!fields.count(key) || !fields.at(key).boolean) return false;
    *value=fields.at(key).number; return true;
}
int calibration_bad_request(WebServer::Request*, const char* reason) { status=400; response=reason; return 0; }
'''
tests = r'''
void invoke(std::map<std::string, Field> input) {
    fields=input; WebServer::Request request; voltage_calibration_handler(&request);
}
void reject(std::map<std::string, Field> input, const char* reason) {
    auto before=writes; invoke(input); assert(status==400 && response==reason && writes==before);
}
int main() {
    using namespace VoltageCalib;
    runtime.frontend=SamplingFrontend::INA226; runtime.available=true; runtime.uncalibrated_uv=2000000;
    invoke({{"voltage_k_ppm", {false, 2123456}}});
    assert(status==200 && persisted==2123456 && response.find("2.123456")!=std::string::npos);
    invoke({{"real_voltage_mv", {false, 4185}}}); assert(status==200 && persisted==2092500);
    invoke({{"reset", {true, 1}}}); assert(status==200 && persisted==2000000);
    invoke({{"voltage_k_ppm", {false, 500000}}}); assert(status==200 && persisted==500000);
    invoke({{"voltage_k_ppm", {false, 4000000}}}); assert(status==200 && persisted==4000000);
    reject({{"voltage_k_ppm", {false, 499999}}}, "invalid_voltage_k");
    reject({{"voltage_k_ppm", {false, 4000001}}}, "invalid_voltage_k");
    reject({{"voltage_k_ppm", {true, 1}}}, "invalid_voltage_k");
    reject({{"real_voltage_mv", {false, 0}}}, "invalid_real_voltage");
    reject({{"real_voltage_mv", {false, 8001}}}, "computed_voltage_k_out_of_range");
    reject({{"reset", {true, 0}}}, "invalid_voltage_reset");
    reject({{"reset", {true, 1}}, {"voltage_k_ppm", {false, 2000000}}}, "choose_one_voltage_operation");
    reject({}, "choose_one_voltage_operation");
    runtime.uncalibrated_uv=0;
    reject({{"real_voltage_mv", {false, 4185}}}, "zero_bus_voltage");
    runtime.frontend=SamplingFrontend::INA228;
    reject({{"voltage_k_ppm", {false, 2000000}}}, "ina228_voltage_k_fixed");
    runtime.frontend=SamplingFrontend::INA226; runtime.available=false;
    reject({{"reset", {true, 1}}}, "voltage_sample_unavailable");
    runtime.frontend=SamplingFrontend::Unknown;
    reject({{"reset", {true, 1}}}, "voltage_sample_unavailable");
    puts("PASS: actual voltage handler validation, measured conversion, independent persistence, fixed INA228, unavailable samples");
}
'''
build = root / 'build/web_voltage_api_test'
build.mkdir(parents=True, exist_ok=True)
cpp = build / 'handler.cpp'
cpp.write_text(stub + handler + tests, encoding='utf-8')
exe = build / 'handler.exe'
subprocess.run(['D:/mingw64/bin/g++.exe', '-std=c++17', '-I', str(root), str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
