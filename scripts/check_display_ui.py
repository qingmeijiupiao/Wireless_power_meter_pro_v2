#!/usr/bin/env python3
"""Host regression checks and snapshots from the actual C++ display functions.

Requires a C++17 compiler (CXX or --cxx) and Pillow. Hardware/service calls are
stubbed; drawing functions and page render methods are extracted unchanged from
the project, so this does not validate SPI timing, panel offsets or live services.
Outputs are written under build_ui_check, never linked into firmware.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')


def function(source, signature):
    """Extract one complete definition, including nested lambdas."""
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    args = parser.parse_args()
    out = ROOT / 'build_ui_check'
    out.mkdir(exist_ok=True)
    for name, content in {'driver/spi_master.h': 'typedef int spi_host_device_t;',
                          'driver/gpio.h': '', 'esp_err.h': 'typedef int esp_err_t;'}.items():
        p = out / name
        p.parent.mkdir(exist_ok=True, parents=True)
        p.write_text(content, encoding='utf-8')
    driver = read('components/bsp/st7789_driver/src/st7789.cpp')
    pages = {name: read('components/app/screen/src/pages/' + name + '.cpp')
             for name in ('dashboard_page', 'battery_page', 'wireless_page', 'settings/settings_page', 'curve/curve_page')}
    cpp = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "st7789.h"
#include "DENGB16.h"
#include "DENGB20.h"
#include "DENGB28_NUM.h"
#include "DENGB44_NUM.h"
#include "DENGB28_UNITS.h"
#include "DENGB32_METER.h"
#include "widgets/ui_chrome.h"
#include "start_logo.h"
#include "pages/curve/curve_history.h"
namespace ST7789 {
uint16_t display_width=WIDTH, display_height=HEIGHT;
struct {
    uint64_t guard_before=0x123456789abcdef0ULL;
    uint16_t data[2][WIDTH*HEIGHT] = {};
    uint64_t guard_after=0x123456789abcdef0ULL;
    uint8_t current_buffer=0;
} double_buffer;
'''
    # These are the real renderer implementations, not a Python approximation.
    cpp += driver[driver.index('void fill_rect('):driver.index('void set_rotation(')]
    cpp += driver[driver.index('static uint16_t map_px_data('):driver.index('uint16_t get_width(')]
    cpp += function(driver, 'void draw_image(')
    cpp += r'''
void snapshot(const char* name) {
    std::ofstream file(std::string(name)+".ppm", std::ios::binary);
    file << "P6\n240 135\n255\n";
    for(auto big : double_buffer.data[0]) {
        uint16_t p=(big>>8)|(big<<8);
        char rgb[]={char(((p>>11)&31)*255/31),char(((p>>5)&63)*255/63),char((p&31)*255/31)};
        file.write(rgb,3);
    }
}
}
enum ProtectState_t {PROTECT_STATE_NORMAL,PROTECT_STATE_WARNING,PROTECT_STATE_PROTECT};
struct State {
    uint16_t voltage_mV=10234; int32_t current_uA=5325000; int32_t board_temperature=3600;
    struct {bool output_enabled=true;} flags;
    struct {struct {ProtectState_t temperature_protect_state=PROTECT_STATE_PROTECT;
      ProtectState_t high_voltage_protect_state=PROTECT_STATE_WARNING;
      ProtectState_t low_voltage_protect_state=PROTECT_STATE_NORMAL;
      ProtectState_t current_protect_state=PROTECT_STATE_NORMAL;} states_bit;} protect_states;
} state;
State get_global_state(){return state;}
uint32_t ticks=21781000;
uint32_t xTaskGetTickCount(){return ticks;}
constexpr int portTICK_PERIOD_MS=1;
constexpr int ESP_OK=0, WIFI_STATE_STA_CONNECTED=1;
const char* esp_err_to_name(int){return "ESP_FAIL";}
namespace EnergyMeter {
struct Snapshot {int64_t energy_uwh=1000230,charge_uah=1000230;uint64_t meter_time_ms=21781000;} meter;
Snapshot snapshot(){return meter;}
}
struct IP_t {unsigned octet1=192,octet2=168,octet3=1,octet4=111;};
namespace WifiService {
enum class Mode{OFF,ESPNOW_ONLY,STA,AP_PROVISION};
Mode mode=Mode::STA; bool provisioning=false; std::string ssid="YOUR-2.4G-WIFI"; IP_t ip;
Mode get_mode(){return mode;} bool is_provisioning(){return provisioning;}
struct Config{const char* ssid;}; Config get_config(){return {ssid.c_str()};}
const char* get_ap_ssid(){return "PRO-V2-SETUP";}
IP_t get_ip(){return ip;} int get_channel(uint8_t* c){*c=1;return 0;}
uint8_t get_signal_percent(){return 75;} int get_wifi_state(){return 1;}
}
namespace EspNowService {
struct RemoteSwitchStatus{bool battery_valid=true;unsigned battery_percent=93;};
bool get_remote_switch_status(RemoteSwitchStatus& s){s={};return true;}
}
namespace SCREEN {
enum class RenderMode {Full};
struct DashboardPage {void render(RenderMode);void draw_protect_tag(uint16_t,uint16_t,const char*,ProtectState_t);};
struct BatteryPage {void render(RenderMode);};
struct WirelessPage {int last_result_=0;void render(RenderMode);};
struct SettingsPage {
 enum class Mode{View,Edit,Dialog};enum class ItemType{Detail,Value};
 enum Item {Rotate180,Backlight,WebBoot,ProtectBypass,BlackboxSnapshot,EspNowPair,EspNowInfo,CanBaudrate,CanTerm,FirmwareInfo,FirmwareUpdate,BlackboxInfo,CalibrationInfo,ITEM_COUNT};
 Mode mode_=Mode::Edit;uint8_t selected_=0;static constexpr uint8_t VISIBLE_ROWS=3;
 const char* item_name(uint8_t i) const;
 const char* item_value(uint8_t i){const char* values[]={"180","1/5","OFF","ON","60s","Pairing","","500 kbps","ON","","Available","",""};return values[i];}
 ItemType item_type(uint8_t i){return i==EspNowInfo||i==FirmwareInfo||i==BlackboxInfo||i==CalibrationInfo?ItemType::Detail:ItemType::Value;}
 const char* detail_lines_[4]={"Version v0.9.99 local","Build Time","2026/09/12 12:00:00","MAC FF:FF:FF:FF:FF:FF"};
 void build_dialog_content(){} void render(RenderMode);void draw_dialog_overlay();
};
struct CurvePage {
 enum class DisplayMode{Voltage,Current,Power,All};enum class EditItem{Display,TimeWindow};
 struct AutoRange{float minimum=0,maximum=1;uint32_t shrink_candidate_ms=0;bool initialized=false;};
 DisplayMode display_mode_=DisplayMode::Voltage;EditItem edit_item_=EditItem::Display;
 bool editing_=false;AutoRange ranges_[3];CurveBucket buckets_[240];
 uint32_t window_ms()const{return 30000;}
 const char* window_text()const{return "30s";}
 const char* display_mode_text()const{return display_mode_==DisplayMode::All?"ALL":display_mode_==DisplayMode::Voltage?"V":display_mode_==DisplayMode::Current?"A":"W";}
 void update_auto_range(CurveMetric,const CurveBucket*,size_t,uint32_t);
 void draw_grid(uint16_t,uint16_t,uint16_t,uint16_t)const;
 void draw_bucket_curve(const CurveBucket*,size_t,const AutoRange&,uint16_t,uint16_t,uint16_t,ST7789::color_t)const;
 void draw_single_metric(CurveMetric,ST7789::color_t);void draw_all_metrics();void render(RenderMode);
};
constexpr float CURVE_MINIMUM_SPANS[]={0.2f,0.05f,0.5f};
'''
    for name in ('void format_duration(', 'const char* format_capacity('):
        cpp += function(pages['battery_page'], name)
    for name in ('void format_curve_value(', 'float nice_curve_step(', 'void draw_curve_badge('):
        cpp += function(pages['curve/curve_page'], name)
    cpp += function(pages['wireless_page'], 'const char* wifi_mode_text(')
    for file, names in {
        'dashboard_page':['void DashboardPage::render(', 'void DashboardPage::draw_protect_tag('],
        'battery_page':['void BatteryPage::render('],
        'wireless_page':['void WirelessPage::render('],
        'settings/settings_page':['const char* SettingsPage::item_name(', 'void SettingsPage::render(', 'void SettingsPage::draw_dialog_overlay('],
        'curve/curve_page':['void CurvePage::update_auto_range(', 'void CurvePage::draw_grid(',
                            'void CurvePage::draw_bucket_curve(', 'void CurvePage::draw_single_metric(',
                            'void CurvePage::draw_all_metrics(', 'void CurvePage::render(']
    }.items():
        for name in names:
            extracted = function(pages[file], name)
            if name == 'void SettingsPage::render(':
                extracted = extracted.replace('UI::text(68, y, name_w', 'assert(UI::text_width(name, DENGB16) <= name_w); UI::text(68, y, name_w')
            cpp += extracted
    # Real curve history implementation, with only its hardware include removed.
    history = read('components/app/screen/src/pages/curve/curve_history.cpp')
    history = history[history.index('namespace SCREEN {') + len('namespace SCREEN {'):]
    cpp += history
    cpp += r'''
int main() {
 using namespace ST7789;
 using namespace SCREEN;
 assert(WIDTH==240 && HEIGHT==135);
 // A two-row image clipped at the right edge must keep its original stride.
 fill_screen(BLACK);uint16_t image[]={0xF800,0x07E0,0x001F,0xFFFF,0x1234,0x5678};
 draw_image(239,0,3,2,image);
 assert(double_buffer.data[0][239]==0x00F8);
 assert(double_buffer.data[0][479]==0xFFFF);
 // Clipping a glyph must not wrap into the next row or second framebuffer.
 fill_screen(BLACK);std::fill(double_buffer.data[1],double_buffer.data[1]+WIDTH*HEIGHT,0xA55A);
 draw_char(239,134,'8',WHITE,BLACK,DENGB28_NUM);
 for(size_t i=0;i<WIDTH*HEIGHT-1;++i) assert(double_buffer.data[0][i]==0);
 for(auto p:double_buffer.data[1]) assert(p==0xA55A);
 assert(double_buffer.guard_before==0x123456789abcdef0ULL && double_buffer.guard_after==0x123456789abcdef0ULL);
 fill_screen(BLACK);draw_string(0,0,"\x7f\xff",WHITE,BLACK,DENGB16);
 std::vector<uint16_t> invalid(double_buffer.data[0],double_buffer.data[0]+WIDTH*HEIGHT);
 fill_screen(BLACK);draw_string(0,0,"??",WHITE,BLACK,DENGB16);
 assert(std::equal(invalid.begin(),invalid.end(),double_buffer.data[0]));
 // Numeric subsetting must retain the exact value, using a smaller font if needed.
 assert(DENGB28_NUM.width_table['A'-32]==0);
 assert(UI::text_width("65.535",DENGB28_NUM)<=123);
 assert(UI::text_width("65.535V",DENGB44_NUM)<=159);
 assert(DENGB44_NUM.font_height<=47);
 assert(UI::text_width("9999999.999",DENGB16)<=123);
 assert(UI::text_width("OUTPUT",DENGB16)<=66);
 assert(UI::text_width("MAX",DENGB16)<=36);
 assert(UI::text_width("NOW",DENGB16)<=40);
 assert(UI::text_width("ALL",DENGB16)<=30);
 assert(UI::text_width("100%",DENGB16)<=40);
 assert(UI::text_width("IP:255.255.255.255",DENGB16)<=160);
 assert(UI::text_width("99:59:59",DENGB16)<=62);
 fill_screen(BLACK);UI::text(10,10,40,18,"abcdefghijklmnopqrstuvwxyz",WHITE,BLACK,DENGB16);
 for(int y=0;y<HEIGHT;++y)for(int x=0;x<WIDTH;++x)
   if(x<10||x>=50||y<10||y>=28)assert(double_buffer.data[0][y*WIDTH+x]==0);
 assert(UI::text_width("999.999mWh",DENGB32_METER)<=198);
 assert(UI::text_width("9.99W",DENGB16)<=48);
 char formatted[32];
 const double capacities[]={0,0.001,9.999,99.999,999.49,999.5,1000.23,999999,9.223372e15};
 for(double value:capacities)for(bool energy:{false,true}){
   const char* unit=format_capacity(formatted,sizeof(formatted),value,energy);
   assert(UI::text_width(formatted,DENGB44_NUM)+UI::text_width(unit,DENGB28_UNITS)+3<=178);
 }
 assert(std::strcmp(format_capacity(formatted,sizeof(formatted),1000.23,true),"Wh")==0);assert(std::strcmp(formatted,"1.000")==0);

 const double currents[]={0,9.999,99.999,99.9995,100,320};
 const char* expected[]={"0.000A","9.999A","99.999A","100.00A","100.00A","320.00A"};
 for(size_t i=0;i<6;++i){
   UI::format_fixed_digits(formatted,sizeof(formatted),currents[i],"A",5,3,false);
   assert(std::strcmp(formatted,expected[i])==0);
   assert(UI::text_width(formatted,DENGB44_NUM)<=161);
 }
 DashboardPage main;main.render(RenderMode::Full);snapshot("01-main");
 BatteryPage energy;energy.render(RenderMode::Full);snapshot("02-energy");
 for(unsigned i=0;i<61;++i){ticks=i*500;state.voltage_mV=uint16_t(15000+5000*std::sin(i/8.0));CurveHistory::instance().poll(ticks);}
 CurvePage curve;curve.render(RenderMode::Full);snapshot("03-curve");
 curve.display_mode_=CurvePage::DisplayMode::All;curve.render(RenderMode::Full);snapshot("07-curve-all");
 WirelessPage wireless;wireless.render(RenderMode::Full);snapshot("04-network");
 SettingsPage settings;settings.render(RenderMode::Full);snapshot("05-settings");
 for(uint8_t i=0;i<SettingsPage::ITEM_COUNT;++i){
   settings.selected_=i;
   assert(UI::text_width(settings.item_name(i),DENGB16)<=208);
   assert(UI::text_width(settings.item_value(i),DENGB16)<=212);
   settings.render(RenderMode::Full);char path[40];snprintf(path,sizeof(path),"settings-%02u",i);snapshot(path);
 }
 settings.selected_=SettingsPage::FirmwareInfo;
 settings.draw_dialog_overlay();snapshot("08-dialog");
 fill_screen(BLACK);draw_image((WIDTH-START_LOGO_WIDTH)/2,(HEIGHT-START_LOGO_HEIGHT)/2,START_LOGO_WIDTH,START_LOGO_HEIGHT,start_logo_data);snapshot("06-boot");
 WifiService::ssid=std::string(32,'W');WifiService::ip={255,255,255,255};wireless.render(RenderMode::Full);snapshot("09-long-network");
 state.voltage_mV=65535;state.current_uA=INT32_MAX;state.board_temperature=-4000;ticks=UINT32_MAX;
 state.flags.output_enabled=false;
 state.protect_states.states_bit={PROTECT_STATE_NORMAL,PROTECT_STATE_NORMAL,PROTECT_STATE_NORMAL,PROTECT_STATE_NORMAL};
 main.render(RenderMode::Full);snapshot("10-large-values");
 EnergyMeter::meter.energy_uwh=INT64_MAX;EnergyMeter::meter.charge_uah=INT64_MAX;EnergyMeter::meter.meter_time_ms=UINT64_MAX;
 energy.render(RenderMode::Full);snapshot("11-large-energy");
 // Normal protections really are blank; no inactive sample tag from the Figma draft.
 main.render(RenderMode::Full);
 for(int y=29;y<102;++y)for(int x=174;x<234;++x)assert(double_buffer.data[0][y*WIDTH+x]==0);
 for(auto p:double_buffer.data[1]) assert(p==0xA55A);
 puts("PASS: clipping, ASCII fallback, font subset, bounded labels, protection states and all settings labels");
}
'''
    (out / 'display_check.cpp').write_text(cpp, encoding='utf-8')
    includes = [out, ROOT / 'components/bsp/st7789_driver/include',
                ROOT / 'components/assets/Fonts/Font_include', ROOT / 'components/assets/ui_resources/include',
                ROOT / 'components/app/screen/private_include']
    executable = out / ('display_check.exe' if os.name == 'nt' else 'display_check')
    subprocess.run([args.cxx, '-std=c++17', '-O1', '-g', *['-I'+str(p) for p in includes],
                    str(out / 'display_check.cpp'), str(ROOT / 'components/app/screen/src/widgets/ui_chrome.cpp'),
                    *map(str, (ROOT / 'components/assets/Fonts/Font_src').glob('*.cpp')),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], cwd=out, check=True)
    sheet = Image.new('RGB', (3 * 504, 2 * 320), '#12151A')
    draw = ImageDraw.Draw(sheet)
    for i, path in enumerate(sorted(out.glob('0[1-6]-*.ppm'))):
        img = Image.open(path)
        img.save(path.with_suffix('.png'))
        x, y = (i % 3) * 504 + 12, (i // 3) * 320 + 32
        sheet.paste(img.resize((480, 270), Image.Resampling.NEAREST), (x, y))
        draw.text((x, y-22), path.stem, fill='white')
    for path in out.glob('*.ppm'):
        Image.open(path).save(path.with_suffix('.png'))
    sheet.save(out / 'ui-preview.png')
    print('Preview:', out / 'ui-preview.png')


if __name__ == '__main__':
    main()
