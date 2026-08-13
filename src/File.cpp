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
#include <iostream>
#include <string>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <sstream>
#include <iomanip>
// Suppress warnings about MD5 being deprecated in later versions of OpenSSL
#define OPENSSL_SUPPRESS_DEPRECATED 1
#include <openssl/md5.h>
#include <zlib.h>

#include "base64.h"
#include "spdlog/spdlog.h"
#include "spdlog/fmt/fmt.h"
#include "Transmitter.h"
#include "File.h"

namespace LibFlute {

File::File(FileDeliveryTable::FileEntry entry)
  : _meta( std::move(entry) )
  , _received_at( time(nullptr) )
  , _file_description()
{
  spdlog::debug("Creating File from FileEntry");
  // Allocate a data buffer
  spdlog::debug("Allocating buffer");
  _buffer = (char*)malloc(_meta.fec_oti.transfer_length);
  if (_buffer == nullptr)
  {
    throw std::runtime_error("Failed to allocate file buffer");
  }
  _own_buffer = true;

  this->calculate_partitioning();
  this->create_blocks(false /* receiving: no source data yet */);
}

File::File(const std::shared_ptr<Transmitter::FileDescription> &file_description)
  : _meta()
  , _file_description(file_description)
{
  spdlog::debug("Creating File from FileDescription");

  auto length = _file_description->data_length();
  _buffer = (char*)malloc(length);
  if (_buffer == nullptr)
  {
    throw std::runtime_error("No data allocated");
  }
  _own_buffer = true;
  memcpy(_buffer, _file_description->data(), length);
  _meta = _file_description->file_entry();

  if (_meta.fec_oti.encoding_id == FecScheme::CompactNoCode ||
      _meta.fec_oti.encoding_id == FecScheme::Raptor) {
    _meta.fec_oti.transfer_length = length;
  } else {
    throw std::runtime_error("Unsupported FEC scheme");
  }

  encode();

  calculate_partitioning();
  create_blocks(true /* transmitting: source data already in _buffer */);
}

File::File(uint32_t toi,
    FecOti fec_oti,
    std::string content_location,
    std::string content_type,
    uint64_t expires,
    char* data,
    size_t length,
    bool copy_data)
  : _own_buffer(false)
  , _meta()
  , _file_description()
{
  spdlog::debug("Creating File from data");
  if (copy_data) {
    spdlog::debug("Allocating buffer");
    _buffer = (char*)malloc(length);
    if (_buffer == nullptr)
    {
      throw std::runtime_error("Failed to allocate file buffer");
    }
    memcpy(_buffer, data, length);
    _own_buffer = true;
  } else {
    _buffer = data;
  }

  unsigned char md5[MD5_DIGEST_LENGTH];
  MD5((const unsigned char*)data, length, md5);

  _meta.toi = toi;
  _meta.content_location = std::move(content_location);
  _meta.content_type = std::move(content_type);
  _meta.content_length = length;
  _meta.content_md5 = base64_encode(md5, MD5_DIGEST_LENGTH);
  _meta.expires = expires;
  _meta.fec_oti = fec_oti;

  if (_meta.fec_oti.encoding_id == FecScheme::CompactNoCode ||
      _meta.fec_oti.encoding_id == FecScheme::Raptor) {
    _meta.fec_oti.transfer_length = length;
  } else {
    throw std::runtime_error("Unsupported FEC scheme");
  }

  this->calculate_partitioning();
  this->create_blocks(true /* transmitting: source data already in _buffer */);
}

File::~File()
{
  spdlog::debug("Destroying File");
  if (_own_buffer && _buffer != nullptr)
  {
    spdlog::debug("Freeing buffer");
    free(_buffer);
  }
}

auto File::put_symbol( const EncodingSymbol& symbol ) -> void
{
  // Bounds must be ">=", not ">": a valid source-block number is
  // 0.._source_blocks.size()-1, so SBN == size() is already out of range and
  // indexing with it below would be undefined behaviour. Throw a std::exception
  // (not a bare const char*, which escapes the receiver's catch(std::exception&)
  // and terminates the process) so an out-of-range symbol -- e.g. a content
  // object whose on-air symbol layout exceeds its FDT-declared FEC-OTI -- is
  // dropped and logged by the caller instead of crashing the client.
  if (symbol.source_block_number() >= _source_blocks.size()) {
    throw std::runtime_error(fmt::format("FLUTE: source block number {} out of range (have {} blocks)",
                                          symbol.source_block_number(), _source_blocks.size()));
  }

  SourceBlock& source_block = _source_blocks[ symbol.source_block_number() ];

  /* The encoding symbol ID is bounded by the source symbol count only for a non-fountain code.
     Raptor generates repair symbols with IDs beyond that count on purpose, so the check belongs
     inside the non-fountain branch below and must not be hoisted out of it.
     RFC 5053 clause 1: "Raptor is a fountain code, i.e., as many encoding symbols as needed can
     be generated by the encoder on-the-fly from the source symbols of a source block of data." */
  if (!is_raptor_family()) {
    if (symbol.id() >= source_block.symbols.size()) {
      throw std::runtime_error(fmt::format("FLUTE: encoding symbol id {} out of range (block {} has {} symbols)",
                                            symbol.id(), symbol.source_block_number(), source_block.symbols.size()));
    }

    SourceBlock::Symbol& target_symbol = source_block.symbols[symbol.id()];

    if (!target_symbol.complete) {
      symbol.decode_to(target_symbol.data, target_symbol.length);
      target_symbol.complete = true;

      check_source_block_completion(symbol.source_block_number(), source_block);
      check_file_completion();
    }
    return;
  }

  // Raptor: unlike Compact No-Code, an ESI at or beyond K is
  // meaningful -- it's a repair symbol, not an error -- so there's no upper
  // bound to enforce here beyond not letting a malicious/corrupt sender grow
  // our per-block state unboundedly.
  auto k = (uint32_t)source_block.symbols.size();
  const uint32_t kMaxEsiSlack = 100000; // generous; just a sanity backstop
  if (symbol.id() >= k + kMaxEsiSlack) {
    throw std::runtime_error(fmt::format("FLUTE: encoding symbol id {} implausibly far beyond block {}'s {} source symbols",
                                          symbol.id(), symbol.source_block_number(), k));
  }

  bool is_source_esi = symbol.id() < k;
  if (is_source_esi) {
    SourceBlock::Symbol& target_symbol = source_block.symbols[symbol.id()];
    if (!target_symbol.complete) {
      symbol.decode_to(target_symbol.data, target_symbol.length);
      target_symbol.complete = true;
    }
  }

  auto codec_it = _raptor_codecs.find(symbol.source_block_number());
  if (codec_it == _raptor_codecs.end()) {
    return; // shouldn't happen: create_blocks() sets one up per block
  }
  auto& codec = codec_it->second;

  if (!codec->can_decode()) {
    // Zero-pad to a full T bytes for the same reason create_blocks() does on
    // the encoder side: the codec's linear system requires every symbol to
    // be the same length, but a literal source ESI's wire length can be
    // shorter than T for the file's last, partial symbol.
    auto T = _meta.fec_oti.encoding_symbol_length;
    std::vector<uint8_t> data(symbol.len());
    symbol.decode_to((char*)data.data(), data.size());
    data.resize(T, 0);
    codec->add_received_symbol(symbol.id(), data);
  }

  if (codec->can_decode()) {
    bool any_missing = std::any_of(source_block.symbols.begin(), source_block.symbols.end(),
                                    [](const auto& s) { return !s.second.complete; });
    if (any_missing) {
      auto decoded = codec->decode_source_symbols();
      for (uint32_t i = 0; i < k; i++) {
        auto& s = source_block.symbols[i];
        if (!s.complete) {
          memcpy(s.data, decoded[i].data(), std::min(s.length, decoded[i].size()));
          s.complete = true;
        }
      }
    }
    check_source_block_completion(symbol.source_block_number(), source_block);
    check_file_completion();
  }
}

auto File::check_source_block_completion( uint16_t source_block_number, SourceBlock& block ) -> void
{
  bool source_done = std::all_of(block.symbols.begin(), block.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });

