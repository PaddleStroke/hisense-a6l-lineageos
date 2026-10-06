// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL, r5 review fix F6 (28 Sep 2026): a6l-q6voiced mute client (see VoiceMute.h).
#include "VoiceMute.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>

namespace android::hardware::radio::a6l {

VoiceMuteReply parseQ6voicedReply(const std::string& line) {
    VoiceMuteReply r;
    r.text = line;
    if (line == "OK mute=0" || line == "OK mute=1") {
        r.kind = VoiceMuteReply::Ok;
        r.mute = line.back() == '1';
        return r;
    }
    if (line.rfind("ERR ", 0) == 0) {
        r.err = atoi(line.c_str() + 4);
        if (r.err == ENOENT || r.err == EOPNOTSUPP || r.err == ENOTTY)
            r.kind = VoiceMuteReply::Unsupported;  // kernel without the "VoiceMMode1 TX Mute" control
        else if (r.err == EINVAL)
            r.kind = VoiceMuteReply::BadRequest;
        else
            r.kind = VoiceMuteReply::DspError;  // EIO (DSP refused), ETIMEDOUT (APR), ENODEV (no card)
        return r;
    }
    r.kind = VoiceMuteReply::Protocol;
    return r;
}

VoiceMuteReply q6voicedRequest(const std::string& cmd, const char* path, int timeoutMs) {
    VoiceMuteReply r;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        r.err = errno;
        r.text = std::string("socket: ") + strerror(r.err);
        return r;
    }
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        r.err = errno;
        r.text = std::string("connect ") + path + ": " + strerror(r.err);
        close(fd);
        return r;
    }
    std::string line = cmd + "\n";
    if (write(fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
        r.err = errno;
        r.text = std::string("write: ") + strerror(r.err);
        close(fd);
        return r;
    }
    std::string in;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (in.find('\n') == std::string::npos) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd p{fd, POLLIN, 0};
        if (left.count() <= 0 || poll(&p, 1, static_cast<int>(left.count())) <= 0) {
            r.kind = VoiceMuteReply::Protocol;
            r.err = ETIMEDOUT;
            r.text = "no reply from a6l-q6voiced";
            close(fd);
            return r;
        }
        char buf[128];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        in.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    auto nl = in.find('\n');
    if (nl == std::string::npos) {
        r.kind = VoiceMuteReply::Protocol;
        r.err = EPROTO;
        r.text = "truncated reply '" + in + "'";
        return r;
    }
    return parseQ6voicedReply(in.substr(0, nl));
}

}  // namespace android::hardware::radio::a6l
