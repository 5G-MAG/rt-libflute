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
#include "File.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <openssl/md5.h>
#include <openssl/evp.h>
#include "base64.h"
#include "spdlog/spdlog.h"

LibFlute::File::File(LibFlute::FileDeliveryTable::FileEntry entry)
  : _meta( std::move(entry) )
  , _received_at( time(nullptr) )
{
  spdlog::debug("Creating File from FileEntry");
  // Allocate a data buffer
  spdlog::debug("Allocating buffer");
  if (_meta.fec_transformer){
    _buffer = (char*) _meta.fec_transformer->allocate_file_buffer(_meta.fec_oti.transfer_length);
  } else {
    _buffer = (char*) malloc(_meta.fec_oti.transfer_length);
  }
  if (_buffer == nullptr)
  {
    throw "Failed to allocate file buffer";
  }
  _own_buffer = true;

  calculate_partitioning();
  create_blocks();
}

LibFlute::File::File(uint32_t toi,
    FecOti fec_oti,
    std::string content_location,
    std::string content_type,
    uint64_t expires,
    char* data,
    size_t length,
    bool copy_data) 
{
  if (!data) {
    spdlog::error("File pointer is null");
    throw "Invalid file";
  }

  spdlog::debug("Creating File from data");

  if (copy_data) {
    spdlog::debug("Allocating buffer");
    _buffer = (char*)malloc(length);
    if (_buffer == nullptr)
    {
      throw "Failed to allocate file buffer";
    }
    memcpy(_buffer, data, length);
    _own_buffer = true;
  } else {
    _buffer = data;
  }

  unsigned char md5[EVP_MAX_MD_SIZE];
  if ( calculate_md5(data, length, md5) < 0 ){
    throw "Failed to calculate md5";
  }

  _meta.toi = toi;
  _meta.content_location = std::move(content_location);
  _meta.content_type = std::move(content_type);
  _meta.content_length = length;
  _meta.content_md5 = base64_encode(md5, MD5_DIGEST_LENGTH);
  _meta.expires = expires;
  _meta.fec_oti = fec_oti;

#ifdef RAPTOR_ENABLED
  RaptorFEC *r = nullptr;
#endif

  switch (_meta.fec_oti.encoding_id) {
    case FecScheme::CompactNoCode:
      _meta.fec_oti.transfer_length = length;
      _meta.fec_transformer = 0;
      break;
#ifdef RAPTOR_ENABLED
    case FecScheme::Raptor:
      _meta.fec_oti.transfer_length = length;
      r = new RaptorFEC(length, fec_oti.encoding_symbol_length);
      _meta.fec_oti.encoding_symbol_length = r->T;
      _meta.fec_oti.max_source_block_length = r->K * r->T;
      _meta.fec_transformer = r; 
      break;
#endif
    default:
      throw "FEC scheme not supported or not yet implemented";
      break;
  }

  calculate_partitioning();
  create_blocks();
}

LibFlute::File::~File()
{
  spdlog::debug("Destroying File");
  if (_own_buffer && _buffer != nullptr)
  {
    spdlog::debug("Freeing buffer");
    free(_buffer);
  }
}

auto LibFlute::File::put_symbol( const LibFlute::EncodingSymbol& symbol ) -> void
{
  if(_complete) {
    spdlog::debug("Not handling symbol {} , SBN {} since file is already complete",symbol.id(),symbol.source_block_number());
    return;
  }
  if (symbol.source_block_number() > _source_blocks.size()) {
    throw "Source Block number too high";
  } 

  SourceBlock& source_block = _source_blocks[ symbol.source_block_number() ];
  
  if(source_block.complete){
      spdlog::warn("Ignoring symbol {} since block {} is already complete",symbol.id(),symbol.source_block_number());
	  return;
  }

  if (symbol.id() > source_block.symbols.size()) {
    throw "Encoding Symbol ID too high";
  } 

  LibFlute::Symbol& target_symbol = source_block.symbols[symbol.id()];

  if (!target_symbol.complete) {
    symbol.decode_to(target_symbol.data, target_symbol.length);
    target_symbol.complete = true;
    if (_meta.fec_transformer) {
      _meta.fec_transformer->process_symbol(source_block,target_symbol,symbol.id());
    }
    check_source_block_completion(source_block);
    check_file_completion();
  }

}