  /* Transmit side, Raptor/RaptorQ only: a block is not finished when its last source symbol goes
     out, it is finished when its repair budget has gone out too. Treating the last source symbol
     as the end of the block leaves the repair branch of get_next_symbols() unreachable (the branch
     sits under "block not complete") and makes complete() true, so the transmitter drops the file
     and the session carries source symbols only. The receiver then has no repair symbol to decode
     from and a fountain code protects nothing, which is the one thing it exists to do.
     RFC 5053 clause 1: "Raptor is a fountain code, i.e., as many encoding symbols as needed can be
     generated by the encoder on-the-fly from the source symbols of a source block of data."
     _raptor_repair_sent carries an entry for this block only where the block was built from source
     data, so this condition is inert on the receive side, where "all source symbols recovered" is
     the correct and only meaning of a complete block. */
  auto repair_sent = _raptor_repair_sent.find(source_block_number);
  if (source_done && repair_sent != _raptor_repair_sent.end()) {
    source_done = repair_sent->second >= nof_repair_symbols_for_block((uint32_t)block.symbols.size());
  }

  block.complete = source_done;
}

auto File::check_file_completion() -> void
{
  _complete = std::all_of(_source_blocks.begin(), _source_blocks.end(), [](const auto& block){ return block.second.complete; });

  if (_complete && !_meta.content_md5.empty() && _meta.content_encoding.empty()) {
    //check MD5 sum if we haven't encoded the contents
    unsigned char md5[MD5_DIGEST_LENGTH];
    MD5((const unsigned char*)buffer(), length(), md5);

    auto content_md5 = base64_decode(_meta.content_md5);
    if (memcmp(md5, content_md5.c_str(), MD5_DIGEST_LENGTH) != 0) {
      spdlog::debug("MD5 mismatch for TOI {}, discarding", _meta.toi);

      // MD5 mismatch, try again
      for (auto& block : _source_blocks) {
        for (auto& symbol : block.second.symbols) {
          symbol.second.complete = false;
        }
        block.second.complete = false;
      } 
      _complete = false;
    }
  }
}

