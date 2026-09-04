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
#include <map>
#include <optional>
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
      FileDeliveryTable(uint32_t instance_id, FecOti fec_oti, FdtNamespace fdt_namespace = FDT_NS_NONE,
                        Profile profile = Profile::Ts26517);

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
      *  Whether this FDT Instance may still be used to interpret arriving packets.
      *
      *  TS 26.346 V18.2.0 clause 7.2.9: "For MBMS operation, the UE shall not use a received FDT
      *  Instance to interpret packets received beyond the expiration time of the FDT Instance."
      *  The same clause notes this is stricter than RFC 3926, which only says SHOULD NOT, so it is
      *  enforced under the 3GPP profiles and advisory outside them.
      */
      bool expired(uint64_t now) const { return _expires != 0 && now > _expires; }

     /** The FDT-Instance Expires attribute, in NTP-epoch seconds. */
      uint64_t expires() const { return _expires; }

     /** 20-bit field width (RFC 3926 clause 3.4.1, "FDT Instance ID, 20 bits"). */
      static constexpr uint32_t kMaxFdtInstanceId = 0xFFFFF;

     /**
      *  Next FDT Instance ID in the sequence RFC 3926 clause 3.4.1 defines, exposed as a pure
      *  function so the wraparound is testable without a live session.
      */
      static uint32_t next_instance_id(uint32_t current, uint64_t current_expires, uint64_t now,
                                       std::map<uint32_t, uint64_t>& expired_instance_ids);

     /**
      *  Which obligation set this FDT is emitted under. See Profile.
      */
      Profile profile() const { return _profile; };

     /**
      *  An entry for a file in the FDT
      */
      struct FileEntry {
        uint32_t toi;
        std::string content_location;
        uint32_t content_length;
        std::string content_md5;
        std::string content_type;
        uint64_t expires;   //< File@Expires, 0 when the attribute was absent
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
      *  When the given entry stops being usable, in NTP-epoch seconds.
      *
      *  TS 26.346 V18.2.0 annex L: "When the optional File@Expires attribute is provided, its value
      *  shall take precedence over that of the FDT@Expires attribute." So the File attribute wins
      *  where present, and the FDT-Instance value applies otherwise.
      */
      uint64_t effective_expiry(const FileEntry& entry) const {
        return entry.expires ? entry.expires : _expires;
      }

     /**
      *  Set the expiry value
      */
     /**
      *  Set the FDT-Instance Expires attribute, in NTP-epoch seconds.
      *
      *  Refuses a time that is not in the future. RFC 3926 clause 3.3: "A sender MUST use an expiry
      *  time in the future upon creation of an FDT Instance relative to its Sender Current Time
      *  (SCT)." Binding under every profile, the 3GPP ones inheriting it through TS 26.346 clause
      *  7.2.0's adoption of RFC 3926.
      */
      void set_expires(uint64_t exp);

     /**
      *  Set the RFC 3926 clause 3.4.2 Complete attribute: true once this FDT Instance describes the full,
      *  final set of files for the session (no further files will ever be announced).
      */
      void set_complete(bool complete) { _complete = complete; };

     /**
      *  Get the Complete attribute (defaults to false if the FDT-Instance never carried one).
      */
      bool complete() const { return _complete; };

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
      /**
       * Advance _instance_id to a value that is safe to reuse.
       *
       * The wrap itself is required.
       * RFC 3926 clause 3.4.1: "After reaching the maximum value (2^20-1), the numbering starts
       * again from '0'."
       *
       * Waiting for the previous holder of an ID to expire before reusing it is a recommendation
       * on the sender, not an obligation, so this is deliberately stronger than the clause asks.
       * Per RFC 3926 clause 3.4.1 it would be reasonable for
       * "FLUTE Senders to only construct and deliver FDT Instances with wraparound IDs after the
       * previous FDT Instance using the same ID has expired."
       * (The clause's own sentence begins "It would be reasonable for"; it is split across a page
       * boundary in the published text, so only the contiguous remainder is quoted here.)
       *
       * Records the outgoing ID's expiry, then either increments linearly or, once the 20-bit
       * space is exhausted, wraps to the smallest ID whose recorded expiry has already passed.
       */
      void advance_instance_id();

      uint32_t _instance_id;
      uint32_t _instance_id_sent;
      Profile _profile = Profile::Ts26517;

      /** FDT Instance IDs that have been sent, and the (NTP-epoch-seconds) time each stops
       *  being live -- i.e. the Expires value that was in effect while that ID was in use.
       *  Read on wraparound to warn when an ID is reused before the previous instance expired. */
      std::map<uint32_t, uint64_t> _expired_instance_ids;


      std::vector<FileEntry> _file_entries;
      FecOti _global_fec_oti;

      /* Zero means "no Expires set yet", which is how expired() reads it. Without an
         initialiser the sender-side constructor leaves this indeterminate: expired() and
         advance_instance_id() then read an indeterminate value, and to_string() would write one
         into the FDT-Instance Expires attribute for any caller that reaches it before
         set_expires(). code-derived, no spec claim. */
      uint64_t _expires = 0;
      bool _complete = false;

      FdtNamespace _fdt_namespace;
  };
};
