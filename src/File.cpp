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
  this->create_blocks();
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

  // for no-code
  if (_meta.fec_oti.encoding_id == FecScheme::CompactNoCode) {
    _meta.fec_oti.transfer_length = length;
  } else {
    throw std::runtime_error("Unsupported FEC scheme");
  }

  encode();

  calculate_partitioning();
  create_blocks();
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

  // for no-code
  if (_meta.fec_oti.encoding_id == FecScheme::CompactNoCode) { 
    _meta.fec_oti.transfer_length = length;
  } else {
    throw std::runtime_error("Unsupported FEC scheme");
  }

  this->calculate_partitioning();
  this->create_blocks();
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

  if (symbol.id() >= source_block.symbols.size()) {
    throw std::runtime_error(fmt::format("FLUTE: encoding symbol id {} out of range (block {} has {} symbols)",
                                          symbol.id(), symbol.source_block_number(), source_block.symbols.size()));
  }

  SourceBlock::Symbol& target_symbol = source_block.symbols[symbol.id()];

  if (!target_symbol.complete) {
    symbol.decode_to(target_symbol.data, target_symbol.length);
    target_symbol.complete = true;

    check_source_block_completion(source_block);
    check_file_completion();
  }

}

auto File::check_source_block_completion( SourceBlock& block ) -> void
{
  block.complete = std::all_of(block.symbols.begin(), block.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
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

auto File::calculate_partitioning() -> void
{
  /* Both divisors come from the FEC OTI and both are used as denominators below. A
     default-constructed FecOti leaves them 0, which makes the first division produce inf, the
     block count inf, and the block-creation loop below effectively unbounded: the object hangs
     rather than reporting anything. Reachable through the public File constructors, which accept
     a FecOti without inspecting it. Refused loudly instead, per RULES.md rule 12.
     `code-derived, no spec claim`. */
  if (_meta.fec_oti.encoding_symbol_length == 0 || _meta.fec_oti.max_source_block_length == 0) {
    throw std::runtime_error(
        "FEC OTI is unusable for partitioning: encoding_symbol_length and "
        "max_source_block_length must both be non-zero");
  }

  // Calculate source block partitioning (RFC5052 9.1)
  _nof_source_symbols = ceil((double)_meta.fec_oti.transfer_length / (double)_meta.fec_oti.encoding_symbol_length);
  _nof_source_blocks = ceil((double)_nof_source_symbols / (double)_meta.fec_oti.max_source_block_length);
  _large_source_block_length = ceil((double)_nof_source_symbols / (double)_nof_source_blocks);
  _small_source_block_length = floor((double)_nof_source_symbols / (double)_nof_source_blocks);
  _nof_large_source_blocks = _nof_source_symbols - _small_source_block_length * _nof_source_blocks;
}

auto File::create_blocks() -> void
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

      SourceBlock::Symbol symbol{.data = buffer_ptr, .length = symbol_length, .complete = false};
      block.symbols[ symbol_id++ ] = symbol;
      
      remaining_size -= symbol_length;
      buffer_ptr += symbol_length;
      
      if (remaining_size <= 0) break;
    }
    _source_blocks[number++] = block;
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
      check_source_block_completion(block->second);
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

      /* Select the framing from the encoding, matching inflateInit2 below. windowBits 15 is the
         zlib wrapper, and adding 16 selects gzip instead. Hardcoding gzip here meant a file
         declared as "deflate" was written with gzip framing, so this library could not read back
         what it had just written, and the framing did not match what the declared encoding means.

         RFC 9110 clause 8.4.1.2: "The "deflate" coding is a "zlib" data format [RFC1950]
         containing a "deflate" compressed data stream [RFC1951] that uses a combination of the
         Lempel-Ziv (LZ77) compression algorithm and Huffman coding."

         General FLUTE only in practice: the MBMS Download Profile permits no encoding other than
         gzip, so a 3GPP session never takes the deflate branch. */
      const int window_bits = 15 | ((_meta.content_encoding == "gzip") ? 16 : 0);
      if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
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
          /* zs.msg is NULL for Z_STREAM_ERROR, so the previous form formatted a null char* through
             spdlog and then threw it. Throwing a raw char* also meant only catch(const char*)
             could handle it, and dereferencing the caught null would crash the handler. */
          const char *zmsg = zs.msg ? zs.msg : "no zlib message";
          spdlog::error("Error compressing file {}: {} (zlib status {})", _meta.toi, zmsg, zstate);
          deflateEnd(&zs);
          if (own_decomp) free(decomp_buffer);
          throw std::runtime_error(std::string("Failed to compress file: ") + zmsg);
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
      /* Z_NO_FLUSH, not Z_FINISH. Z_FINISH promises inflate that the output buffer can hold the
         whole result; with the 16384-byte staging buffer below that is only true for small
         objects, and for anything larger inflate returns Z_BUF_ERROR with zs.msg left NULL. The
         loop below continues on Z_OK, so it ran zero times and control fell straight into the
         error branch, which then formatted that NULL pointer. Reproduced standalone: a 100,000
         byte object returned Z_BUF_ERROR immediately with total_out stuck at 16384, and the same
         input with Z_NO_FLUSH reached Z_STREAM_END in six iterations with all 100,000 bytes.
         `code-derived, no spec claim`. */
      auto zstate = inflate(&zs, Z_NO_FLUSH);
      size_t last_out = 0;
      while (zstate == Z_OK) {
        spdlog::debug("Part decompressed: {} bytes", 16384-zs.avail_out);
        _buffer = reinterpret_cast<char*>(realloc(_buffer, zs.total_out));
        memcpy(_buffer+last_out, decomp_buffer.get(), 16384-zs.avail_out);
        last_out = zs.total_out;
        _own_buffer = true;
	zs.avail_out = 16384;
        zs.next_out = decomp_buffer.get();
	zstate = inflate(&zs, Z_NO_FLUSH);
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
        /* zs.msg is NULL for several zlib statuses, so the previous form formatted a null
           char* through spdlog and then threw it, which only catch(const char*) could take. */
        const char *zmsg = zs.msg ? zs.msg : "no zlib message";
        spdlog::error("Error decompressing file {}: {} (zlib status {})", _meta.toi, zmsg, zstate);
        inflateEnd(&zs);
        if (own_comp) free(comp_buffer);
        throw std::runtime_error(std::string("Failed to decompress file: ") + zmsg);
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