auto File::missing_symbol_esis() const -> std::vector<uint32_t>
{
  std::vector<uint32_t> missing;
  // _source_blocks is keyed by sbn (source block number), and each block's own
  // `symbols` map is keyed by a block-local ESI -- both iterate in ascending key order
  // (std::map), which matches create_blocks()'s own sequential fill order (block 0's
  // bytes first, then block 1's, ...; within a block, local ESI 0's bytes first, then
  // 1's, ...). Accumulating global_base across blocks in this same order therefore
  // yields a flat ESI list that is both correctly file-byte-order-consistent and
  // already sorted ascending -- exactly what File.h's contract promises callers (e.g.
  // TS 26.517 cl.6.2.4.5's ComputeRepairByteRanges(), which assumes both).
  uint32_t global_base = 0;
  for (const auto& block_entry : _source_blocks) {
    const SourceBlock& block = block_entry.second;
    for (const auto& symbol_entry : block.symbols) {
      if (!symbol_entry.second.complete) {
        missing.push_back(global_base + static_cast<uint32_t>(symbol_entry.first));
      }
    }
    global_base += static_cast<uint32_t>(block.symbols.size());
  }
  return missing;
}

auto File::calculate_partitioning() -> void
{
  if (is_raptor_family()) {
    calculate_partitioning_raptor();
    return;
  }

  // Calculate source block partitioning (RFC5052 9.1)
  _nof_source_symbols = ceil((double)_meta.fec_oti.transfer_length / (double)_meta.fec_oti.encoding_symbol_length);
  _nof_source_blocks = ceil((double)_nof_source_symbols / (double)_meta.fec_oti.max_source_block_length);
  _large_source_block_length = ceil((double)_nof_source_symbols / (double)_nof_source_blocks);
  _small_source_block_length = floor((double)_nof_source_symbols / (double)_nof_source_blocks);
  _nof_large_source_blocks = _nof_source_symbols - _small_source_block_length * _nof_source_blocks;
}

