#include "output_host_runtime.h"
#include "power_output.h"
#include "short_circuit_detect.h"
#include <cstdio>
#include <cstdlib>
#include <future>

using namespace PowerOutput;
using R=OutputResult;
using O=OutputOperation;
constexpr char SOURCE[]="host_test";

std::future<R> start(O op=O::ON) {
    auto promise=std::make_shared<std::promise<R>>();
    auto future=promise->get_future();
    request(op,SOURCE,[promise](R result,bool){promise->set_value(result);});
    return future;
}
R finish(std::future<R>& future) {
    assert(future.wait_for(2s)==std::future_status::ready);
    return future.get();
}
void off_and_cool() { assert(off(SOURCE)==R::OK); Host::cool(); }

int main() {
    assert(on(SOURCE)==R::FAIL_NOT_INIT);
    assert(init(19)==ESP_OK);
    // 通知回调可读取快照，证明唤醒不在事务锁内执行。
    set_event_notifier([]{ (void)snapshot(); });
    assert(on(SOURCE)==R::OK);
    assert(Host::samples==3 && Host::enables==1 && !Host::pulse);
    const int initial_pulses=Host::pulses;
    assert(on(SOURCE)==R::OK && Host::pulses==initial_pulses);
    ShortCircuitDetect::Result measured{};
    assert(test_short_circuit(measured,SOURCE)==R::FAIL_BUSY);
    assert(off(SOURCE)==R::OK);
    assert(on(SOURCE)==R::FAIL_COOLDOWN_ACTIVE);
    auto cooldown_status=snapshot();
    assert(cooldown_status.result==R::FAIL_COOLDOWN_ACTIVE && !cooldown_status.output_on);
    assert(cooldown_status.cooldown_remaining_ms>0 && cooldown_status.cooldown_remaining_ms<=500);
    Host::cool();

    Host::voltage=5;
    assert(on(SOURCE)==R::FAIL_SHORT_CIRCUIT);
    assert(!get_state() && !Host::pulse);
    FailureNotice notice;
    assert(take_failure_notice(notice) && notice.result==R::FAIL_SHORT_CIRCUIT);
    assert(notice.voltage_mV==5 && notice.threshold_mV==200);
    assert(!take_failure_notice(notice));
    assert(test_short_circuit(measured,SOURCE)==R::FAIL_SHORT_CIRCUIT && measured.sample_count>=3);

    // Equality passes; one low reading resets the run, not just its average.
    Host::voltage=200;
    Host::voltage_sequence={200,200,199,200,200,200};
    assert(test_short_circuit(measured,SOURCE)==R::OK && measured.sample_count==6);
    assert(measured.voltage_mV==200 && measured.min_voltage_mV==199);
    // Transient ADC errors are retried, counted, and must not break the good run.
    Host::voltage_sequence={200,200,200,200,200};
    Host::adc_error_sequence={ESP_OK,ESP_OK,ESP_FAIL,ESP_OK,ESP_OK,ESP_OK};
    assert(test_short_circuit(measured,SOURCE)==R::OK && measured.sample_count==3 && measured.invalid_count==1);
    assert(measured.min_voltage_mV==200);
    // A negative ADC result is also invalid and retried before reporting failure.
    Host::voltage_sequence={-1,200,200,200};
    assert(test_short_circuit(measured,SOURCE)==R::OK && measured.sample_count==3);
    // Charging beyond the old 250ms transaction timeout is now supported.
    Host::voltage_sequence={5,5};
    Host::gate_on();
    const auto charging_started=std::chrono::steady_clock::now();
    auto charging=std::async(std::launch::async,[]{return on(SOURCE);});
    Host::gate_wait();
    std::this_thread::sleep_for(280ms);
    Host::gate_release();
    assert(finish(charging)==R::OK);
    assert(std::chrono::steady_clock::now()-charging_started>=280ms);
    off_and_cool();
    // Repeated pairs of good samples must never count as three consecutive.
    for (int i=0;i<60;++i) Host::voltage_sequence.push_back(i%3==2 ? 5 : 200);
    const auto short_started=std::chrono::steady_clock::now();
    assert(on(SOURCE)==R::FAIL_SHORT_CIRCUIT);
    const auto short_elapsed=std::chrono::steady_clock::now()-short_started;
    assert(short_elapsed>=490ms && short_elapsed<650ms && !Host::pulse);
    Host::voltage_sequence.clear();
    Host::voltage=5;

    Host::bypass=true;
    const int bypass_pulses=Host::pulses;
    assert(on(SOURCE)==R::OK && Host::pulses==bypass_pulses);
    off_and_cool();
    // A bypass removed between admission and commit must require a measurement.
    Host::disable_bypass_on_idle=true;
    assert(on(SOURCE)==R::FAIL_SHORT_CIRCUIT);
    Host::voltage=2100;
    Host::adc_error=true;
    assert(on(SOURCE)==R::FAIL_SHORT_DETECT && !Host::pulse && !get_state());
    assert(take_failure_notice(notice) && notice.result==R::FAIL_SHORT_DETECT);
    Host::adc_error=false;
    Host::voltage_sequence={-1,-1,-1};
    assert(on(SOURCE)==R::FAIL_SHORT_DETECT && !Host::pulse && !get_state());
    assert(take_failure_notice(notice) && notice.result==R::FAIL_SHORT_DETECT);
    Host::disable_error=true;
    assert(on(SOURCE)==R::FAIL_SHORT_DETECT && !get_state());
    Host::bypass=true;
    assert(on(SOURCE)==R::FAIL_SHORT_DETECT && !get_state());
    Host::disable_error=false;
    assert(on(SOURCE)==R::OK && !Host::pulse);
    off_and_cool();
    Host::bypass=false;

    // Pause the real detector inside ADC: OFF/TOGGLE must not wait for the ADC.
    for (O cancel : {O::OFF,O::TOGGLE}) {
        Host::gate_on();
        auto future=start();
        Host::gate_wait();
        const auto checking=snapshot();
        assert(checking.checking && !checking.output_on && checking.result==R::PENDING);
        assert(request(O::ON,SOURCE)==R::FAIL_BUSY);
        assert(snapshot().request_id==checking.request_id && snapshot().checking);
        assert(test_short_circuit(measured,SOURCE)==R::FAIL_BUSY);
        assert(request(cancel,SOURCE)==R::OK);
        const auto cancelled_status=snapshot();
        assert(!cancelled_status.checking && !cancelled_status.output_on);
        Host::gate_release();
        assert(finish(future)==R::FAIL_CANCELLED);
        assert(snapshot().request_id==cancelled_status.request_id && snapshot().result==R::OK);
        assert(!get_state() && !Host::pulse);
    }
    // Existing protection callbacks cancel even while the output is still OFF.
    Host::gate_on();
    auto protected_request=start();
    Host::gate_wait();
    Host::fault=true;
    Host::protect_callback(0,PROTECT_STATE_PROTECT);
    Host::gate_release();
    assert(finish(protected_request)==R::FAIL_CANCELLED);
    assert(on(SOURCE)==R::FAIL_PROTECT_ACTIVE);
    Host::fault=false;

    // Manual diagnostics reserve the same transaction; no parallel ON may pass.
    Host::gate_on();
    auto manual=std::async(std::launch::async,[&]{return test_short_circuit(measured,SOURCE);});
    Host::gate_wait();
    assert(request(O::ON,SOURCE)==R::FAIL_BUSY);
    assert(ShortCircuitDetect::test(measured)==ESP_ERR_INVALID_STATE);
    Host::gate_release();
    assert(finish(manual)==R::OK && !get_state());

    // Enabling bypass during measurement skips a SHORT verdict after cleanup.
    Host::voltage=5;
    Host::gate_on();
    auto bypass_changed=start();
    Host::gate_wait();
    Host::bypass=true;
    Host::gate_release();
    assert(finish(bypass_changed)==R::OK && get_state() && !Host::pulse);
    off_and_cool();
    Host::bypass=false;

    // Threshold changes do not change the verdict halfway through one test.
    Host::voltage=100;
    Host::gate_on();
    auto threshold_request=start();
    Host::gate_wait();
    assert(ShortCircuitDetect::set_threshold_mV(50)==ESP_OK);
    Host::gate_release();
    assert(finish(threshold_request)==R::FAIL_SHORT_CIRCUIT);
    assert(ShortCircuitDetect::set_threshold_mV(200)==ESP_OK);
    Host::voltage=2100;

    // Timeout returns without dangling waiter storage and can never enable later.
    Host::gate_on();
    auto timeout=std::async(std::launch::async,[]{return on(SOURCE);});
    Host::gate_wait();
    assert(finish(timeout)==R::FAIL_TIMEOUT);
    assert(!snapshot().checking && snapshot().result==R::FAIL_TIMEOUT);
    assert(take_failure_notice(notice) && notice.result==R::FAIL_TIMEOUT);
    assert(request(O::ON,SOURCE)==R::FAIL_BUSY);
    Host::gate_release();
    R retry=R::FAIL_BUSY;
    for (int i=0;i<100 && retry==R::FAIL_BUSY;++i) {
        std::this_thread::sleep_for(2ms);
        // A diagnostic request proves the timed-out slot has been released,
        // without introducing a new ON into the no-late-enable assertion.
        retry=test_short_circuit(measured,SOURCE);
    }
    assert(retry==R::OK && !get_state() && !Host::pulse);
    assert(!take_failure_notice(notice)); // 超时弹窗已消费，清理完成不再重弹。

    // Async requests also have a deadline if the worker stalls.
    Host::gate_on();
    auto expired=start();
    Host::gate_wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(REQUEST_TIMEOUT_MS+20));
    Host::gate_release();
    assert(finish(expired)==R::FAIL_TIMEOUT && !get_state());

    // Completion runs outside the lock and may synchronously turn OFF.
    std::promise<R> reentry;
    auto reentry_future=reentry.get_future();
    request(O::ON,SOURCE,[&](R result,bool){assert(result==R::OK);reentry.set_value(off(SOURCE));});
    assert(finish(reentry_future)==R::OK && !get_state());
    Host::cool();
    Host::output_error=true;
    assert(on(SOURCE)==R::FAIL_GPIO && !get_state());
    Host::output_error=false;

    Host::gate_on();
    auto stopped=start();
    Host::gate_wait();
    assert(deinit()==ESP_OK);
    assert(init(19)==ESP_ERR_INVALID_STATE);
    Host::gate_release();
    assert(finish(stopped)==R::FAIL_CANCELLED && !get_state() && !Host::pulse);
    assert(init(19)==ESP_OK);
    assert(on(SOURCE)==R::OK);
    assert(deinit()==ESP_OK && !get_state());
    puts("PASS: real output/detector sources: pulse ordering, bypass, faults, cooldown, cancellation, timeout, callbacks, lifecycle");
    fflush(stdout);
    // Firmware worker intentionally lives forever; avoid static teardown races.
    std::_Exit(0);
}
