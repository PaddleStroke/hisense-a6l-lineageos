// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte5, 28 Sep 2026): the AP-side IMS client setup the stock qcril does on
// this DSDS SDM660 modem, reproduced for modem-centric VoLTE (plan B, no Android ImsService):
//   1. IMSS (18) Bind Subscription 0x0098 {0x01 u32 sub}   (ImssModemEndPointModule::handleQmiBinding)
//   2. IMSS Get IMS Service Enable Config 0x0090             (qcril_qmi_imss_get_ims_service_enable_config)
//   3. IMSS Set IMS Service Enable Config 0x008F {0x10 volte=1, 0x18 ims_service_enabled=1} when the
//      modem reports IMS/VoLTE off: what Android's ImsService turnOnIms -> qcril set_ims_registration /
//      set_ims_srv_status do on stock at every boot (the modem does not start IMS registration while
//      ims_service_enabled is 0).
//   4. IMSA (33) Bind Subscription 0x0033 {0x10 u32 sub}, register indications, read reg/services
//      (ImsaModemEndPointModule::handleQmiBinding + qcril_qmi_imsa_init).
// Without the binds, every IMSA/IMSS request answers INVALID_OPERATION (28 Sep attended logs).
#pragma once

#include <a6lqmi/client.h>
#include <a6lqmi/ims.h>

#include <functional>
#include <optional>
#include <string>

namespace a6l::radio {

// volte6 (29 Sep): Force = always write volte=1 + ims_service_enabled=1, even when the modem already reports
// them on. That is what stock does: Android's ImsService calls turnOnIms / setVoLteUserPref after every SIM
// load, and qcril forwards it as IMSS 0x008F without comparing with the modem state. Toggle = write
// ims_service_enabled=0 then 1 (the VoLTE switch off/on in Settings on stock) to make the modem IMS
// re-evaluate. Both write the IMS settings in the modem EFS: RAM copy only (the test script gates it).
enum class ImssMode { Off, Read, Enable, Force, Toggle };
ImssMode imssModeFromString(const std::string& s);  // "off" | "read" (default) | "enable" | "force" | "toggle"
const char* imssModeName(ImssMode m);

using ImsReportFn = std::function<void(const std::string& line)>;

struct ImssOutcome {
    bool bound = false, readOk = false, wrote = false, writeOk = false, readAfterOk = false;
    qmi::imss::ServiceEnableConfig before, after;
    bool enabled() const;  // ims_service_enabled == 1 after the step (or before when no write)
};
// sub: nullopt = do not bind. Lines: "A6L_IMSDCM_IMSS ...".
// toggleGapMs: pause between the 0 and the 1 write of Toggle.
ImssOutcome imssSetup(qmi::Client& c, std::optional<uint32_t> sub, ImssMode mode, const ImsReportFn& report,
                      int toggleGapMs = 2000);

struct ImsaOutcome {
    bool bound = false, indOk = false, regOk = false, svcOk = false;
    qmi::imsa::RegStatus reg;
    qmi::imsa::ServicesStatus services;
};
// Lines: "A6L_IMSDCM_IMSA ..." (same wording as volte2 so the old greps keep working).
ImsaOutcome imsaAttach(qmi::Client& c, std::optional<uint32_t> sub, const ImsReportFn& report);

// ---------------------------------------------------------------- volte6: NAS side of VoLTE
// What decides whether the modem IMS brings up its PDN on LTE, read from NAS (read-only):
//  * Get System Selection Preference 0x0034: resp TLV 0x20 voice_domain_pref (u32: 0 CS only, 1 PS only,
//    2 CS preferred, 3 IMS PS preferred), 0x1F usage_setting (u32: 1 voice centric, 2 data centric), 0x18
//    service domain pref. Offsets checked against the stock qcril (qcril_qmi_nas_get_voice_domain_preference
//    reads the cached 0x0034 response at +0xbc valid / +0xc0 value = IDL resp TLV 0x20 u32@192).
//  * Get System Info 0x004D: 0x14 LTE service status {srv, true_srv, pref}, 0x29 lte_ims_voice_avail (the
//    network's "IMS voice over PS supported" bit from the attach/TAU accept: VoPS), 0x2A lte_voice_domain.
// Stock VoLTE switch: qcril_qmi_ims_map_ims_volte_user_pref_to_qmi_nas_voice_domain_pref maps VoLTE on -> 3
// (IMS PS preferred), off -> 2, and qcril_qmi_nas_set_voice_domain_preference sends Set System Selection
// Preference 0x0033 with only TLV 0x23 = voice_domain_pref (req struct +0xbc valid / +0xc0 value, IDL req
// TLV 0x23 u32@192; no change-duration TLV).
struct NasImsState {
    std::optional<uint32_t> vdp, usage, srvDomain, lteVoiceDomain;
    std::optional<uint8_t> vops, lteSrv, lteTrueSrv;
    bool lteFullService() const { return lteSrv && *lteSrv == 2; }
    std::string summary() const;  // "vdp=3(ims-pref) usage=1(voice) vops=0 lte_voice_domain=3(cs) lte_srv=2"
};
const char* vdpName(uint32_t v);
qmi::Result readNasImsState(qmi::Client& c, NasImsState* out);  // both queries; ok when at least one answered
qmi::Message buildSetVoiceDomainPref(uint32_t vdp);             // 0x0033 {0x23 u32} (stock layout)
enum class VdpMode { Read, Set };                               // A6L_IMSDCM_VDP=read (default) | set
VdpMode vdpModeFromString(const std::string& s);
// Set: when voice_domain_pref != 3, write 3 (stock "VoLTE on"), then read back. Lines "A6L_IMSDCM_NAS ...".
// Returns true when the preference is 3 afterwards.
bool nasVoiceDomainSetup(qmi::Client& c, VdpMode mode, const ImsReportFn& report, NasImsState* state = nullptr);

// ---------------------------------------------------------------- volte6: re-assert policy
// When to re-send the IMS settings (IMSS force/toggle + IMSA re-bind) in a6l-imsdcm:
//  * after a 0x33 SUB_DESTROY_INSTANCE from the modem (29 Sep: GLOBAL instance destroyed, nothing after),
//  * when LTE reaches full service (SIM ready + attached; first time and after each loss),
// each after delayS seconds, at most maxKicks times, never two within delayS of each other.
class ImsKickPolicy {
  public:
    ImsKickPolicy(int maxKicks = 3, int delayS = 3) : mMax(maxKicks), mDelay(delayS) {}
    void onDestroy(uint32_t instance, int nowS);
    void onNas(const NasImsState& st, int nowS);
    // true when a kick is due now (why = reason); the caller then performs it.
    bool due(int nowS, std::string* why);
    int kicks() const { return mKicks; }

  private:
    int mMax, mDelay, mKicks = 0;
    int mDueAt = -1, mLastKick = -1000000;
    std::string mWhy;
    bool mHadFull = false;
    void schedule(int at, const std::string& why);
};

}  // namespace a6l::radio
