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

#include <libminradio/sim/apps/FilesystemApp.h>

#include "tlv.h"

#include <android-base/logging.h>
#include <libminradio/binder_printing.h>
#include <libminradio/sim/IccConstants.h>
#include <libminradio/sim/IccUtils.h>

#include <unordered_set>

namespace android::hardware::radio::minimal::sim::apps {

using namespace ::android::hardware::radio::minimal::binder_printing;
using namespace ::android::hardware::radio::minimal::sim::constants;
namespace aidl = ::aidl::android::hardware::radio::sim;

// ETSI TS 102 221 11.1.1.2 Table 11.1: Coding of P1 for SELECT
static constexpr uint8_t SELECT_BY_FILE_ID = 0x00;

// ETSI TS 102 221 11.1.1.2 Table 11.2: Coding of P2 for SELECT
static constexpr uint8_t SELECT_RETURN_FCP_TEMPLATE = 0x04;
static constexpr uint8_t SELECT_RETURN_NOTHING = 0x0C;

// From android.carrierapi.cts.FcpTemplate
static constexpr uint8_t BER_TAG_FCP_TEMPLATE = 0x62;
static constexpr uint8_t FILE_IDENTIFIER = 0x83;

static const std::unordered_set<int32_t> kLinearFixedFiles{EF_MSISDN};

class FilesystemApp::FilesystemChannel : public App::Channel {
  public:
    FilesystemChannel(int32_t channelId, std::shared_ptr<Filesystem> filesystem);

    void select(Filesystem::Path path);
    aidl::IccIoResult transmit(const aidl::SimApdu& message) override;

  private:
    std::shared_ptr<Filesystem> mFilesystem;
    Filesystem::Path mSelectedFile = paths::mf;

