// SPDX-License-Identifier: Apache-2.0
// Host-test stub of android-base/logging.h (agent ril3): enough for hal/ModemCore.cpp.
#pragma once
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace hoststub {
inline std::mutex& logLock() { static std::mutex m; return m; }
inline std::vector<std::string>& logLines() { static std::vector<std::string> v; return v; }
inline bool& logEcho() { static bool b = false; return b; }
inline bool logContains(const std::string& needle) {
    std::lock_guard<std::mutex> l(logLock());
    for (auto& s : logLines())
        if (s.find(needle) != std::string::npos) return true;
    return false;
}
class LogLine {
  public:
    explicit LogLine(const char* sev) { mS << sev << ": "; }
    ~LogLine() {
        std::lock_guard<std::mutex> l(logLock());
        logLines().push_back(mS.str());
        if (logEcho()) fprintf(stderr, "%s\n", mS.str().c_str());
    }
    template <typename T>
    LogLine& operator<<(const T& v) {
        mS << v;
        return *this;
    }

  private:
    std::ostringstream mS;
};
}  // namespace hoststub

#define LOG(sev) ::hoststub::LogLine(#sev)
