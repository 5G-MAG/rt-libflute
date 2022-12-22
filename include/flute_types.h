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
   *  Error correction schemes. From the registry for FEC schemes http://www.iana.org/assignments/rmt-fec-parameters (RFC 5052)
   */
  enum class FecScheme {
    CompactNoCode,
    Raptor
  };

  /**
   *  abstract class for FEC Object En/De-coding
   */
  class FecOti {
    public:

    FecScheme encoding_id;
    uint64_t transfer_length;
    uint32_t encoding_symbol_length;
    uint32_t max_source_block_length;

    /**
     * @brief Attempt to decode a source block
     *
     * @param srcblk the source block that should be decoded
     * @return whether or not the decoding was successful
     */
    virtual bool check_source_block_completion(SourceBlock& srcblk);

    /**
     * @brief Encode a source block into multiple symbols
     *
     * @param buffer a pointer to the buffer containing the data
     * @param bytes_read a pointer to an integer to store the number of bytes read out of buffer
     * @return a map of source blocks that the object has been encoded to
     */
    virtual std::map<uint16_t, SourceBlock> create_blocks(char *buffer, int *bytes_read);


    virtual void calculate_partioning();

    private:


  };

  class CompactNoCode : FecOti {



  }

  class RaptorFEC : FecOti {

    unsigned int F; // object size in bytes
    unsigned int Al; // symbol alignment: 4
    unsigned int T; // symbol size in bytes
    unsigned int Z; // number of source blocks
    unsigned int N; // number of sub-blocks per source block
    unsigned int K; // number of symbols in a source block
    unsigned int P; // maximum payload size: 1420 for ipv4 over 802.3

  }

};