auto LibFlute::File::check_source_block_completion( SourceBlock& block ) -> void
{
  if (_meta.fec_transformer) {
    block.complete = _meta.fec_transformer->check_source_block_completion(block);
    return;
  }
  block.complete = std::all_of(block.symbols.begin(), block.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
}

auto LibFlute::File::check_file_completion() -> void
{
  _complete = std::all_of(_source_blocks.begin(), _source_blocks.end(), [](const auto& block){ return block.second.complete; });

  if (_complete && !_meta.content_md5.empty()) {

      if(_meta.fec_transformer){
          _meta.fec_transformer->extract_file(_source_blocks);
      }

    //check MD5 sum
    unsigned char md5[EVP_MAX_MD_SIZE];
    calculate_md5(buffer(),length(),md5);

    auto content_md5 = base64_decode(_meta.content_md5);
    if (memcmp(md5, content_md5.c_str(), MD5_DIGEST_LENGTH) != 0) {
      spdlog::error("MD5 mismatch for TOI {}, discarding", _meta.toi);
 
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

auto LibFlute::File::calculate_partitioning() -> void
{
  if (_meta.fec_transformer && _meta.fec_transformer->calculate_partitioning()){
    _nof_source_symbols = _meta.fec_transformer->nof_source_symbols;
    _nof_source_blocks = _meta.fec_transformer->nof_source_blocks;
    _large_source_block_length = _meta.fec_transformer->large_source_block_length;
    _small_source_block_length = _meta.fec_transformer->small_source_block_length;
    _nof_large_source_blocks = _meta.fec_transformer->nof_large_source_blocks;
    return;
  }
  // Calculate source block partitioning (RFC5052 9.1) 
  _nof_source_symbols = ceil((double)_meta.fec_oti.transfer_length / (double)_meta.fec_oti.encoding_symbol_length);
  _nof_source_blocks = ceil((double)_nof_source_symbols / (double)_meta.fec_oti.max_source_block_length);
  _large_source_block_length = ceil((double)_nof_source_symbols / (double)_nof_source_blocks);
  _small_source_block_length = floor((double)_nof_source_symbols / (double)_nof_source_blocks);
  _nof_large_source_blocks = _nof_source_symbols - _small_source_block_length * _nof_source_blocks;
}

auto LibFlute::File::create_blocks() -> void
{
  // Create the required source blocks and encoding symbols

  if (_meta.fec_transformer){
    int bytes_read = 0;
    _source_blocks = _meta.fec_transformer->create_blocks(_buffer, &bytes_read);
    if (_source_blocks.size() <= 0) {
      spdlog::error("FEC Transformer failed to create source blocks");
      throw "FEC Transformer failed to create source blocks";
    }
    return;
  }

  auto buffer_ptr = _buffer;
  size_t remaining_size = _meta.fec_oti.transfer_length;
  auto number = 0;
  while (remaining_size > 0) {
    LibFlute::SourceBlock block;
    auto symbol_id = 0;
    auto block_length = ( number < _nof_large_source_blocks ) ? _large_source_block_length : _small_source_block_length;

    for (int i = 0; i < block_length; i++) {
      auto symbol_length = std::min(remaining_size, (size_t)_meta.fec_oti.encoding_symbol_length);
      assert(buffer_ptr + symbol_length <= _buffer + _meta.fec_oti.transfer_length);

      LibFlute::Symbol symbol{ .data = buffer_ptr, .length = symbol_length, .complete = false};
      block.symbols[ symbol_id++ ] = symbol;
      
      remaining_size -= symbol_length;
      buffer_ptr += symbol_length;
      
      if (remaining_size <= 0) break;
    }
    _source_blocks[number++] = block;
  }
}

auto LibFlute::File::get_next_symbols(size_t max_size) -> std::vector<EncodingSymbol> 
{
  auto block = _source_blocks.begin();
  int nof_symbols = std::floor((float)max_size / (float)_meta.fec_oti.encoding_symbol_length);
  auto cnt = 0;
  std::vector<EncodingSymbol> symbols;
  spdlog::debug("Attempting to queue {} symbols",nof_symbols);
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

auto LibFlute::File::mark_completed(const std::vector<EncodingSymbol>& symbols, bool success) -> void
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

  int LibFlute::calculate_md5(char *input, int length, unsigned char *result)
{
  // simple implementation based on openssl docs (https://www.openssl.org/docs/man3.0/man3/EVP_DigestInit_ex.html) 
  if (!input || ! length) {
    spdlog::error("MD5 called with invalid input");
    return -1;
  }
  spdlog::debug("MD5 calculation called for input length {}", length);

  EVP_MD_CTX*   context = EVP_MD_CTX_new();
  const EVP_MD* md = EVP_md5();
  unsigned int  md_len;

  EVP_DigestInit_ex(context, md, NULL);
  EVP_DigestUpdate(context, input, length);
  EVP_DigestFinal_ex(context, result, &md_len);
  EVP_MD_CTX_free(context);

  char buf [EVP_MAX_MD_SIZE * 2] = {};
  for (unsigned int i = 0 ; i < md_len ; ++i){
    sprintf(&buf[i*2], "%02x", result[i]);
  }
  spdlog::debug("MD5 Digest is {}", buf);

  return md_len;
}
