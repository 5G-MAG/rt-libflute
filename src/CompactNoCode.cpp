#include "flute_types.h"
#include "spdlog/spdlog.h"

LibFlute::CompactNoCodeFEC::CompactNoCodeFEC() {
//   this->encoding_id = LibFlute::FecScheme::CompactNoCode;
}

LibFlute::CompactNoCodeFEC::CompactNoCodeFEC( uint64_t transfer_length, uint32_t encoding_symbol_length, uint32_t max_source_block_length)
{
//   this->encoding_symbol_length = encoding_symbol_length;
//   this->transfer_length = transfer_length;
//   this->max_source_block_length = max_source_block_length;
//   this->encoding_id = LibFlute::FecScheme::CompactNoCode;
}

LibFlute::CompactNoCodeFEC::~CompactNoCodeFEC() = default;

bool LibFlute::CompactNoCodeFEC::check_source_block_completion(LibFlute::SourceBlock& srcblk){
  return true;
}

std::map<uint16_t, LibFlute::SourceBlock> LibFlute::CompactNoCodeFEC::create_blocks(char *buffer, int *bytes_read) {
  std::map<uint16_t, LibFlute::SourceBlock> m;
  return m;
}

void LibFlute::CompactNoCodeFEC::calculate_partioning(){

}