auto File::calculate_partitioning_raptor() -> void
{
  // RFC 5053 §4.2 partitioning: Kt = ceil(F/T); (KL, KS, ZL, ZS) =
  // Partition[Kt, Z], where Partition[I, J] = (IL, IS, JL, JS) with
  // IL = ceil(I/J), IS = floor(I/J), JL = I - IS*J, JS = J - JL.
  //
  // We fix N (sub-blocks) = 1 and Al (symbol alignment) = 1 -- see
  // FecOti::nof_sub_blocks's comment in flute_types.h -- so the second
  // partition, Partition[T/Al, N], is trivial and doesn't need computing.
  //
  // K is capped well below Raptor's hard spec limit (8192, RFC 5053 §5.7) by
  // default, since the codec's linear system scales at least quadratically
  // in block size (see GF2LinearSystem.h) -- a caller who actually wants
  // larger blocks can ask for them via max_source_block_length, up to the
  // scheme's real limit.
  const uint32_t kSchemeMaxK = 8192;
  const uint32_t kDefaultK = 8192;
  uint32_t k_cap = _meta.fec_oti.max_source_block_length;
  if (k_cap == 0) k_cap = kDefaultK;
  if (k_cap > kSchemeMaxK) k_cap = kSchemeMaxK;

  uint32_t Kt = (uint32_t)ceil((double)_meta.fec_oti.transfer_length / (double)_meta.fec_oti.encoding_symbol_length);
  if (Kt == 0) Kt = 1;
  uint32_t Z = (uint32_t)ceil((double)Kt / (double)k_cap);
  if (Z == 0) Z = 1;

  // Partition[Kt, Z]
  uint32_t IL = (uint32_t)ceil((double)Kt / (double)Z);
  uint32_t IS = Kt / Z; // floor
  uint32_t JL = Kt - IS * Z;
  uint32_t JS = Z - JL;
  (void)JS;

  _nof_source_symbols = Kt;
  _nof_source_blocks = Z;
  _large_source_block_length = IL; // KL: size (in symbols) of the first JL blocks
  _small_source_block_length = IS; // KS: size (in symbols) of the remaining JS blocks
  _nof_large_source_blocks = JL;   // ZL

  _meta.fec_oti.nof_source_blocks = Z;
  _meta.fec_oti.nof_sub_blocks = 1;
  _meta.fec_oti.symbol_alignment = 1;
}

auto File::setup_raptor_codec_for_block(uint16_t sbn, uint32_t k) -> void
{
  _raptor_codecs[sbn] = std::make_shared<Raptor::RaptorCodec>(k);
}

auto File::nof_repair_symbols_for_block(uint32_t k) const -> uint32_t
{
  /* Two operator-set sources, in order of precedence, and no third. Where the FEC OTI names an
     encoding-symbol maximum for the block, that is the budget and the repair count follows from
     it. Otherwise the session's FEC redundancy level applies, as a percentage of k.

     TS 26.346 V18.2.0 clause 7.3.2.11: "For example, a FEC redundancy level of 40% means that
     for an FEC-encoded block of K symbols, 1.4*K symbols are broadcast over the air."

     The previous fallback, max(4, ceil(k * 0.10)), rested on nothing: neither the percentage nor
     the floor of 4 came from a clause, a configuration option or a documented default, and the
     floor quietly gave a small block proportionally more protection than a large one.
     RULES.md rule 12. */
  auto budget = _meta.fec_oti.max_number_of_encoding_symbols;
  if (budget > k) return budget - k;
  if (_fec_redundancy_level == 0) return 0;
  return (uint32_t)std::ceil((double)k * (double)_fec_redundancy_level / 100.0);
}

