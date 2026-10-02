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
#include <optional>

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
   *  Map an FEC Encoding ID as it appears on the wire onto a scheme this library implements.
   *
   *  Returns no value for an identifier naming no scheme here. The FDT carries this as an
   *  arbitrary integer, so casting it straight to FecScheme would manufacture an enumerator that
   *  no branch handles and defer the failure to whichever switch reaches it first. Extend this
   *  alongside the enumeration, never separately. `code-derived, no spec claim`.
   */
  constexpr auto fec_scheme_from_encoding_id(unsigned long id) -> std::optional<FecScheme>
  {
    switch (id) {
      case 0: return FecScheme::CompactNoCode;
      default: return std::nullopt;
    }
  }

  /**
   *  Which set of obligations this session is held to.
   *
   *  The three are not interchangeable, and two of them mandate a different FDT schema with a
   *  different mandatory schemaVersion value, so the schema is derived from this rather than chosen
   *  separately: a session cannot be conformant while its profile and its FDT schema disagree.
   *
   *  Spelled Profile::None, Profile::MBMS::Download and Profile::MBS, as agreed on
   *  5G-MAG/rt-libflute#99. An enum class cannot place a value under a nested scope, so this is a
   *  small value type with named constants; it compares and copies like an enumeration.
   */
  class Profile {
    public:
      struct MBMS;

      /**
       *  No 3GPP profile: the session is bound only by the FLUTE specification in force and the
       *  ALC and LCT documents beneath it, RFC 3926 for version 1 and RFC 6726 for version 2. The
       *  default, so that a caller who selects no profile keeps plain FLUTE behaviour and none of
       *  the 3GPP restrictions, several of which refuse a session outright.
       */
      static const Profile None;

      /**
       *  TS 26.517 clause 6.2, layered on TS 26.346 clause 7.2 and annex L.4.
       *
       *  TS 26.517 V18.6.0 clause 6.2.1: "If FLUTE [12] is used to realise the Object Distribution
       *  Method, the MBS Distribution Session shall conform to the MBMS Download Profile as defined
       *  in clause L.4 of TS 26.346 [7] with the additional requirements in clause 6.2 of the
       *  present document." The same clause fixes the schema: "The MBSTF shall use the Profiled FDT
       *  Schema according to clause L.6 of TS 26.346 [7] to describe the object list currently
       *  being transmitted in the MBS Distribution Session."
       */
      static const Profile MBS;

      /** Transitional names, kept so existing callers build while they move to the names above. */
      [[deprecated("use Profile::MBS")]] static const Profile Ts26517;
      [[deprecated("use Profile::MBMS::Download")]] static const Profile Ts26346;
      [[deprecated("use Profile::None")]] static const Profile Unprofiled;

      constexpr bool operator==(const Profile &other) const { return _value == other._value; }
      constexpr bool operator!=(const Profile &other) const { return _value != other._value; }

    private:
      constexpr explicit Profile(int value) : _value(value) {}
      int _value;
  };

  struct Profile::MBMS {
    /**
     *  The MBMS Download Profile, TS 26.346 clause 7.2 and annex L.4, without TS 26.517's
     *  additions. Annex L.4 is the only one of the three that the specifications name.
     *
     *  TS 26.346 V18.2.0 clause 7.2.9 fixes its schema: "The extended FLUTE FDT instance schema
     *  defined in clause 7.2.10.1 (based on the one in RFC 3926 [9]) shall be used."
     */
    static const Profile Download;
  };

  inline constexpr Profile Profile::None{0};
  inline constexpr Profile Profile::MBMS::Download{1};
  inline constexpr Profile Profile::MBS{2};
  inline constexpr Profile Profile::Ts26517{2};
  inline constexpr Profile Profile::Ts26346{1};
  inline constexpr Profile Profile::Unprofiled{0};

  /** True for the profiles bound by the 3GPP obligations, i.e. anything but an unprofiled session. */
  constexpr bool is_3gpp(Profile p) { return p != Profile::None; }

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
