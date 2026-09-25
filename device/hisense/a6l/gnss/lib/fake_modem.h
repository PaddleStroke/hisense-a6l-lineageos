// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): in-process fake modem (QRTR name service + QMI LOC server) for the host tests
// and for `a6l_gnss_test --replay` (feeds a recorded/synthesized packet log through the real client/engine code).
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "loc_v02.h"
#include "qmi.h"
#include "transport.h"

namespace a6l {

class FakeModem {
  public:
    static constexpr uint32_t kNode = 0, kPort = 0x4001;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<QrtrAddr, std::vector<uint8_t>>> inbox;
    std::vector<qmi::Message> requests;       // every request received, in order
    std::map<uint16_t, uint16_t> errors;      // msgId -> QMI error code to answer with
    bool failOpen = false;
    bool serviceAtOpen = true;
    bool closed = false;
    int opens = 0;
    std::function<void(FakeModem&, const qmi::Message&)> onRequest;   // called without the lock held

    void pushLocked(const QrtrAddr& a, std::vector<uint8_t> p) {
        inbox.emplace_back(a, std::move(p));
        cv.notify_all();
    }
    void inject(const qmi::Message& ind) {
        std::lock_guard<std::mutex> lk(mu);
        pushLocked(QrtrAddr{kNode, kPort}, qmi::encode(ind));
    }
    void serverEvent(bool up) {
        QrtrCtrl c;
        c.cmd = up ? kQrtrNewServer : kQrtrDelServer;
        c.service = loc::kServiceId;
        c.instance = 2;
        c.node = kNode;
        c.port = kPort;
        std::lock_guard<std::mutex> lk(mu);
        pushLocked(QrtrAddr{1, kQrtrPortCtrl}, makeCtrl(c));
    }
    size_t count(uint16_t msgId) {
        std::lock_guard<std::mutex> lk(mu);
        size_t n = 0;
        for (auto& r : requests) n += r.msgId == msgId;
        return n;
    }
    bool waitFor(std::function<bool()> pred, int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return pred();
    }
};

class FakeModemTransport : public Transport {
  public:
    explicit FakeModemTransport(std::shared_ptr<FakeModem> m) : m_(std::move(m)) {}

    bool open(std::string* err) override {
        std::lock_guard<std::mutex> lk(m_->mu);
        m_->opens++;
        if (m_->failOpen) {
            if (err) *err = "socket(AF_QIPCRTR): Address family not supported by protocol (fake)";
            return false;
        }
        m_->closed = false;
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> lk(m_->mu);
        m_->closed = true;
    }
    bool sendCtrl(const std::vector<uint8_t>& pkt) override {
        QrtrCtrl c;
        if (!parseCtrl(pkt, &c) || c.cmd != kQrtrNewLookup) return false;
        std::lock_guard<std::mutex> lk(m_->mu);
        if (m_->serviceAtOpen && (c.service == 0 || c.service == loc::kServiceId)) {
            QrtrCtrl s;
            s.cmd = kQrtrNewServer;
            s.service = loc::kServiceId;
            s.instance = 2;
            s.node = FakeModem::kNode;
            s.port = FakeModem::kPort;
            m_->pushLocked(QrtrAddr{1, kQrtrPortCtrl}, makeCtrl(s));
        }
        QrtrCtrl end;
        end.cmd = kQrtrNewServer;
        m_->pushLocked(QrtrAddr{1, kQrtrPortCtrl}, makeCtrl(end));
        return true;
    }
    bool send(const QrtrAddr& to, const std::vector<uint8_t>& pkt) override {
        qmi::Message req;
        std::string err;
        if (to.port != FakeModem::kPort || !qmi::decode(pkt.data(), pkt.size(), &req, &err)) return false;
        qmi::Message resp;
        resp.type = qmi::kResponse;
        resp.txn = req.txn;
        resp.msgId = req.msgId;
        {
            std::lock_guard<std::mutex> lk(m_->mu);
            m_->requests.push_back(req);
            uint16_t e = m_->errors.count(req.msgId) ? m_->errors[req.msgId] : 0;
            resp.add(qmi::kResultTlv, qmi::Writer().u16(e ? 1 : 0).u16(e));
            m_->pushLocked(QrtrAddr{FakeModem::kNode, FakeModem::kPort}, qmi::encode(resp));
        }
        if (m_->onRequest) m_->onRequest(*m_, req);
        return true;
    }
    int recv(QrtrAddr* from, std::vector<uint8_t>* pkt, int timeoutMs) override {
        std::unique_lock<std::mutex> lk(m_->mu);
        if (!m_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return !m_->inbox.empty(); }))
            return 0;
        *from = m_->inbox.front().first;
        *pkt = std::move(m_->inbox.front().second);
        m_->inbox.pop_front();
        return 1;
    }

  private:
    std::shared_ptr<FakeModem> m_;
};

}  // namespace a6l
