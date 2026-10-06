#include "loc_client.h"
#include "fake_modem.h"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <thread>
using namespace a6l;
using namespace std::chrono_literals;
struct Listener : EngineListener {
    std::atomic<int> ended{0};
    void onSessionState(bool started) override { if (!started) ++ended; }
};
int main() {
    auto f = std::make_shared<FakeModem>(); Listener l;
    std::atomic<bool> receiver{false};
    f->onRequest = [&](FakeModem& modem, const qmi::Message& r) {
        std::lock_guard<std::mutex> lock(modem.mu);
        if (modem.errors.count(r.msgId) && modem.errors[r.msgId]) return;
        if (r.msgId == loc::kStart) receiver = true;
        if (r.msgId == loc::kStop) receiver = false;
    };
    EngineConfig cfg; cfg.retryMinMs = 50; cfg.retryMaxMs = 100;
    GnssEngine e([f]{ return std::make_unique<FakeModemTransport>(f); }, &l, cfg, nullptr);
    e.begin(); e.setActive(true);
    assert(f->waitFor([&]{return e.sessionRunning() && receiver;}, 3000));
    { std::lock_guard<std::mutex> lock(f->mu); f->errors[loc::kStop] = 3; }
    e.setActive(false);
    assert(f->waitFor([&]{return f->count(loc::kStop) == 1 && !e.sessionRunning();}, 3000));
    { std::lock_guard<std::mutex> lock(f->mu); f->errors.erase(loc::kStop); }
    e.setActive(false);
    std::this_thread::sleep_for(1200ms);
    assert(receiver && !e.sessionRunning() && f->count(loc::kStop) == 1);
    printf("STOP_REJECTED engine_running=0 fake_receiver_running=1 stop_requests=1 after_recovery_and_second_stop\n");
    e.end();
    assert(receiver && f->count(loc::kStop) == 1);
    puts("END_AFTER_REJECTED_STOP no_cleanup_stop=1");

    // Independently exercise the existing listener path; no modem service loss.
    auto f2 = std::make_shared<FakeModem>(); Listener l2;
    GnssEngine e2([f2]{return std::make_unique<FakeModemTransport>(f2);}, &l2, cfg, nullptr);
    e2.begin(); e2.setActive(true);
    assert(f2->waitFor([&]{return e2.sessionRunning();}, 3000));
    qmi::Message m; m.type = qmi::kIndication; m.msgId = loc::kIndFixSessionState;
    m.add(1, qmi::Writer().u32(2)); f2->inject(m);
    assert(f2->waitFor([&]{return l2.ended == 1;}, 1000));
    std::this_thread::sleep_for(1200ms);
    assert(e2.sessionRunning() && f2->count(loc::kStart) == 1);
    puts("SESSION_END_IND listener_ended=1 engine_running=1 restart_requests=0");
    e2.end();
}
