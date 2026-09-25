/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <libminradio/sim/Filesystem.h>

#include <libminradio/sim/IccConstants.h>
#include <libminradio/sim/IccUtils.h>

#include <format>

namespace android::hardware::radio::minimal::sim {

using namespace ::android::hardware::radio::minimal::sim::constants;
using FileView = Filesystem::FileView;

namespace paths {

// 3GPP TS 51.011 10.7
const Filesystem::Path mf{MF_SIM_VAL, ""};
const Filesystem::Path fplmn{EF_FPLMN, MF_SIM + DF_ADF};
const Filesystem::Path iccid{EF_ICCID, MF_SIM};
const Filesystem::Path msisdn{EF_MSISDN, MF_SIM + DF_ADF};
const Filesystem::Path pl{EF_PL, MF_SIM};
const Filesystem::Path arr{EF_ARR, MF_SIM};
const Filesystem::Path ad{EF_AD, MF_SIM + DF_ADF};

}  // namespace paths

Filesystem::Filesystem() {
    write(paths::mf, "");  // Directories are not implemented.
    write(paths::arr, "");
}

void Filesystem::write(const Path& path, FileView contents) {
    std::unique_lock lck(mFilesGuard);
    mFiles[path].assign(contents.begin(), contents.end());  // C++23: assign_range
    mUpdates.insert(path.fileId);
}

void Filesystem::write(const Path& path, std::string_view contents) {
    std::unique_lock lck(mFilesGuard);
    mFiles[path].assign(contents.begin(), contents.end());  // C++23: assign_range
    mUpdates.insert(path.fileId);
}

void Filesystem::write(const Path& path, std::vector<uint8_t>&& contents) {
    write(path, FileView(contents));
}

std::optional<FileView> Filesystem::read(const Path& path) const {
    std::unique_lock lck(mFilesGuard);
    auto it = mFiles.find(path);
    if (it == mFiles.end()) return std::nullopt;

    return FileView(it->second);
}

void Filesystem::writeBch(const Path& path, std::string_view contents) {
    write(path, hexStringToBch(contents));
}

std::optional<std::string> Filesystem::readBch(const Path& path) const {
    auto contents = read(path);
    if (!contents.has_value()) return std::nullopt;
    return bchToHexString(*contents);
}

std::optional<Filesystem::Path> Filesystem::find(uint16_t fileId) {
    std::unique_lock lck(mFilesGuard);
    for (auto& [path, content] : mFiles) {
        if (path.fileId == fileId) return path;
    }
    return std::nullopt;
}

std::set<int32_t> Filesystem::fetchAndClearUpdates() {
    std::unique_lock lck(mFilesGuard);
    std::set<int32_t> result;
    std::swap(result, mUpdates);
    return result;
}

std::string Filesystem::Path::toString() const {
    return std::format("{:s}/{:X}", pathId, fileId);
}

}  // namespace android::hardware::radio::minimal::sim
