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
  /**
   *  Represents a file being transmitted or received
   */
  class File {
    public:
     /**
      *  Create a file from an FDT entry (used for reception)
      *
      *  @param entry FDT entry
      */
      File(LibFlute::FileDeliveryTable::FileEntry entry);

     /**
      *  Create a file from the given parameters (used for transmission)
      *
      *  @param toi TOI of the file
      *  @param content_location Content location URI to use
      *  @param content_type MIME type
      *  @param expires Expiry value (in seconds since the NTP epoch)
      *  @param data Pointer to the data buffer
      *  @param length Length of the buffer
      *  @param copy_data Copy the buffer. If false (the default), the caller must ensure the buffer remains valid 
      *                   while the file is being transmitted.
      */
      File(uint32_t toi, 
          FecOti fec_oti,
          std::string content_location,
          std::string content_type,
          uint64_t expires,
          char* data,
          size_t length,
          bool copy_data = false);

     /**
      *  Default destructor.
      */
      virtual ~File();

     /**
      *  Write the data from an encoding symbol into the appropriate place in the buffer
      */
      void put_symbol(const EncodingSymbol& symbol);

     /**
      *  Check if the file is complete
      */
      bool complete() const { return _complete; };

     /**
      *  Get the data buffer
      */
      char* buffer() const { return _buffer; };

     /**
      *  Get the data buffer length
      */
      size_t length() const { return _meta.fec_oti.transfer_length; };

     /**
      *  Get the FEC OTI values
      */
      const FecOti& fec_oti() const { return _meta.fec_oti; };

     /**
      *  Get the file metadata from its FDT entry
      */
      const LibFlute::FileDeliveryTable::FileEntry& meta() const { return _meta; };

     /**
      *  Timestamp of file reception
      */
      unsigned long received_at() const { return _received_at; };

     /**
      *  Log access to the file by incrementing a counter
      */
      void log_access() { _access_count++; };

     /**
      *  Get the access counter value
      */
      unsigned access_count() const { return _access_count; };

     /**
      *  Get the next encoding symbols that fit in max_size bytes
      */
      std::vector<EncodingSymbol> get_next_symbols(size_t max_size);

     /**
      *  Mark encoding symbols as completed
      */
      void mark_completed(const std::vector<EncodingSymbol>& symbols, bool success);

     /**
      *  Set the FDT instance ID
      */
      void set_fdt_instance_id( uint16_t id) { _fdt_instance_id = id; };

     /**
      *  Get the FDT instance ID
      */
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
