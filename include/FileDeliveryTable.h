// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//               2025 British Broadcasting Corporation (David Waring <david.waring2@bbc.co.uk>)
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
#include <optional>
#include <set>
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
      * FDT namespace enumeration
      */
      enum FdtNamespace {
          FDT_NS_NONE = 0,
          FDT_NS_RFC3926,
          FDT_NS_DRAFT_2005,
//          FDT_NS_RFC6726, // FLUTE v2 - will need other things implementing to use this correctly
          FDT_NS_3GPP_CONSOLIDATED_V2
      };

     /**
      *  Create an empty FDT
      *
      *  @param instance_id FDT instance ID to set
      *  @param fec_oti Global FEC OTI parameters
      *  @param fdt_namespace The XML namespace to use for FDT
      */
      FileDeliveryTable(uint32_t instance_id, FecOti fec_oti, FdtNamespace fdt_namespace = FDT_NS_NONE);

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
      virtual ~FileDeliveryTable() {};

     /**
      *  Get the FDT instance ID
      */
      uint32_t instance_id() { return _instance_id; };

     /**
      *  The FDT Instance ID is a 20-bit field on the wire (RFC 6726 SS3.4.1).
      */
      static constexpr uint32_t kMaxFdtInstanceId = 0xFFFFF;

     /**
      *  Compute the next FDT Instance ID to use (RFC 6726 SS3.4.1): normally just
      *  @p current + 1, but once the 20-bit ID space is exhausted, wrap to the
      *  smallest ID recorded in @p expired_ids instead of just letting the value
      *  overflow and get bit-masked on the wire (which could collide with an ID a
      *  receiver still considers live). @p current is added to @p expired_ids (it
      *  is being superseded); if a wrapped ID is reused, it is removed from
      *  @p expired_ids.
      *
      *  Exposed as a static method, taking its state explicitly, so the
      *  wraparound behaviour itself is independently testable without needing
      *  ~2^20 real FDT changes to reach the ceiling.
      *
      *  @param current The FDT Instance ID currently in use (about to be replaced)
      *  @param expired_ids IDs known to no longer be associated with a live FDT
      *         Instance; updated in place
      *  @return The next FDT Instance ID to use
      */
      static uint32_t next_instance_id(uint32_t current, std::set<uint32_t>& expired_ids);

     /**
      *  Is this FDT Instance marked Complete (RFC 6726 SS3.4.1 `Complete`
      *  FDT-Instance attribute)? Complete indicates no further FDT Instances will
      *  describe additional files for this session -- the file set is final.
      */
      bool complete() const { return _complete; };

     /**
      *  Mark this FDT Instance Complete (or not, see complete()). Marking Complete
      *  does not itself allocate a new FDT Instance ID or trigger a resend; the
      *  caller (e.g. Transmitter::close_session()) is responsible for that.
      */
      void set_complete(bool complete) { _complete = complete; };

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
        struct {
          bool no_cache;
          std::optional<uint64_t> cache_expires;
        } cache_control;
        std::string content_encoding;
        std::string etag;

        bool operator==(const FileEntry &other) const;
        bool operator!=(const FileEntry &other) const { return !(*this == other); };
      };

     /**
      *  Set the expiry value
      */
      void set_expires(uint64_t exp) { _expires = exp; };

     /**
      *  Add a file entry
      */
      void add(const FileEntry& entry);

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
      const std::vector<FileEntry> &file_entries() const {
        return _file_entries;
      };

     /**
      * Mark instance sent
      */
      void sent() { _instance_id_sent = _instance_id; };

    private:
      void _advance_instance_id();

      uint32_t _instance_id;
      uint32_t _instance_id_sent;
      std::set<uint32_t> _expired_instance_ids;

      std::vector<FileEntry> _file_entries;
      FecOti _global_fec_oti;

      uint64_t _expires;
      bool _complete = false;

      FdtNamespace _fdt_namespace;
  };
};
