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
   *  Error correction schemes. Numeric values are fixed by IANA's FEC
   *  Encoding ID registry (http://www.iana.org/assignments/rmt-fec-parameters,
   *  RFC 5052) and are written to the wire (FEC-OTI-FEC-Encoding-ID) as-is, so
   *  they are set explicitly here rather than left as sequential ordinals --
   *  values 2-6 (Reed-Solomon GF(2^^m), LDPC Staircase, LDPC Triangle,
   *  Reed-Solomon GF(2^^8), RaptorQ) are registered but not implemented by
   *  this library. RaptorQ in particular is deliberately left out: 3GPP
   *  TS 26.346 cl.7.2.2/7.2.12 mandates Raptor (RFC 5053, this library's
   *  primary consumer base) but does not define or reference RaptorQ for
   *  this delivery method -- see the future/raptorq-support branch.
   */
  enum class FecScheme {
    CompactNoCode = 0,
    Raptor = 1
  };

  /**
   *  Which set of obligations this session is held to.
   *
   *  The two are not interchangeable and the difference is not cosmetic. A general FLUTE
   *  session is bound only by RFC 3926 and the ALC/LCT documents it sits on. A 3GPP MBMS
   *  session is bound by those *and* by the MBMS Download Profile, which forbids the sender
   *  several things RFC 3926 permits, so a session that is correct as general FLUTE can be
   *  non-conformant as MBMS.
   *
   *  What selects the profile for 5G MBS:
   *  TS 26.517 V18.6.0 clause 6.2.1: "If FLUTE [12] is used to realise the Object Distribution
   *  Method, the MBS Distribution Session shall conform to the MBMS Download Profile as defined
   *  in clause L.4 of TS 26.346 [7] with the additional requirements in clause 6.2 of the
   *  present document."
   *
   *  The default is Mbms3gpp, because that is what the specifications mandating FLUTE in this
   *  project require. A caller wanting plain RFC 3926 behaviour has to ask for it. Note this is
   *  deliberately the stricter default: it withholds attributes a non-3GPP peer may expect.
   */
  enum class Profile {
    /** RFC 3926 plus TS 26.346 annex L.4 sender restrictions. The default. */
    Mbms3gpp,
    /** RFC 3926 only, with no 3GPP restriction applied. */
    GeneralFlute
  };

  /**
   *  OTI values struct
   */
  /**
   *  Default FEC redundancy level, as a percentage of a source block's K symbols, used for a
   *  Raptor-family session when the FEC OTI names no encoding-symbol maximum. The operator
   *  overrides it per session (Transmitter's fec_redundancy_level argument) or fixes the budget
   *  exactly by setting FecOti::max_number_of_encoding_symbols.
   *
   *  The unit and its meaning are the specification's, not this library's.
   *  TS 26.346 V18.2.0 clause 7.3.2.11: "For example, a FEC redundancy level of 40% means that
   *  for an FEC-encoded block of K symbols, 1.4*K symbols are broadcast over the air."
   *
   *  This value is a documented default and nothing more: no clause sets it, and it is
   *  deliberately not derived from any measurement taken on one network. 10 preserves the
   *  behaviour this library had before the level became settable.
   *
   *  It is not signalled. The download profile forbids carrying it in the FDT
   *  (TS 26.346 V18.2.0 clause L.4.4 lists mbms2012:FEC-Redundancy-Level among the attributes
   *  that "shall not be carried in the FDT sent by the FLUTE sender"), and the session-level
   *  declaration of clause 7.3.2.11 lives in SDP, which this library does not generate.
   *
   *  A level must exceed the loss it is meant to survive by the code's own decoding
   *  inefficiency, which is not a fixed margin: near the point where a block becomes decodable
   *  a further symbol is roughly as likely to be linearly dependent as not. A level equal to the
   *  expected loss rate is therefore not enough.
   */
  constexpr uint32_t kDefaultFecRedundancyLevel = 10;

  struct FecOti {
    FecScheme encoding_id;
    uint32_t instance_id;
    uint64_t transfer_length;
    uint32_t encoding_symbol_length;
    uint32_t max_source_block_length;
    uint32_t max_number_of_encoding_symbols;

    /**
     *  Raptor scheme-specific OTI (RFC 5053 §3.2.3). Unused (left at their
     *  defaults) for FecScheme::CompactNoCode.
     *
     *  nof_sub_blocks is always 1 in this implementation: RFC 5052 permits
     *  N == 1 (no further sub-block byte-interleaving within a symbol), and
     *  skipping it keeps the encoder/decoder considerably simpler. It only
     *  gives up an orthogonal robustness feature (resilience to *partial*,
     *  sub-symbol burst loss within one link-layer payload) -- it does not
     *  affect interoperability: a receiver just sees N=1 in the OTI and
     *  decodes accordingly.
     */
    uint32_t nof_source_blocks = 0;   // Z
    uint32_t nof_sub_blocks = 1;      // N
    uint32_t symbol_alignment = 1;    // Al

    bool operator==(const FecOti &other) const {
      return encoding_id == other.encoding_id && transfer_length == other.transfer_length &&
             encoding_symbol_length == other.encoding_symbol_length && max_source_block_length == other.max_source_block_length &&
             max_number_of_encoding_symbols == other.max_number_of_encoding_symbols &&
             nof_source_blocks == other.nof_source_blocks && nof_sub_blocks == other.nof_sub_blocks &&
             symbol_alignment == other.symbol_alignment;
    };
    bool operator!=(const FecOti &other) const { return !(*this == other); };
  };
};
