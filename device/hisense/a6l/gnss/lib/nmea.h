// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): NMEA 0183 helpers. The modem normally sends its own NMEA (QMI LOC NMEA
// indication); these synthesize GGA/RMC from a QMI position report when it does not (fallback, marked by talker "GP").
#pragma once

#include <string>

#include "loc_v02.h"

namespace a6l {
namespace nmea {

uint8_t checksum(const std::string& body);             // XOR of the characters between '$' and '*'
std::string finish(const std::string& body);            // "$" + body + "*HH"
bool valid(const std::string& sentence);                // "$...*HH" with a correct checksum
std::string gga(const loc::Fix& f);                     // empty if the fix has no lat/lon/utc
std::string rmc(const loc::Fix& f);

}  // namespace nmea
}  // namespace a6l
