/*
libflute - FLUTE/ALC library

Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)

Licensed under the License terms and conditions for use, reproduction, and
distribution of 5G-MAG software (the “License”).  You may not use this file
except in compliance with the License.  You may obtain a copy of the License at
https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
agreed to in writing, software distributed under the License is distributed on
an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
or implied.

See the License for the specific language governing permissions and limitations
under the License.
*/
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
