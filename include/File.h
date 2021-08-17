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
#include <map>
#include <memory>
#include "AlcPacket.h"
#include "FileDeliveryTable.h"
#include "EncodingSymbol.h"

namespace LibFlute {
  class File {
    public:
      File(LibFlute::FileDeliveryTable::FileEntry entry);
      File(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data = false);
      virtual ~File();

      void put_symbol(const EncodingSymbol& symbol);

      bool complete() const { return _complete; };

      char* buffer() const { return _buffer; };
      size_t length() const { return _meta.fec_oti.transfer_length; };

      const FecOti& fec_oti() const { return _meta.fec_oti; };
      const LibFlute::FileDeliveryTable::FileEntry& meta() const { return _meta; };
      unsigned long received_at() const { return _received_at; };

      void log_access() { _access_count++; };
      unsigned access_count() const { return _access_count; };

      std::vector<EncodingSymbol> get_next_symbols(size_t max_size);
      void mark_completed(const std::vector<EncodingSymbol>& symbols, bool success);

      void set_fdt_instance_id( uint16_t id) { _fdt_instance_id = id; };
      uint16_t fdt_instance_id() { return _fdt_instance_id; };

    private:
      void calculate_partitioning();
      void create_blocks();

      struct SourceBlock {
        bool complete = false;
        struct Symbol {
          char* data;
          size_t length;
          bool complete = false;
          bool queued = false;
        };
        std::map<uint16_t, Symbol> symbols; 
      };

      void check_source_block_completion(SourceBlock& block);
      void check_file_completion();

      std::map<uint16_t, SourceBlock> _source_blocks; 

      bool _complete = false;;

      uint32_t _nof_source_symbols = 0;
      uint32_t _nof_source_blocks = 0;
      uint32_t _nof_large_source_blocks = 0;
      uint32_t _large_source_block_length = 0;
      uint32_t _small_source_block_length = 0;

      char* _buffer = nullptr;
      bool _own_buffer = false;

      LibFlute::FileDeliveryTable::FileEntry _meta;
      unsigned long _received_at;
      unsigned _access_count = 0;

      uint16_t _fdt_instance_id = 0;
  };
};
