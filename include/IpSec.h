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
#include <cstdint>
#include <string>

namespace LibFlute::IpSec {
  enum class Direction { In, Out };
  /**
   *  Configure an IPsec ESP security association and policy for FLUTE traffic to or from
   *  @p dest_address. The association carries both an encryption and an authentication
   *  algorithm; see the citation at the call site in `IpSec.cpp` for why.
   *
   *  @param spi Security Parameter Index value to use
   *  @param dest_address Destination address to apply the SA and policy to
   *  @param direction In or Out
   *  @param key AES encryption key, as a hex string, without a leading 0x, of even length
   *  @param auth_key HMAC-SHA256 authentication key, in the same form. If empty, one is derived
   *                  from @p key, so a caller supplying a single key still gets authentication.
   */
  /**
   *  Install an ESP security association and the policy that selects the session's traffic into
   *  it.
   *
   *  @param dest_port The session's UDP port. The policy selector names the protocol and this
   *         port as well as the destination address, so it captures this session's packets and
   *         not everything else addressed to the same group.
   *
   *         RFC 5775 clause 5.1.1: "The sender IPsec SPD entry MUST be configured to process
   *         outbound packets to the destination address and UDP port number of the applicable ALC
   *         session."
   */
  void enable_esp(uint32_t spi, const std::string& dest_address, unsigned short dest_port,
                   Direction direction, const std::string& key,
                   const std::string& auth_key = "");
};
