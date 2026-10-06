// SPDX-License-Identifier: Apache-2.0
// Host-test stub of android-base/properties.h (agent ril3).
#pragma once
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace hoststub {
inline std::mutex& propLock() { static std::mutex m; return m; }
inline std::map<std::string, std::string>& props() { static std::map<std::string, std::string> p; return p; }
}  // namespace hoststub

namespace android::base {
inline std::string GetProperty(const std::string& k, const std::string& d) {
    std::lock_guard<std::mutex> l(hoststub::propLock());
    auto it = hoststub::props().find(k);
    return it == hoststub::props().end() ? d : it->second;
}
inline bool GetBoolProperty(const std::string& k, bool d) {
    auto v = GetProperty(k, "");
    if (v == "1" || v == "true" || v == "y" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "n" || v == "no" || v == "off") return false;
    return d;
}
template <typename T>
T GetIntProperty(const std::string& k, T d) {
    auto v = GetProperty(k, "");
    return v.empty() ? d : static_cast<T>(strtoll(v.c_str(), nullptr, 0));
}
inline bool SetProperty(const std::string& k, const std::string& v) {
    std::lock_guard<std::mutex> l(hoststub::propLock());
    hoststub::props()[k] = v;
    return true;
}
}  // namespace android::base
