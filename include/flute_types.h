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

/** \mainpage LibFlute - ALC/FLUTE library
 *
 * The library contains two simple **example applications** as a starting point:
 * - examples/flute-transmitter.cpp for sending files
 * - examples/flute-receiver.cpp for receiving files
 *
 * The relevant public headers for using this library are
 * - LibFlute::Transmitter (in include/Transmitter.h), and
 * - LibFlute::Receiver (in include/Receiver.h)
 *
 */

namespace LibFlute {
  /**
   *  Content Encodings
   */
  enum class ContentEncoding {
    NONE,
    ZLIB,
    DEFLATE,
    GZIP
  };

  /**
   *  Error correction schemes 
   */
  enum class FecScheme {
    CompactNoCode
  };

  /**
   *  Which set of obligations this session is held to, named by the document that imposes them.
   *
   *  The three are not interchangeable, and two of them mandate a different FDT schema with a
   *  different mandatory schemaVersion value, so the schema is derived from this rather than chosen
   *  separately: a session cannot be conformant while its profile and its FDT schema disagree.
   *
   *  Named after the governing documents rather than after the profiles, because only one of the
   *  three profiles is named in the specifications at all. Annex L.4 of TS 26.346 is titled "MBMS
   *  Download Profile". Its TS 26.517 variant has no name, being described only as that profile
   *  plus the additional requirements of clause 6.2 (quoted at Ts26517 below). Plain FLUTE outside
   *  any 3GPP profile has no name because it is simply the absence of one.
   */
  enum class Profile {
    /**
     *  TS 26.517 clause 6.2, layered on TS 26.346 clause 7.2 and annex L.4. The default.
     *
     *  TS 26.517 V18.6.0 clause 6.2.1: "If FLUTE [12] is used to realise the Object Distribution
     *  Method, the MBS Distribution Session shall conform to the MBMS Download Profile as defined
     *  in clause L.4 of TS 26.346 [7] with the additional requirements in clause 6.2 of the present
     *  document." The same clause fixes the schema: "The MBSTF shall use the Profiled FDT Schema
     *  according to clause L.6 of TS 26.346 [7] to describe the object list currently being
     *  transmitted in the MBS Distribution Session."
     */
    Ts26517,

    /**
     *  TS 26.346 clause 7.2 and annex L.4, without TS 26.517's additions.
     *
     *  TS 26.346 V18.2.0 clause 7.2.9 fixes its schema instead: "The extended FLUTE FDT instance
     *  schema defined in clause 7.2.10.1 (based on the one in RFC 3926 [9]) shall be used."
     */
    Ts26346,

    /** RFC 3926 and the ALC and LCT documents it sits on, with no 3GPP restriction applied. */
    Rfc3926
  };

  /** True for the profiles bound by the 3GPP obligations, i.e. anything but plain RFC 3926. */
  constexpr bool is_3gpp(Profile p) { return p != Profile::Rfc3926; }

  /**
   *  OTI values struct
   */
  struct FecOti {
    FecScheme encoding_id;
    uint32_t instance_id;
    uint64_t transfer_length;
    uint32_t encoding_symbol_length;
    uint32_t max_source_block_length;
    uint32_t max_number_of_encoding_symbols;

    bool operator==(const FecOti &other) const {
      return encoding_id == other.encoding_id && transfer_length == other.transfer_length &&
             encoding_symbol_length == other.encoding_symbol_length && max_source_block_length == other.max_source_block_length &&
             max_number_of_encoding_symbols == other.max_number_of_encoding_symbols;
    };
    bool operator!=(const FecOti &other) const { return !(*this == other); };
  };
};
