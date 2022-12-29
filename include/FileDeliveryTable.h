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
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "flute_types.h"

namespace LibFlute {
  /**
   *  A class for parsing and creating FLUTE FDTs (File Delivery Tables).
   */
  class FileDeliveryTable {
    public:
     /**
      *  Create an empty FDT
      *
      *  @param instance_id FDT instance ID to set
      *  @param fec_oti Global FEC OTI parameters
      */
      FileDeliveryTable(uint32_t instance_id, FecOti fec_oti);

     /**
      *  Parse an XML string and create a FDT class from it
      *
      *  @param instance_id FDT instance ID (from ALC headers)
      *  @param buffer String containing the FDT XML
      *  @param len Length of the buffer
      */
      FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len);

     /**
      *  Default destructor.
      */
      virtual ~FileDeliveryTable();

     /**
      *  Get the FDT instance ID
      */
      uint32_t instance_id() { return _instance_id; };

     /**
      *  An entry for a file in the FDT
      */
      struct FileEntry {
        uint32_t toi;
        std::string content_location;
        uint32_t content_length;
        std::string content_md5;
        std::string content_type;
        uint64_t expires;
        FecOti fec_oti;
        FecTransformer *fec_transformer;
      };

     /**
      *  Set the expiry value
      */
      void set_expires(uint64_t exp) { _expires = exp; };

     /**
      *  Add a file entry
      */
      void add(FileEntry& entry);

     /**
      *  Remove a file entry
      */
      void remove(uint32_t toi);

     /**
      *  Serialize the FDT to an XML string
      */
      std::string to_string() const;

     /**
      *  Get all current file entries
      */
      std::vector<FileEntry> file_entries() { return _file_entries; };

    private:
      uint32_t _instance_id;

      std::vector<FileEntry> _file_entries;
      FecOti _global_fec_oti;
      FecTransformer *_fdt_fec_transformer = 0;

      uint64_t _expires;
  };
};
