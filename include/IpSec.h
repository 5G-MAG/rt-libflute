// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
// 
// See the License for the specific language governing permissions and limitations
// under the License.
//
#pragma once
#include <string>

namespace LibFlute::IpSec {
  enum class Direction { In, Out };

  /**
   *  Configure an IPSec ESP SA + policy for FLUTE traffic to/from @p dest_address.
   *
   *  RFC 6726 SS7.5 requires that authentication SHALL be present alongside
   *  encryption -- an HMAC-SHA256 (XFRMA_ALG_AUTH) authentication algorithm is
   *  always configured in addition to the AES encryption (XFRMA_ALG_CRYPT).
   *
   *  @param spi Security Parameter Index value to use
   *  @param dest_address Destination address to apply the SA/policy to
   *  @param direction In or Out
   *  @param key AES encryption key, as a hex string (without leading 0x, even length)
   *  @param auth_key Authentication (HMAC-SHA256) key, as a hex string (without leading
   *                  0x, even length). If empty, a key is derived deterministically from
   *                  @p key so that authentication is still configured for existing
   *                  callers that only supply one key.
   */
  void enable_esp(uint32_t spi, const std::string& dest_address, Direction direction, const std::string& key,
                   const std::string& auth_key = "");
};