auto File::create_blocks(bool have_source_data) -> void
{
  // Create the required source blocks and encoding symbols
  auto buffer_ptr = _buffer;
  size_t remaining_size = _meta.fec_oti.transfer_length;
  decltype(_nof_large_source_blocks) number = 0;
  while (remaining_size > 0) {
    SourceBlock block;
    size_t symbol_id = 0;
    auto block_length = ( number < _nof_large_source_blocks ) ? _large_source_block_length : _small_source_block_length;

    for (decltype(block_length) i = 0; i < block_length; i++) {
      auto symbol_length = std::min(remaining_size, (size_t)_meta.fec_oti.encoding_symbol_length);
      assert(buffer_ptr + symbol_length <= _buffer + _meta.fec_oti.transfer_length);

      // `complete` means different things on the two sides that reuse this
      // same struct: "already sent" on the encoder side (see
      // get_next_symbols()/mark_completed()), "data received" on the
      // decoder side (see put_symbol()). Either way it starts false: an
      // encoder-side symbol hasn't been sent yet even though its data is
      // already known, and a decoder-side symbol's data obviously isn't in
      // yet.
      SourceBlock::Symbol symbol{.data = buffer_ptr, .length = symbol_length, .complete = false};
      block.symbols[ symbol_id++ ] = symbol;

      remaining_size -= symbol_length;
      buffer_ptr += symbol_length;

      if (remaining_size <= 0) break;
    }

    if (is_raptor_family()) {
      uint16_t sbn = (uint16_t)number;
      uint32_t k = (uint32_t)block.symbols.size();
      setup_raptor_codec_for_block(sbn, k);

      if (have_source_data) {
        // Encoder side: solve for the intermediate symbols right away, so
        // get_next_symbols() can hand out source or repair ESIs for this
        // block on demand without redoing the pre-coding maths each time.
        //
        // Every symbol handed to the codec must be exactly T bytes: the
        // last symbol of the file's last source block is normally shorter
        // (transfer_length isn't usually an exact multiple of T), so it's
        // zero-padded here purely for the FEC linear algebra -- this is
        // internal to Raptor's maths, not a wire format change; a repair
        // symbol generated from these padded intermediate symbols still
        // comes out as a full T bytes, same as any other repair symbol.
        auto T = _meta.fec_oti.encoding_symbol_length;
        std::vector<std::vector<uint8_t>> source_symbols;
        source_symbols.reserve(k);
        for (auto& s : block.symbols) {
          std::vector<uint8_t> padded((uint8_t*)s.second.data, (uint8_t*)s.second.data + s.second.length);
          padded.resize(T, 0);
          source_symbols.push_back(std::move(padded));
        }
        _raptor_intermediate[sbn] = _raptor_codecs[sbn]->compute_intermediate_symbols(source_symbols);
        _raptor_repair_sent[sbn] = 0;
      }
      // Receive side (have_source_data == false): the codec is ready to
      // accept received symbols via put_symbol(); nothing else to do yet.
    }

    _source_blocks[number] = block;
    number++;
  }
}

