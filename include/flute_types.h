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
   *  values 2-5 (Reed-Solomon GF(2^^m), LDPC Staircase, LDPC Triangle,
   *  Reed-Solomon GF(2^^8)) are registered but not implemented by this library.
   */
  enum class FecScheme {
    CompactNoCode = 0,
    Raptor = 1,
    RaptorQ = 6
  };

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

    /**
     *  Raptor/RaptorQ scheme-specific OTI (RFC 5053 §3.2.3, RFC 6330 §4.2).
     *  Unused (left at their defaults) for FecScheme::CompactNoCode.
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
