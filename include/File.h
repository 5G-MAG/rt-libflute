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
#include <map>
#include <memory>
#include "AlcPacket.h"
#include "FileDeliveryTable.h"
#include "EncodingSymbol.h"
#include "Transmitter.h"
#include "fec/FecBlockCodec.h"
#include "fec/RaptorCodec.h"

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
      *  Create a file from a Transmitter::FileDescription (used for transmission)
      *
      *  @param file_description Transmitter File Description
      */
      File(const std::shared_ptr<Transmitter::FileDescription> &file_description);

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
      size_t length() const { return _been_decoded?_meta.content_length:_meta.fec_oti.transfer_length; };

     /**
      *  Encode the buffer using the Content-Encoding
      */
      void encode();

     /**
      *  Decode the buffer using the Content-Encoding
      *
      *  Will check the MD5 sum after decoding, if present.
      */
      void decode();

     /**
      *  Check if the buffer is content encoded
      */
      bool is_encoded() const { return _been_encoded || !_been_decoded; };

     /**
      *  Get the FEC OTI values
      */
      const FecOti& fec_oti() const { return _meta.fec_oti; };

     /**
      *  Get the file metadata from its FDT entry
      */
      const LibFlute::FileDeliveryTable::FileEntry& meta() const { return _meta; };

     /**
      *  Fill in the FDT-derived fields (content_location, content_type, ...) once the FDT
      *  entry for this TOI becomes available. Used when reception started from a packet's own
      *  EXT_FTI before the describing FDT arrived, so the in-progress reception isn't discarded
      *  and restarted once it does. Only content_location/content_type/content_md5/expires are
      *  taken from the FDT entry -- fec_oti is left as-is, since it already came from the
      *  packet's own EXT_FTI and is what the in-flight reassembly is keyed on.
      */
      void adopt_fdt_metadata(const LibFlute::FileDeliveryTable::FileEntry& fdt_entry) {
        _meta.content_location = fdt_entry.content_location;
        _meta.content_type = fdt_entry.content_type;
        _meta.content_md5 = fdt_entry.content_md5;
        _meta.expires = fdt_entry.expires;
      };

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
       *  Set the FEC redundancy level for this object, as a percentage of a source block's k
       *  symbols. Ignored where the FEC OTI already fixes the budget through
       *  max_number_of_encoding_symbols, and for a scheme that has no repair symbols.
       */
      void set_fec_redundancy_level( uint32_t percent ) { _fec_redundancy_level = percent; };

     /**
      *  Get the FDT instance ID
      */
      uint16_t fdt_instance_id() { return _fdt_instance_id; };

     /**
      *  Inject bytes recovered out-of-band (TS 26.517 cl.6.2.4.6 step 2: "the MBSTF Client
      *  shall ... use the recovered data to fill in the missing parts of the object"), e.g. via
      *  an MBS-4-UC unicast repair GET (cl.6.2.4 -- RequestRepairRanges() and friends), rather
      *  than a symbol received over FLUTE. `byte_offset` is a plain offset into the file's own
      *  byte buffer, using the exact same flat numbering missing_symbol_esis()'s own contract
      *  already promises callers (`esi * fec_oti().encoding_symbol_length`) -- so a recovered
      *  LibFlute::RecoveredRange (or the caller's own equivalent) can be passed straight through
      *  with no re-derivation. `data` is chopped into fec_oti().encoding_symbol_length-sized
      *  pieces (the trailing piece may be shorter, for the file's last, partial symbol, matching
      *  every other symbol-length handling in this class); each piece must align exactly with
      *  one source symbol's own byte range -- behaviour is undefined otherwise, the same
      *  precondition style missing_symbol_esis() already documents for its own callers.
      *
      *  Only source symbol slots not already complete are written -- a byte range that overlaps
      *  or duplicates data already received over FLUTE is left untouched there, the same
      *  idempotency put_symbol() already has. Runs the identical completion bookkeeping
      *  put_symbol() does (check_source_block_completion()/check_file_completion()), so a file
      *  completed entirely (or partly) via recovered bytes reports complete() and passes its own
      *  MD5 check exactly like one completed entirely over FLUTE.
      *
      *  @param byte_offset Offset into the file's own byte buffer where `data` starts.
      *  @param data The recovered bytes.
      */
      void put_recovered_bytes(uint64_t byte_offset, const std::vector<uint8_t>& data);

     /**
      *  Get the sorted list of missing source symbol Encoding Symbol Identifiers (ESIs),
      *  as a flat index across the whole file rather than per-source-block-local -- i.e.
      *  block 0's K0 symbols are ESIs 0..K0-1, block 1's are K0..K0+K1-1, and so on,
      *  matching how source symbol bytes are laid out contiguously across blocks in the
      *  file buffer (see create_blocks()). This flat numbering is exactly what TS 26.517
      *  cl.6.2.4.5's minimal-byte-range pseudocode assumes (Range[m].start = SS[n] * T):
      *  each returned ESI maps directly to byte offset `esi * fec_oti().encoding_symbol_length`.
      *
      *  For Compact-No-Code FEC (a single source block, RFC 5052), this is exactly what
      *  TS 26.517 cl.6.2.4.5 asks for: the source symbols not yet received.
      *
      *  For Raptor/RaptorQ, this returns every missing source symbol across all blocks --
      *  a correct, always-sufficient set for recovery, but NOT necessarily the *minimal*
      *  one TS 26.517 cl.6.2.4.5 describes for that scheme (which additionally accounts
      *  for already-received REPAIR symbols reducing how many source symbols are still
      *  actually needed, per TR 26.946 cl.6.1.3.1's decoder-rank-aware method -- that
      *  refinement needs the FEC decoder's internal linear-dependency state, which this
      *  method does not inspect). Returning this superset is a bandwidth-efficiency gap
      *  for Raptor/RaptorQ repair requests, not a correctness one: the extra byte ranges
      *  it implies are simply not strictly necessary, never wrong.
      */
      std::vector<uint32_t> missing_symbol_esis() const;

    private:
      void calculate_partitioning();
      // have_source_data: true from the transmit-side constructors (the
      // file's bytes are already in _buffer, so source symbols start out
      // complete and, for Raptor, the intermediate symbols get solved
      // immediately); false from the receive-side constructor (empty
      // buffer, everything arrives via put_symbol()).
      void create_blocks(bool have_source_data);

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

      void check_source_block_completion(uint16_t source_block_number, SourceBlock& block);
      void check_file_completion();

      // -- Raptor support -----------------------------------------------------
      // File keeps the same SourceBlock/Symbol bookkeeping above for every
      // scheme (source symbols are always the file's raw bytes, chopped up
      // identically -- Raptor is a systematic code); a RaptorCodec per source
      // block is the only extra state needed, handling the pre-coding/LT maths
      // in complete isolation from this class. See fec/RaptorCodec.h for the
      // codec itself.
      //
      // Encoder side: create_blocks() feeds all K source symbols of a block
      // into its codec once and keeps the resulting intermediate symbols
      // around (raptor_intermediate), so get_next_symbols() can manufacture
      // any repair ESI on demand via RaptorCodec::generate_encoding_symbol().
      //
      // Decoder side: put_symbol() feeds every received symbol (source or
      // repair) into the block's codec; the moment it becomes decodable, all
      // of that block's source symbol slots are filled in one shot, whether
      // or not they'd individually arrived.
      bool is_raptor_family() const {
        return _meta.fec_oti.encoding_id == FecScheme::Raptor;
      }
      void calculate_partitioning_raptor();
      void setup_raptor_codec_for_block(uint16_t sbn, uint32_t k);
      // How many repair (ESI >= K) symbols to generate for a block of k source
      // symbols. Exactly one of two operator-set sources decides it: the FEC
      // OTI's max_number_of_encoding_symbols, which fixes the total budget for
      // the block, or failing that the session's FEC redundancy level, a
      // percentage of k. Neither is invented here.
      uint32_t nof_repair_symbols_for_block(uint32_t k) const;

      // FEC redundancy level for this object, as a percentage of a source
      // block's k symbols. See kDefaultFecRedundancyLevel for the unit, what
      // sets it, and why it is not signalled.
      uint32_t _fec_redundancy_level = kDefaultFecRedundancyLevel;

      std::map<uint16_t, std::shared_ptr<FecBlockCodec>> _raptor_codecs; // one RaptorCodec per source block
      std::map<uint16_t, std::vector<std::vector<uint8_t>>> _raptor_intermediate; // encoder side only, filled once per block
      std::map<uint16_t, uint32_t> _raptor_repair_sent; // encoder side only: how many repair ESIs already queued for this block
      // encoder side only: generated repair symbol bytes, cached so the
      // pointers EncodingSymbol hands to the (possibly async) send path stay
      // valid for as long as source-symbol pointers into _buffer do. Once
      // queued a repair ESI is never regenerated or resent -- if that
      // specific packet is lost in transit it's simply gone, same as any
      // other repair symbol that was never received; the redundancy is in
      // the overhead count as a whole, not in any one symbol.
      std::map<uint16_t, std::map<uint32_t, std::vector<uint8_t>>> _raptor_repair_data;

      std::map<uint16_t, SourceBlock> _source_blocks;

      bool _complete = false;;

      uint32_t _nof_source_symbols = 0;
      uint32_t _nof_source_blocks = 0;
      uint32_t _nof_large_source_blocks = 0;
      uint32_t _large_source_block_length = 0;
      uint32_t _small_source_block_length = 0;

      char* _buffer = nullptr;
      bool _own_buffer = false;
      bool _been_encoded = false;
      bool _been_decoded = false;

      LibFlute::FileDeliveryTable::FileEntry _meta;
      unsigned long _received_at;
      unsigned _access_count = 0;

      uint16_t _fdt_instance_id = 0;

      std::shared_ptr<Transmitter::FileDescription> _file_description;
  };
};