auto File::get_next_symbols(size_t max_size) -> std::vector<EncodingSymbol> 
{
  int nof_symbols = std::ceil((float)(max_size - 4) / (float)_meta.fec_oti.encoding_symbol_length);
  auto cnt = 0;
  std::vector<EncodingSymbol> symbols;
  
  for (auto& block : _source_blocks) {
    if (cnt >= nof_symbols) break;

    if (!block.second.complete) {
      for (auto& symbol : block.second.symbols) {
        if (cnt >= nof_symbols) break;

        if (!symbol.second.complete && !symbol.second.queued) {
          symbols.emplace_back(symbol.first, block.first, symbol.second.data, symbol.second.length, _meta.fec_oti.encoding_id);
          symbol.second.queued = true;
          cnt++;
        }
      }

      if (is_raptor_family() && cnt < nof_symbols) {
        /* Either in flight or already acknowledged: mark_completed() clears "queued" when a
           symbol is sent, so testing "queued" alone closes this gate permanently the moment the
           block's last source symbol is acknowledged, which is exactly when repair must start. */
        bool all_source_sent = std::all_of(block.second.symbols.begin(), block.second.symbols.end(),
                                           [](const auto& s) { return s.second.queued || s.second.complete; });
        if (all_source_sent) {
          uint16_t sbn = block.first;
          uint32_t k = (uint32_t)block.second.symbols.size();
          uint32_t budget = nof_repair_symbols_for_block(k);
          auto& sent = _raptor_repair_sent[sbn];
          auto& cache = _raptor_repair_data[sbn];
          auto& intermediate = _raptor_intermediate[sbn];
          auto& codec = _raptor_codecs[sbn];
          while (sent < budget && cnt < nof_symbols) {
            uint32_t esi = k + sent;
            auto it = cache.find(esi);
            if (it == cache.end()) {
              it = cache.emplace(esi, codec->generate_encoding_symbol(esi, intermediate)).first;
            }
            symbols.emplace_back(esi, sbn, (char*)it->second.data(), it->second.size(), _meta.fec_oti.encoding_id);
            sent++;
            cnt++;
          }
        }
      }
    }
  }
  return symbols;

}

auto File::mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) -> void
{
  for (auto& symbol : symbols) {
    auto block = _source_blocks.find(symbol.source_block_number());
    if (block != _source_blocks.end()) {
      auto sym = block->second.symbols.find(symbol.id());
      if (sym != block->second.symbols.end()) {
        sym->second.queued = false;
        sym->second.complete = success;
      }
      check_source_block_completion(symbol.source_block_number(), block->second);
      check_file_completion();
    }
  }
}

auto File::encode() -> void
{
  if (!_been_encoded && !_meta.content_encoding.empty()) {
    if (_meta.content_encoding == "gzip" || _meta.content_encoding=="deflate") {
      auto decomp_buffer = _buffer;
      bool own_decomp = _own_buffer;
      std::shared_ptr<unsigned char[]> comp_buffer(new unsigned char[16384]);
      z_stream zs = {
        .next_in = reinterpret_cast<unsigned char*>(decomp_buffer),
        .avail_in = static_cast<uint32_t>(_meta.content_length),
        .next_out = comp_buffer.get(),
        .avail_out = 16384
      };
      spdlog::debug("Compressing contents with {}", _meta.content_encoding);

      if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
        _buffer = nullptr;
        auto zstate = deflate(&zs, Z_FINISH);
        size_t last_out = 0;
        while (zstate == Z_OK) {
          spdlog::debug("Part compressed: {} bytes", 16384-zs.avail_out);
          _buffer = reinterpret_cast<char*>(realloc(_buffer, zs.total_out));
          memcpy(_buffer+last_out, comp_buffer.get(), 16384-zs.avail_out);
          last_out = zs.total_out;
          _own_buffer = true;
          zs.avail_out = 16384;
          zs.next_out = comp_buffer.get();
          zstate = deflate(&zs, Z_FINISH);
        }
        if (zstate==Z_STREAM_END) {
          if (last_out != zs.total_out) {
            spdlog::debug("Finish compress, last block is {} bytes. Total {} bytes", 16384-zs.avail_out, zs.total_out);
            _buffer = reinterpret_cast<char*>(realloc(_buffer, zs.total_out));
            memcpy(_buffer+last_out, comp_buffer.get(), 16384-zs.avail_out);
            _own_buffer = true;
          }
          _meta.fec_oti.transfer_length = zs.total_out;
        } else {
          spdlog::error("Error compressing file {}: {}", _meta.toi, zs.msg);
          throw zs.msg;
        }
        deflateEnd(&zs);

        if (own_decomp) free(decomp_buffer);
      }
    } else {
      spdlog::error("Unknown Content-Encoding {}", _meta.content_encoding);
      throw std::runtime_error("Content-Encoding not known");
    }

    _been_encoded = true;
    _been_decoded = false;
  }
}

