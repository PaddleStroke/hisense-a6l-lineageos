// Offline regression cases independent of the happy-path volte5 fake.
#include "fake_modem.h"
#include <a6lqmi/ims.h>
#include <a6lqmi/ims_setup.h>
#include <cstdio>
using namespace a6l;
using namespace a6l::qmi;
using namespace a6l::test;
static int pass, fail;
static void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    (ok ? pass : fail)++;
}
int main() {
    const std::string reason = "Forbidden";
    for (auto type : {MsgType::Response, MsgType::Indication}) {
        Message m(type, type == MsgType::Response ? imsa::kGetRegStatus : imsa::kRegStatusInd);
        m.raw(type == MsgType::Response ? 0x13 : 0x12,
              std::vector<uint8_t>(reason.begin(), reason.end()));
        m.u16(type == MsgType::Response ? 0x11 : 0x10, 403);
        auto s = imsa::parseRegStatus(m);
        check(s.errorText == reason, type == MsgType::Response ? "response raw error text" : "indication raw error text");
        check(s.errorCode && *s.errorCode == 403, "numeric rejection code preserved");
    }
    FakeModem fm;
    fm.addService(kSvcImss); fm.addService(kSvcImsa);
    fm.setHandler([](uint32_t svc, const Message& m) -> std::optional<Message> {
        if ((svc == kSvcImss && m.msgId == imss::kBindSubscription) ||
            (svc == kSvcImsa && m.msgId == imsa::kBindSubscription))
            return errResponse(m.msgId, kErrInvalidOperation);
        return okResponse(m.msgId); // a default/old subscription may still answer
    });
    Client c(fm.transport(), "review");
    check(c.start({kSvcImss, kSvcImsa}), "client starts");
    check(c.waitForServices({kSvcImss, kSvcImsa}, 2000).empty(), "services present");
    auto o = radio::imssSetup(c, 0u, radio::ImssMode::Enable, {});
    check(!o.bound, "failed IMSS bind reported");
    check(!o.wrote, "failed IMSS bind prevents settings write");
    check(fm.requests().size() == 1, "failed IMSS bind stops setup");
    size_t before = fm.requests().size();
    auto a = radio::imsaAttach(c, 0u, {});
    check(!a.bound, "failed IMSA bind reported");
    check(fm.requests().size() == before + 1, "failed IMSA bind stops attach");
    c.stop();
    std::printf("REVIEW_IMS pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}
