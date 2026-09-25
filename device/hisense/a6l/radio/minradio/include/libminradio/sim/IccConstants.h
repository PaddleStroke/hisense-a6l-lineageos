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
#pragma once

#include <string>

namespace android::hardware::radio::minimal::sim::constants {

// From frameworks/opt/telephony/src/java/com/android/internal/telephony/uicc/IccConstants.java
// 3GPP TS 51.011 Annex D
// ETSI TS 131 102 Annex A
constexpr int EF_ADN = 0x6F3A;
constexpr int EF_FDN = 0x6F3B;
constexpr int EF_GID1 = 0x6F3E;
constexpr int EF_GID2 = 0x6F3F;
constexpr int EF_SDN = 0x6F49;
constexpr int EF_EXT1 = 0x6F4A;
constexpr int EF_EXT2 = 0x6F4B;
constexpr int EF_EXT3 = 0x6F4C;
constexpr int EF_EXT5 = 0x6F4E;
constexpr int EF_EXT6 = 0x6FC8;
constexpr int EF_MWIS = 0x6FCA;
constexpr int EF_MBDN = 0x6FC7;
constexpr int EF_PNN = 0x6FC5;
constexpr int EF_OPL = 0x6FC6;
constexpr int EF_SPN = 0x6F46;
constexpr int EF_SMS = 0x6F3C;
constexpr int EF_ICCID = 0x2FE2;
constexpr int EF_AD = 0x6FAD;
constexpr int EF_MBI = 0x6FC9;
constexpr int EF_MSISDN = 0x6F40;
constexpr int EF_SPDI = 0x6FCD;
constexpr int EF_SST = 0x6F38;
constexpr int EF_CFIS = 0x6FCB;
constexpr int EF_IMG = 0x4F20;
constexpr int EF_PSISMSC = 0x6FE5;
constexpr int EF_SMSS = 0x6F43;
constexpr int EF_PBR = 0x4F30;
constexpr int EF_LI = 0x6F05;
constexpr int EF_MAILBOX_CPHS = 0x6F17;
constexpr int EF_VOICE_MAIL_INDICATOR_CPHS = 0x6F11;
constexpr int EF_CFF_CPHS = 0x6F13;
constexpr int EF_SPN_CPHS = 0x6F14;
constexpr int EF_SPN_SHORT_CPHS = 0x6F18;
constexpr int EF_INFO_CPHS = 0x6F16;
constexpr int EF_CSP_CPHS = 0x6F15;
constexpr int EF_CST = 0x6F32;
constexpr int EF_RUIM_SPN = 0x6F41;
constexpr int EF_PL = 0x2F05;
constexpr int EF_ARR = 0x2F06;
constexpr int EF_CSIM_LI = 0x6F3A;
constexpr int EF_CSIM_SPN = 0x6F41;
constexpr int EF_CSIM_MDN = 0x6F44;
constexpr int EF_CSIM_IMSIM = 0x6F22;
constexpr int EF_CSIM_CDMAHOME = 0x6F28;
constexpr int EF_CSIM_EPRL = 0x6F5A;
constexpr int EF_CSIM_PRL = 0x6F30;
constexpr int EF_CSIM_MLPL = 0x4F20;
constexpr int EF_CSIM_MSPL = 0x4F21;
constexpr int EF_CSIM_MIPUPP = 0x6F4D;
constexpr int EF_IMPU = 0x6F04;
constexpr int EF_IMPI = 0x6F02;
constexpr int EF_DOMAIN = 0x6F03;
constexpr int EF_IST = 0x6F07;
constexpr int EF_PCSCF = 0x6F09;
constexpr int EF_PLMN_W_ACT = 0x6F60;
constexpr int EF_OPLMN_W_ACT = 0x6F61;
constexpr int EF_HPLMN_W_ACT = 0x6F62;
constexpr int EF_EHPLMN = 0x6FD9;
constexpr int EF_FPLMN = 0x6F7B;
constexpr int EF_LRPLMNSI = 0x6FDC;
constexpr int EF_HPPLMN = 0x6F31;
// 3GPP TS 51.011 10.7
constexpr int MF_SIM_VAL = 0x3F00;
constexpr std::string MF_SIM = "3F00";
constexpr std::string DF_TELECOM = "7F10";
constexpr std::string DF_PHONEBOOK = "5F3A";
constexpr std::string DF_GRAPHICS = "5F50";
constexpr std::string DF_GSM = "7F20";
constexpr std::string DF_CDMA = "7F25";
constexpr std::string DF_MMSS = "5F3C";
constexpr std::string DF_ADF = "7FFF";

// From frameworks/base/telephony/java/com/android/internal/telephony/uicc/IccUtils.java
constexpr int FPLMN_BYTE_SIZE = 3;

// From frameworks/opt/telephony/src/java/com/android/internal/telephony/uicc/IccFileHandler.java
// 3GPP TS 11.11 9.2
constexpr int COMMAND_READ_BINARY = 0xB0;     // 176
constexpr int COMMAND_UPDATE_BINARY = 0xD6;   // 214
constexpr int COMMAND_READ_RECORD = 0xB2;     // 178
constexpr int COMMAND_UPDATE_RECORD = 0xDC;   // 220
constexpr int COMMAND_SEEK = 0xA2;            // 162 (also: SEARCH RECORD)
constexpr int COMMAND_SELECT = 0xA4;          // 164
constexpr int COMMAND_GET_RESPONSE = 0xC0;    // 192
constexpr int COMMAND_STATUS = 0xF2;          // 242
constexpr int COMMAND_GET_DATA = 0xCA;        // 202 (ISO 7816 7.4.2)
constexpr int COMMAND_MANAGE_CHANNEL = 0x70;  // 112
constexpr int EF_TYPE_TRANSPARENT = 0;
constexpr int EF_TYPE_LINEAR_FIXED = 1;
constexpr int EF_TYPE_CYCLIC = 3;
constexpr int TYPE_RFU = 0;
constexpr int TYPE_MF = 1;
constexpr int TYPE_DF = 2;
constexpr int TYPE_EF = 4;
constexpr int GET_RESPONSE_EF_SIZE_BYTES = 15;
constexpr int RESPONSE_DATA_RFU_1 = 0;
constexpr int RESPONSE_DATA_RFU_2 = 1;
constexpr int RESPONSE_DATA_FILE_SIZE_1 = 2;
constexpr int RESPONSE_DATA_FILE_SIZE_2 = 3;
constexpr int RESPONSE_DATA_FILE_ID_1 = 4;
constexpr int RESPONSE_DATA_FILE_ID_2 = 5;
constexpr int RESPONSE_DATA_FILE_TYPE = 6;
constexpr int RESPONSE_DATA_RFU_3 = 7;
constexpr int RESPONSE_DATA_ACCESS_CONDITION_1 = 8;
constexpr int RESPONSE_DATA_ACCESS_CONDITION_2 = 9;
constexpr int RESPONSE_DATA_ACCESS_CONDITION_3 = 10;
constexpr int RESPONSE_DATA_FILE_STATUS = 11;
constexpr int RESPONSE_DATA_LENGTH = 12;
constexpr int RESPONSE_DATA_STRUCTURE = 13;
constexpr int RESPONSE_DATA_RECORD_LENGTH = 14;

// From frameworks/opt/telephony/src/java/com/android/internal/telephony/uicc/IccIoResult.java
// ISO 7816 5.1.3
constexpr uint16_t IO_RESULT_SUCCESS = 0x9000;
constexpr uint16_t IO_RESULT_NOT_SUPPORTED = 0x6A81;
constexpr uint16_t IO_RESULT_FILE_NOT_FOUND = 0x6A82;  // file or application
constexpr uint16_t IO_RESULT_INCORRECT_DATA = 0x6A80;
constexpr uint16_t IO_RESULT_INCORRECT_P1_P2 = 0x6A86;
constexpr uint16_t IO_RESULT_INCORRECT_LENGTH = 0x6C00;  // low byte is suggested length
constexpr uint16_t IO_RESULT_CLASS_NOT_SUPPORTED = 0x6E00;
constexpr uint16_t IO_RESULT_CHANNEL_NOT_SUPPORTED = 0x6881;
constexpr uint16_t IO_RESULT_TECHNICAL_PROBLEM = 0x6F00;

}  // namespace android::hardware::radio::minimal::sim::constants