    aidl::IccIoResult commandSelect(int32_t p1, int32_t p2, int32_t p3, const std::string& data);
    aidl::IccIoResult commandStatus(int32_t p1) const;
    aidl::IccIoResult commandReadBinary(int32_t p1, int32_t p2) const;
    aidl::IccIoResult commandUpdateBinary(int32_t p1, int32_t p2, std::string_view data);
    aidl::IccIoResult commandReadRecord(int32_t p1, int32_t p2, int32_t p3);
    aidl::IccIoResult commandGetResponse() const;
};

FilesystemApp::FilesystemApp(const std::shared_ptr<Filesystem>& filesystem)
    : App(AID), mFilesystem(filesystem) {}

std::shared_ptr<App::Channel> FilesystemApp::newChannel(int32_t id) {
    auto channel = std::make_shared<FilesystemApp::FilesystemChannel>(id, mFilesystem);
    if (id == 0) mBasicChannel = channel;
    return channel;
}

FilesystemApp::FilesystemChannel::FilesystemChannel(  //
        int32_t channelId, std::shared_ptr<Filesystem> filesystem)
    : App::Channel(channelId), mFilesystem(filesystem) {}

void FilesystemApp::FilesystemChannel::select(Filesystem::Path path) {
    mSelectedFile = path;
}

// android.carrierapi.cts.FcpTemplate.parseFcpTemplate (inversion)
static std::vector<uint8_t> makeFcpTemplate(const Filesystem::Path& path) {
    // clang-format off
    return makeTlv(BER_TAG_FCP_TEMPLATE,
        makeTlv(FILE_IDENTIFIER, uint16ToBytes(path.fileId))
    );
    // clang-format on
}

// ETSI TS 102 221 11.1.1
aidl::IccIoResult FilesystemApp::FilesystemChannel::commandSelect(  //
        int32_t p1, int32_t p2, int32_t length, const std::string& data) {
    if (p1 != SELECT_BY_FILE_ID ||
        (p2 != SELECT_RETURN_FCP_TEMPLATE && p2 != SELECT_RETURN_NOTHING)) {
        return toIccIoResult(IO_RESULT_INCORRECT_P1_P2);
    }
    if (length != 2) {  // file ids are 2 byte long
        return toIccIoResult(IO_RESULT_INCORRECT_LENGTH | 2);
    }

    auto fileId = strtol(data.c_str(), nullptr, 16);
    if (fileId <= 0 || fileId > 0xFFFF) {
        LOG(WARNING) << "Incorrect file ID: " << data;
        return toIccIoResult(IO_RESULT_INCORRECT_DATA);
    }

    auto path = mFilesystem->find(fileId);
    if (!path.has_value()) {
        LOG(WARNING) << "FilesystemChannel: file " << std::hex << fileId << " not found";
        return toIccIoResult(IO_RESULT_FILE_NOT_FOUND);
    }
    select(*path);

    if (p2 == SELECT_RETURN_FCP_TEMPLATE) {
        return toIccIoResult(bytesToHexString(makeFcpTemplate(mSelectedFile)));
    }
    return toIccIoResult("");
}

// ETSI TS 102 221 11.1.2
aidl::IccIoResult FilesystemApp::FilesystemChannel::commandStatus(int32_t p1) const {
    if (p1 != 0x00 && p1 != 0x01) {  // 0x02 (termination) not implemented
        return toIccIoResult(IO_RESULT_INCORRECT_P1_P2);
    }
    return toIccIoResult(bytesToHexString(makeFcpTemplate(mSelectedFile)));
}

// ETSI TS 102 221 11.1.3
aidl::IccIoResult FilesystemApp::FilesystemChannel::commandReadBinary(  //
        int32_t offsetHi, int32_t offsetLo) const {
    CHECK(offsetHi == 0 && offsetLo == 0) << "Offset not supported";
    if (auto contents = mFilesystem->read(mSelectedFile); contents.has_value()) {
        return toIccIoResult(*contents);
    }
    LOG(DEBUG) << "Missing ICC file (READ_BINARY): " << mSelectedFile.toString();
    return toIccIoResult(IO_RESULT_FILE_NOT_FOUND);
}

// ETSI TS 102 221 11.1.4
aidl::IccIoResult FilesystemApp::FilesystemChannel::commandUpdateBinary(  //
        int32_t offsetHi, int32_t offsetLo, std::string_view data) {
    CHECK(offsetHi == 0 && offsetLo == 0) << "Offset not supported";
    mFilesystem->write(mSelectedFile, hexStringToBytes(data));
    return toIccIoResult("");
}

// ETSI TS 102 221 11.1.5
aidl::IccIoResult FilesystemApp::FilesystemChannel::commandReadRecord(  //
        int32_t recordId, int32_t mode, int32_t length) {
    CHECK(recordId == 1) << "Records other than no 1 are not supported";
    CHECK(mode == 4) << "Unsupported record mode";  // absolute is the only currently supported mode
    CHECK(length >= 0);
    if (auto contents = mFilesystem->read(mSelectedFile); contents.has_value()) {
        CHECK(static_cast<size_t>(length) == contents->size())
                << "Partial reads not supported (" << length << " != " << contents->size() << ")";
        return toIccIoResult(*contents);
    }
    LOG(DEBUG) << "Missing ICC file (READ_RECORD): " << mSelectedFile.toString();
    return toIccIoResult(IO_RESULT_FILE_NOT_FOUND);
}

// com.android.internal.telephony.uicc.IccFileHandler (inversion)
// ETSI TS 102 221 12.1.1
aidl::IccIoResult FilesystemApp::FilesystemChannel::commandGetResponse() const {
    auto file = mSelectedFile;
    auto contents = mFilesystem->read(file);
    if (!contents.has_value()) {
        LOG(DEBUG) << "Missing ICC file (GET_RESPONSE): " << file.toString();
        return toIccIoResult(IO_RESULT_FILE_NOT_FOUND);
    }
    auto fileSize = contents->size();
    CHECK(fileSize <= 0xFFFF) << "File size won't fit in GET_RESPONSE";

    // 3GPP TS 51.011 9.2.1
    std::vector<uint8_t> response(GET_RESPONSE_EF_SIZE_BYTES, 0);
    response[RESPONSE_DATA_FILE_SIZE_1] = fileSize >> 8;
    response[RESPONSE_DATA_FILE_SIZE_2] = 0xFF & fileSize;
    response[RESPONSE_DATA_FILE_ID_1] = file.fileId >> 8;
    response[RESPONSE_DATA_FILE_ID_2] = 0xFF & file.fileId;
    response[RESPONSE_DATA_FILE_TYPE] = TYPE_EF;
    response[RESPONSE_DATA_LENGTH] = GET_RESPONSE_EF_SIZE_BYTES - RESPONSE_DATA_STRUCTURE;
    if (kLinearFixedFiles.contains(file.fileId)) {
        response[RESPONSE_DATA_STRUCTURE] = EF_TYPE_LINEAR_FIXED;
        response[RESPONSE_DATA_RECORD_LENGTH] = fileSize;  // single record support only
    } else {
        response[RESPONSE_DATA_STRUCTURE] = EF_TYPE_TRANSPARENT;
    }

    return toIccIoResult(response);
}

aidl::IccIoResult FilesystemApp::FilesystemChannel::transmit(const aidl::SimApdu& message) {
    switch (message.instruction) {
        case COMMAND_SELECT:
            return commandSelect(message.p1, message.p2, message.p3, message.data);
        case COMMAND_STATUS:
            return commandStatus(message.p1);
        case COMMAND_READ_BINARY:
            return commandReadBinary(message.p1, message.p2);
        case COMMAND_UPDATE_BINARY:
            return commandUpdateBinary(message.p1, message.p2, message.data);
        case COMMAND_READ_RECORD:
            return commandReadRecord(message.p1, message.p2, message.p3);
        case COMMAND_GET_RESPONSE:
            return commandGetResponse();
        default:
            LOG(ERROR) << "Unsupported filesystem instruction: " << message;
            return toIccIoResult(IO_RESULT_NOT_SUPPORTED);
    }
}

aidl::IccIoResult FilesystemApp::iccIo(const aidl::IccIo& iccIo) {
    CHECK(mBasicChannel) << "Basic channel must always be present";

    if (iccIo.fileId != 0) {
        mBasicChannel->select({iccIo.fileId, iccIo.path});
    }

    aidl::SimApdu message = {
            .instruction = iccIo.command,
            .p1 = iccIo.p1,
            .p2 = iccIo.p2,
            .p3 = iccIo.p3,
            .data = iccIo.data,
    };
    return mBasicChannel->transmit(message);
}

}  // namespace android::hardware::radio::minimal::sim::apps
