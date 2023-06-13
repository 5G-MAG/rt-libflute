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
#include <stddef.h>
#include <stdint.h>
#include <map>
#include "tinyxml2.h"
#ifdef RAPTOR_ENABLED
#include "raptor.h"
#endif

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
    Raptor,
    Reed_Solomon_GF_2_m,
    LDPC_Staircase_Codes,
    LDPC_Triangle_Codes,
    Reed_Solomon_GF_2_8,
    RaptorQ
  };

  struct Symbol {
    char* data;
    size_t length;
    bool complete = false;
    bool queued = false;
  };

  struct SourceBlock {
    uint16_t id = 0;
    bool complete = false;
    std::map<uint16_t, Symbol> symbols; 
  };

  struct FecOti {
    FecScheme encoding_id;
    uint64_t transfer_length;
    uint32_t encoding_symbol_length;
    uint32_t max_source_block_length;
  };

  /**
   *  abstract class for FEC Object En/De-coding
   */
  class FecTransformer {

    public:

    virtual ~FecTransformer() = default;


    /**
     * @brief Attempt to decode a source block
     *
     * @param srcblk the source block that should be decoded
     * @return whether or not the decoding was successful
     */
    virtual bool check_source_block_completion(SourceBlock& srcblk) = 0;

    /**
     * @brief Encode a file into multiple source blocks
     *
     * @param buffer a pointer to the buffer containing the data
     * @param bytes_read a pointer to an integer to store the number of bytes read out of buffer
     * @return a map of source blocks that the object has been encoded to
     */
    virtual std::map<uint16_t, SourceBlock> create_blocks(char *buffer, int *bytes_read) = 0;

    /**
     * @brief Process a received symbol
     *
     * @param srcblk the source block this symbols corresponds to
     * @param symb the received symbol
     * @param id the symbols id
     * @return success or failure
     */
    virtual bool process_symbol(LibFlute::SourceBlock& srcblk, LibFlute::Symbol& symb, unsigned int id) = 0;

    virtual bool calculate_partitioning() = 0;

    /**
     * @brief Attempt to parse relevent information for decoding from the FDT
     *
     * @return success status
     */
    virtual bool parse_fdt_info(tinyxml2::XMLElement *file) = 0;

    /**
     * @brief Add relevant information about the FEC Scheme which the decoder may need, to the FDT
     * 
     * @return success status
     */
    virtual bool add_fdt_info(tinyxml2::XMLElement *file) = 0;

    /**
     * @brief Allocate the size of the buffer needed for this encoding scheme (since it may be larger)
     * 
     * @param min_length this should be the size of the file (transfer length). This determines the minimum size of the returned buffer
     * @return 0 on failure, otherwise return a pointer to the buffer
     */
    virtual void *allocate_file_buffer(int min_length) = 0;

      /**
       * @brief Called after the file is marked as complete, to finish extraction/decoding (if necessary)
       *
       * @param blocks the source blocks of the file, stored in the File object
       */
    virtual bool extract_file(std::map<uint16_t, SourceBlock> blocks) = 0;

    uint32_t nof_source_symbols = 0;
    uint32_t nof_source_blocks = 0;
    uint32_t large_source_block_length = 0;
    uint32_t small_source_block_length = 0;
    uint32_t nof_large_source_blocks = 0;
    
  };

#ifdef RAPTOR_ENABLED
  class RaptorFEC : public FecTransformer {

    private:

    bool is_encoder = true;

    unsigned int target_K(int blockno);

    Symbol translate_symbol(struct enc_context *encoder_ctx);

    LibFlute::SourceBlock create_block(char *buffer, int *bytes_read, int blockid);

    const float surplus_packet_ratio = 1.15; // adds 15% transmission overhead in exchange for protection against up to 15% packet loss. Assuming 1 symbol per packet, for smaller files packets may contain up to 10 symbols per packet but small files are much less vulnerable to packet loss anyways
    
    void extract_finished_block(LibFlute::SourceBlock& srcblk, struct dec_context *dc);

    public: 

    RaptorFEC(unsigned int transfer_length, unsigned int max_payload);

    RaptorFEC() {};

    ~RaptorFEC();

    bool check_source_block_completion(SourceBlock& srcblk);

    std::map<uint16_t, SourceBlock> create_blocks(char *buffer, int *bytes_read);

    bool process_symbol(LibFlute::SourceBlock& srcblk, LibFlute::Symbol& symb, unsigned int id);

    bool calculate_partitioning();

    bool parse_fdt_info(tinyxml2::XMLElement *file);

    bool add_fdt_info(tinyxml2::XMLElement *file);

    void *allocate_file_buffer(int min_length);

    bool extract_file(std::map<uint16_t, SourceBlock> blocks);

    std::map<uint16_t, struct dec_context* > decoders; // map of source block number to decoders

    uint32_t nof_source_symbols = 0;
    uint32_t nof_source_blocks = 0;
    uint32_t large_source_block_length = 0;
    uint32_t small_source_block_length = 0;
    uint32_t nof_large_source_blocks = 0;

    unsigned int F; // object size in bytes
    unsigned int Al = 4; // symbol alignment: 4
    unsigned int T; // symbol size in bytes
    unsigned long W = 16*1024*1024; // target on sub block size- set default to 16 MB, to keep the number of sub-blocks, N, = 1 (you probably only need W >= 11MB to achieve this, assuming an ethernet mtu of ~1500 bytes, but we round to the nearest power of 2)
    unsigned int G; // number of symbols per packet
    unsigned int Z; // number of source blocks
    unsigned int N; // number of sub-blocks per source block
    unsigned int K; // number of symbols in a source block
    unsigned int Kt; // total number of symbols
    unsigned int P; // maximum payload size: e.g. 1436 for ipv4 over 802.3

  };
#endif

};
