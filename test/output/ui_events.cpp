#include "ui_host_runtime.h"
#include "core/ui_manager.h"
#include "core/page_registry.h"
#include "core/ui_schedule.h"
#include <cstdio>

namespace PowerOutput {
bool notice_pending=false, output_on=false;
FailureNotice next_notice{OutputResult::FAIL_SHORT_CIRCUIT,5,200,ESP_OK};
void (*event_notifier)()=nullptr;
void set_event_notifier(void (*callback)()){event_notifier=callback;}
bool take_failure_notice(FailureNotice& notice){
    if(!notice_pending)return false;
    notice_pending=false;notice=next_notice;return true;
}
bool get_state(){return output_on;}
Status test_status;
Status snapshot(){auto s=test_status;s.initialized=true;s.output_on=output_on;return s;}
OutputResult request(OutputOperation,const char*,CompletionCallback){++UiHost::requests;return OutputResult::PENDING;}
void fail(){notice_pending=true;if(event_notifier)event_notifier();}
}
namespace SCREEN {
class FakePage:public Page {
    PageId value;
  public:
    explicit FakePage(PageId id):value(id){}
    PageId id()const override{return value;}
    const char* title()const override{return "fake";}
    uint32_t refresh_interval_ms()const override{return UiHost::periods[static_cast<int>(value)];}
    bool handle_button(ButtonId,ButtonEvent)override{++UiHost::page_events;return false;}
    void render(RenderMode)override{
        UiHost::dialog=false;
        ++UiHost::page_renders;
        UiHost::current_page=static_cast<int>(value);
        UiHost::render_times.push_back(Host::ui_ticks);
        Host::ui_ticks+=UiHost::render_cost_ms;
    }
};
PageRegistry get_page_registry(){
    static FakePage a(PageId::Dashboard),b(PageId::Battery),c(PageId::Curve),d(PageId::Wireless),e(PageId::Settings);
    static Page* pages[]={&a,&b,&c,&d,&e};return {pages,5};
}
}
int main(){
    using namespace SCREEN;
    HostTask screen;
    current_task=&screen;
    auto& ui=UIManager::instance();assert(ui.init());
    for(auto button:{ButtonId::Main,ButtonId::Side,ButtonId::Previous}){
        for(auto event:{ButtonEvent::PRESS,ButtonEvent::SHORT_PRESS,ButtonEvent::DOUBLE_CLICK,ButtonEvent::LONG_PRESS}){
            PowerOutput::fail();ui.loop_once();assert(UiHost::dialog);
            const int requests=UiHost::requests,events=UiHost::page_events;
            assert(ui.post_button_event(button,event));
            PowerOutput::fail();ui.loop_once();
            assert(!UiHost::dialog && UiHost::requests==requests && UiHost::page_events==events);
        }
    }
    assert(ui.post_button_event(ButtonId::Main,ButtonEvent::PRESS));
    PowerOutput::fail();ui.loop_once();
    assert(!UiHost::dialog && UiHost::requests==0);

    PowerOutput::fail();ui.loop_once();
    for(int i=0;i<20;++i)assert(ui.post_button_event(ButtonId::Side,ButtonEvent::SHORT_PRESS));
    ui.loop_once();assert(!UiHost::dialog && UiHost::requests==0 && UiHost::page_events==0);

    // The five page periods remain independent. Idle waits hit deadlines,
    // instead of consuming 5ms polling loops.
    for(int page=0;page<5;++page){
        assert(UiHost::current_page==page);
        const auto previous=UiHost::render_times.back();
        // History may have an earlier deadline; such a wake need not redraw.
        for(int wake=0;wake<8 && UiHost::render_times.back()==previous;++wake)ui.loop_once();
        assert(UiHost::render_times.back()-previous==UiHost::periods[page]);
        if(page!=4){
            assert(ui.post_button_event(ButtonId::Side,ButtonEvent::SHORT_PRESS));
            ui.loop_once();
        }
    }

    // While blocked for a long page deadline, a real notification wakes early.
    const auto before=Host::ui_ticks;
    const int requests=UiHost::requests;
    Host::notification_wait_hook=[&](TickType_t timeout){
        assert(timeout>7);
        Host::notification_wait_hook={};
        Host::ui_ticks+=7;
        assert(ui.post_button_event(ButtonId::Main,ButtonEvent::PRESS));
    };
    ui.loop_once();
    assert(UiHost::requests==requests+1 && Host::ui_ticks-before<20);

    // Modal paints once, coalesces identical failures, and keeps history alive.
    PowerOutput::fail();ui.loop_once();assert(UiHost::dialog);
    const int modal_frames=UiHost::transfers, backgrounds=UiHost::page_renders;
    const size_t history_before=UiHost::history_times.size();
    for(int i=0;i<3;++i)ui.loop_once();
    assert(UiHost::transfers==modal_frames && UiHost::page_renders==backgrounds);
    assert(UiHost::history_times.size()>=history_before+3);
    PowerOutput::fail();ui.loop_once();assert(UiHost::transfers==modal_frames);
    PowerOutput::next_notice.voltage_mV=10;
    PowerOutput::fail();ui.loop_once();
    assert(UiHost::transfers==modal_frames+1 && UiHost::page_renders==backgrounds);

    // Remote success clears a modal without waiting for the 500ms history tick.
    PowerOutput::output_on=true;PowerOutput::event_notifier();
    ui.loop_once();assert(!UiHost::dialog);
    PowerOutput::output_on=false;

    // Slow frames and even forced redraw floods obey measured work/recovery
    // budgets. Buttons generated by a lower-priority task can still run.
    UiHost::render_cost_ms=90;
    ui.request_redraw();ui.loop_once();
    int lower_priority_runs=0;
    bool button_pending=true;
    PowerOutput::fail();ui.loop_once();
    Host::delay_hook=[&](TickType_t ticks){
        assert(ticks>=30);
        ++lower_priority_runs;
        if(button_pending){
            button_pending=false;
            assert(ui.post_button_event(ButtonId::Previous,ButtonEvent::SHORT_PRESS));
        }
    };
    ui.loop_once();
    assert(lower_priority_runs>0 && !UiHost::dialog);
    Host::delay_hook={};
    for(int i=0;i<5;++i){
        const auto previous=UiHost::render_times.back();
        ui.request_redraw();ui.loop_once();
        assert(UiHost::render_times.back()-previous>=120);
    }

    // Pure deadline math: missed frames are skipped, wraparound stays valid.
    assert(UiSchedule::next_frame(100,190,67)==234);
    assert(UiSchedule::next_frame(UINT32_MAX-9,15,20)==30);
    assert(UiSchedule::remaining(UINT32_MAX-9,5)==15);
    assert(UiSchedule::due(7,5));
    assert(UiSchedule::recovery(90)==30);
    // 主按键按下即提交输出切换，不等待双击窗口；释放只结束高亮。
    UiHost::render_cost_ms=2;
    const int before_press=UiHost::requests;
    assert(ui.post_button_event(ButtonId::Main,ButtonEvent::PRESS));
    ui.loop_once();assert(UI::output_view().pressed && UiHost::requests==before_press+1);
    assert(ui.post_button_event(ButtonId::Main,ButtonEvent::RELEASE));
    ui.loop_once();assert(UI::output_view().pressed && UiHost::requests==before_press+1);
    // 双击/长按事件不触发主按键动作。
    assert(ui.post_button_event(ButtonId::Main,ButtonEvent::DOUBLE_CLICK));
    ui.loop_once();assert(!UI::output_view().pressed && UiHost::requests==before_press+1);

    PowerOutput::test_status.request_id=1;
    PowerOutput::test_status.checking=true;
    PowerOutput::test_status.result=PowerOutput::OutputResult::PENDING;
    PowerOutput::event_notifier();ui.loop_once();
    assert(UI::output_view().state==UI::OutputVisual::Checking);
    const auto check_started=Host::ui_ticks;
    ui.loop_once();assert(Host::ui_ticks-check_started<=102);
    assert(UI::output_view().state==UI::OutputVisual::Checking);
    assert(ui.post_button_event(ButtonId::Side,ButtonEvent::SHORT_PRESS));
    ui.loop_once();assert(UI::output_view().state==UI::OutputVisual::Checking);
    PowerOutput::test_status.checking=false;
    PowerOutput::test_status.result=PowerOutput::OutputResult::OK;
    PowerOutput::output_on=true;
    PowerOutput::event_notifier();ui.loop_once();
    assert(UI::output_view().state==UI::OutputVisual::On);

    // 冷却到期只能回到 OFF；纯模型无输出调用，提示过期和计时回绕可验证。
    OutputFeedback feedback;
    PowerOutput::Status status;
    status.initialized=true;status.request_id=1;
    status.result=PowerOutput::OutputResult::FAIL_COOLDOWN_ACTIVE;
    status.cooldown_remaining_ms=350;
    auto view=feedback.update(status,UINT32_MAX-100,false);
    assert(view.state==UI::OutputVisual::Wait && std::strcmp(view.detail,"WAIT 0.4s")==0);
    status.cooldown_remaining_ms=0;
    view=feedback.update(status,300,false);
    assert(view.state==UI::OutputVisual::Off && !view.detail[0]);
    status.request_id=2;status.result=PowerOutput::OutputResult::FAIL_PROTECT_ACTIVE;
    status.protect_mask=8;
    view=feedback.update(status,400,false);
    assert(view.state==UI::OutputVisual::Locked && std::strcmp(view.detail,"OCP ACTIVE")==0);
    view=feedback.update(status,2500,false);
    assert(view.state==UI::OutputVisual::Locked && !view.detail[0]);
    status.request_id=3;status.result=PowerOutput::OutputResult::OK;
    status.bypassed=true;status.output_on=true;
    view=feedback.update(status,2600,false);
    assert(view.state==UI::OutputVisual::On && view.bypassed && !view.detail[0]);
    status.request_id=4;status.result=PowerOutput::OutputResult::FAIL_GPIO;
    view=feedback.update(status,2700,false);
    assert(view.state==UI::OutputVisual::On && std::strcmp(view.detail,"OUTPUT ERROR")==0);
    // 普通关闭后即使冷却计时未清零也显示 OFF。
    status.request_id=5;status.result=PowerOutput::OutputResult::OK;
    status.output_on=false;status.bypassed=false;status.protect_mask=0;
    status.cooldown_remaining_ms=500;
    view=feedback.update(status,2800,false);
    assert(view.state==UI::OutputVisual::Off && !view.detail[0]);
    assert(UiHost::requests==before_press+1);
    puts("PASS: real UIManager: per-page deadlines, early event wake, static modal, history, modal keys, overload budget, tick wrap");
}