auto File::decode() -> void
{
  if (!_been_decoded && !_meta.content_encoding.empty()) {
    if (_meta.content_encoding == "gzip" || _meta.content_encoding=="deflate") {
      auto comp_buffer = _buffer;
      bool own_comp = _own_buffer;
      std::shared_ptr<unsigned char[]> decomp_buffer(new unsigned char[16384]);
      z_stream zs = {
	.next_in = reinterpret_cast<unsigned char*>(comp_buffer),
	.avail_in = static_cast<uint32_t>(_meta.fec_oti.transfer_length),
        .next_out = decomp_buffer.get(),
        .avail_out = 16384
      };
      spdlog::debug("Decompressing contents with {}", _meta.content_encoding);

      inflateInit2(&zs, 15 | ((_meta.content_encoding == "gzip")?16:0));
      _buffer = nullptr;
      auto zstate = inflate(&zs, Z_FINISH);
      size_t last_out = 0;
      while (zstate == Z_OK) {
        spdlog::debug("Part decompressed: {} bytes", 16384-zs.avail_out);
        _buffer = reinterpret_cast<char*>(realloc(_buffer, zs.total_out));
        memcpy(_buffer+last_out, decomp_buffer.get(), 16384-zs.avail_out);
        last_out = zs.total_out;
        _own_buffer = true;
	zs.avail_out = 16384;
        zs.next_out = decomp_buffer.get();
	zstate = inflate(&zs, Z_FINISH);
      }
      if (zstate==Z_STREAM_END) {
	if (last_out != zs.total_out) {
          spdlog::debug("Finish decompress, last block is {} bytes. Total {} bytes", 16384-zs.avail_out, zs.total_out);
          _buffer = reinterpret_cast<char*>(realloc(_buffer, zs.total_out));
          memcpy(_buffer+last_out, decomp_buffer.get(), 16384-zs.avail_out);
          _own_buffer = true;
        }
        if (!_meta.content_length) {
          _meta.content_length = zs.total_out;
        } else if (_meta.content_length != zs.total_out) {
          spdlog::error("Decompressed length does not match expected Content-Length ({} != {})", _meta.content_length, zs.total_out);
        }
      } else {
        spdlog::error("Error decompressing file {}: {}", _meta.toi, zs.msg);
	throw zs.msg;
      }

      if (own_comp) free(comp_buffer);
    } else {
      spdlog::error("Unknown Content-Encoding {}", _meta.content_encoding);
      throw std::runtime_error("Content-Encoding not known");
    }

    _been_decoded = true;
    _been_encoded = false;

    // Check MD5
    if (!_meta.content_md5.empty()) {
      unsigned char md5[MD5_DIGEST_LENGTH];
      MD5((const unsigned char*)buffer(), length(), md5);

      auto content_md5 = base64_decode(_meta.content_md5);
      if (memcmp(md5, content_md5.c_str(), MD5_DIGEST_LENGTH) != 0) {
        spdlog::debug("MD5 mismatch for TOI {}, discarding", _meta.toi);

        // MD5 mismatch, try again
        for (auto& block : _source_blocks) {
          for (auto& symbol : block.second.symbols) {
            symbol.second.complete = false;
          }
          block.second.complete = false;
        }
        _complete = false;
      }
    }
  }
}

} // end namespace LibFlute
