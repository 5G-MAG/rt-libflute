// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "flute_types.h"
#include "EncodingSymbol.h"

namespace LibFlute {
  /**
   *  A class for parsing and creating ALC packets
   */
  class AlcPacket {
    public:
     /**
      *  Create an ALC packet from payload data
      *
      *  @param data Received data to be parsed
      *  @param len Length of the buffer
      */
      AlcPacket(char* data, size_t len);

     /**
      *  Create an ALC packet from encoding symbols 
      *
      *  @param tsi Transport Stream Identifier
      *  @param toi Transport Object Identifier
      *  @param fec_oti OTI values
      *  @param symbols Vector of encoding symbols
      *  @param max_size Maximum payload size
      *  @param fdt_instance_id FDT instance ID (only relevant for FDT with TOI=0)
      */
      AlcPacket(uint16_t tsi, uint16_t toi, FecOti fec_oti, const std::vector<EncodingSymbol>& symbols, size_t max_size, uint32_t fdt_instance_id);

     /**
      *  Default destructor.
      */
      ~AlcPacket();

     /**
      *  Get the TSI
      */
      uint64_t tsi() const { return _tsi; };

     /**
      *  Get the TOI
      */
      uint64_t toi() const { return _toi; };

     /**
      *  Get the FEC OTI values
      */
      const FecOti& fec_oti() const { return _fec_oti; };

     /**
      *  Get the LCT header length
      */
      size_t header_length() const  { return _lct_header.lct_header_len * 4; };

     /**
      *  Get the FDT instance ID 
      */
      uint32_t fdt_instance_id() const { return _fdt_instance_id; };

     /**
      *  Get the FEC scheme
      */
      FecScheme fec_scheme() const { return _fec_oti.encoding_id; };

     /**
      *  Get the content encoding
      */
      ContentEncoding content_encoding() const { return _content_encoding; };

     /**
      *  Get a pointer to the payload data of the constructed packet
      */
      char* data() const { return _buffer; };

     /**
      *  Get the payload size
      */
      size_t size() const { return _len; };

    private:
      uint64_t _tsi = 0;
      uint64_t _toi = 0;

      uint32_t _fdt_instance_id = 0;

      uint32_t _source_block_number = 0;
      uint32_t _encoding_symbol_id = 0;

      ContentEncoding _content_encoding = ContentEncoding::NONE;
      FecOti _fec_oti = {};

      char* _buffer = nullptr;
      size_t _len;

      // RFC5651 5.1 - LCT Header Format
      struct __attribute__((packed)) lct_header_t {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint8_t res1:1;
        uint8_t source_packet_indicator:1;
        uint8_t congestion_control_flag:2;
        uint8_t version:4;

        uint8_t close_object_flag:1;
        uint8_t close_session_flag:1;
        uint8_t res:2;
        uint8_t half_word_flag:1;
        uint8_t toi_flag:2;
        uint8_t tsi_flag:1;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        uint8_t version:4;
        uint8_t congestion_control_flag:2;
        uint8_t source_packet_indicator:1;
        uint8_t res1:1;

        uint8_t tsi_flag:1;
        uint8_t toi_flag:2;
        uint8_t half_word_flag:1;
        uint8_t res2:2;
        uint8_t close_session_flag:1;
        uint8_t close_object_flag:1;
#else
#error "Endianness can not be determined"
#endif
        uint8_t lct_header_len;
        uint8_t codepoint;
      } _lct_header;
      static_assert(sizeof(_lct_header) == 4);

      enum HeaderExtension { 
        EXT_NOP  =   0,
        EXT_AUTH =   1,
        EXT_TIME =   2,
        EXT_FTI  =  64,
        EXT_FDT  = 192,
        EXT_CENC = 193
      };

  };
};

