#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "flute_types.h"

namespace LibFlute {
  class FileDeliveryTable {
    public:
      FileDeliveryTable(uint32_t instance_id, FecOti fec_oti);
      FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len);
      virtual ~FileDeliveryTable() {};

      uint32_t instance_id() { return _instance_id; };

      struct FileEntry {
        uint32_t toi;
        std::string content_location;
        uint32_t content_length;
        std::string content_md5;
        std::string content_type;
        uint64_t expires;
        FecOti fec_oti;
      };

      void set_expires(uint64_t exp) { _expires = exp; };
      void add(const FileEntry& entry);
      void remove(uint32_t toi);
      std::string to_string() const;

      std::vector<FileEntry> file_entries() { return _file_entries; };

    private:
      uint32_t _instance_id;

      std::vector<FileEntry> _file_entries;
      FecOti _global_fec_oti;

      uint64_t _expires;
  };
};
