#include "RaptorFEC.h"

LibFlute::RaptorFEC::RaptorFEC(unsigned int transfer_length, unsigned int max_payload) 
    : F(transfer_length)
    , P(max_payload)
{
  double g = fmin( fmin(ceil((double)P*1024/(double)F), (double)P/(double)Al), 10.0f);
  spdlog::debug("double g = fmin( fmin(ceil((double)P*1024/F), (double)P/(double)Al), 10.0f");
  spdlog::debug("G = {} = min( ceil({}*1024/{}), {}/{}, 10.0f)",g,P,F,P,Al);
  G = (unsigned int) g;

  T = (unsigned int) floor((double)P/(double)(Al*g)) * Al;
  spdlog::debug("T = (unsigned int) floor((double)P/(double)(Al*g)) * Al");
  spdlog::debug("T = {} = floor({}/({}*{})) * {}",T,P,Al,g,Al);

  if (T % Al){
    spdlog::error(" Symbol size T should be a multiple of symbol alignment parameter Al");
    throw "Symbol size doesnt align";
  }
  
  Kt = ceil((double)F/(double)T); // total symbols
  spdlog::debug("double Kt = ceil((double)F/(double)T)");
  spdlog::debug("Kt = {} = ceil({}/{})",Kt,F,T);

  if (Kt < 4){
    spdlog::error("Input file is too small, it must be a minimum of 4 Symbols");
    throw "Input is less than 4 symbols";
  }

  Z = (unsigned int) ceil((double)Kt/(double)8192);
  spdlog::debug("Z = (unsigned int) ceil(Kt/8192)");
  spdlog::debug("Z = {} = ceil({}/8192)",Z,Kt);

  K = (Kt > 8192) ? 8192 : (unsigned int) Kt; // symbols per source block
  spdlog::debug("K = {}",K);

  N = fmin( ceil( ceil((double)Kt/(double)Z) * (double)T/(double)W ) , (double)T/(double)Al );
  spdlog::debug("N = fmin( ceil( ceil(Kt/(double)Z) * (double)T/(double)W ) , (double)T/(double)Al )");
  spdlog::debug("N = {} = min( ceil( ceil({}/{}) * {}/{} ) , {}/{} )",N,Kt,Z,T,W,T,Al);

  
  // Set the values that the File class may need:
  nof_source_symbols = (unsigned int) Kt;
  nof_source_blocks = Z;

  small_source_block_length = (Z * K - nof_source_symbols) * T; // = (number of symbols in the final (small) source block, if nof_source_symbols isnt cleanly divisible by Z * K ) * symbol size

  // open question as to how we define "large source blocks" because either none of the remaining "regular" blocks are large, or all of them are, since raptor has a fixed block size

  /*
  nof_large_source_blocks = K - (small_source_block_length != 0); // if we define a "large" source block as a normal one then its just the nof "regular" source blocks minus the nof small ones (which is either one or zero)
  large_source_block_length = K * T;
  */

  nof_large_source_blocks = 0; //for now argue that there are no "large" blocks, only regular and small ones
  large_source_block_length = 0;
}

LibFlute::RaptorFEC::~RaptorFEC() {
  for(auto iter = decoders.begin(); iter != decoders.end(); iter++){
    free_decoder_context(iter->second);
  }
}

bool LibFlute::RaptorFEC::calculate_partitioning() {
  return true;
}

void LibFlute::RaptorFEC::extract_finished_block(LibFlute::SourceBlock& srcblk, struct dec_context *dc) {
    if(!dc){
        return;
    }
    for(auto iter = srcblk.symbols.begin(); iter != srcblk.symbols.end(); iter++) {
        memcpy(iter->second.data,dc->pp[iter->first],T); // overwrite the encoded symbol with the source data;
    }
   spdlog::debug("Raptor Decoder: finished decoding source block {}",srcblk.id);
}

void *LibFlute::RaptorFEC::allocate_file_buffer(int min_length){
  assert(min_length <= Z*target_K(0)*T); // min length should be exactly Z*K*T, so including repair symbols we should have a larger value
  return malloc(Z*target_K(0)*T);
}

bool LibFlute::RaptorFEC::process_symbol(LibFlute::SourceBlock& srcblk, LibFlute::Symbol& symbol, unsigned int id) {
  assert(symbol.length == T); // symbol.length should always be the symbol size, T
  struct dec_context *dc = decoders[srcblk.id];
  if (!dc) {
    int nsymbs = (srcblk.id < Z - 1) ? K : Kt - K*(Z-1); // the last block will usually be smaller than the normal block size, unless the file size is an exact multiple
    int blocksize = (srcblk.id < Z - 1) ? K*T : F - K*T*(Z-1);
    spdlog::debug("Preparing decoder context with {} blocks and blocksize {}", nsymbs, blocksize);
    struct enc_context *sc = create_encoder_context(NULL, nsymbs, T, blocksize, srcblk.id);
    //struct enc_context *sc = create_encoder_context(NULL, K, T, K*T, srcblk.id); // the "length" will always be K*T from the decoders perspective
    dc = create_decoder_context(sc);
    decoders[srcblk.id] = dc;
  }
  if (dc->finished){
    spdlog::warn("Skipped processing of symbol for finished block : SBN {}, ESI {}",srcblk.id,id);
    return true;
  }
  struct LT_packet * pkt = (struct LT_packet *) calloc(1, sizeof(struct LT_packet));
  pkt->id = id;
  pkt->syms = (GF_ELEMENT *) malloc(symbol.length * sizeof(char));
  memcpy(pkt->syms, symbol.data, symbol.length * sizeof(char));

  process_LT_packet(dc, pkt);
  free_LT_packet(pkt);
  return true;
}

bool LibFlute::RaptorFEC::extract_file(std::map<uint16_t, SourceBlock> blocks) {
    for(auto iter = blocks.begin(); iter != blocks.end(); iter++) {
     extract_finished_block(iter->second,decoders[iter->second.id]);
    }
    return true;
}

bool LibFlute::RaptorFEC::check_source_block_completion(LibFlute::SourceBlock& srcblk) {
  if (is_encoder) {
    // check source block completion for the Encoder
    bool complete = std::all_of(srcblk.symbols.begin(), srcblk.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });

    if(complete)
        std::for_each(srcblk.symbols.begin(), srcblk.symbols.end(), [](const auto& symbol){ delete[] symbol.second.data; });

    return complete;
  }
  // else case- we are the Decoder

  if(!srcblk.symbols.size()){
    spdlog::warn("Empty source block (size 0) SBN {}",srcblk.id);
    return false;
  }

  struct dec_context *dc = decoders[srcblk.id];
  if (!dc) {
    spdlog::error("Couldnt find raptor decoder for source block {}",srcblk.id);
    return false;
  }
  return dc->finished;
}

unsigned int LibFlute::RaptorFEC::target_K(int blockno) {
    // always send at least one repair symbol
  if (blockno < Z-1) {
      int target = K * surplus_packet_ratio;
      return (target > K) ? target : K + 1;
  }
  // last block gets special treatment
  int remaining_symbs = Kt - K*(Z-1);
  return (remaining_symbs + 1 > remaining_symbs*surplus_packet_ratio) ? remaining_symbs + 1 : remaining_symbs * surplus_packet_ratio;
}

LibFlute::Symbol LibFlute::RaptorFEC::translate_symbol(struct enc_context *encoder_ctx){
    struct LT_packet *lt_packet = encode_LT_packet(encoder_ctx);
    struct Symbol symbol { new char[T], T};

    memcpy(symbol.data, lt_packet->syms, T);

    free_LT_packet(lt_packet);
    return symbol;
}

LibFlute::SourceBlock LibFlute::RaptorFEC::create_block(char *buffer, int *bytes_read, int blockid) {
    struct SourceBlock source_block;
    source_block.id = blockid;
    int seed = blockid;
    int nsymbs = (blockid < Z - 1) ? K : Kt - K*(Z-1);
    int blocksize = (blockid < Z - 1) ? K*T : F - K*T*(Z-1); // the last block will usually be smaller than the normal block size, unless the file size is an exact multiple
    struct enc_context *encoder_ctx = create_encoder_context((unsigned char *)buffer, nsymbs , T, blocksize, seed);
//    struct enc_context *encoder_ctx = create_encoder_context((unsigned char *)buffer, K, T, blocksize, seed);
    if (!encoder_ctx) {
        spdlog::error("Error creating encoder context");
        throw "Error creating encoder context";
    }
    unsigned int symbols_to_read = target_K(blockid);
    for(unsigned int symbol_id = 0; symbol_id < symbols_to_read; symbol_id++) {
        source_block.symbols[symbol_id] = translate_symbol(encoder_ctx);
    }
    *bytes_read += blocksize;
    spdlog::debug("Creating encoder context with {} blocks and blocksize {}", nsymbs, blocksize);

    free_encoder_context(encoder_ctx);
    return source_block;
}


std::map<uint16_t, LibFlute::SourceBlock> LibFlute::RaptorFEC::create_blocks(char *buffer, int *bytes_read) {
  if(!bytes_read)
      throw std::invalid_argument("bytes_read pointer shouldn't be null");
  if(N != 1)
    throw std::invalid_argument("Currently the encoding only supports 1 sub-block per block");

  std::map<uint16_t, LibFlute::SourceBlock> block_map;
  *bytes_read = 0;

  for(unsigned int src_blocks = 0; src_blocks < Z; src_blocks++) {
    if(!is_encoder) {
      LibFlute::SourceBlock block;
      unsigned int symbols_to_read = target_K(src_blocks);
      for (int i = 0; i < symbols_to_read; i++) {
        block.symbols[i] = Symbol {.data = buffer + src_blocks*K*T + T*i, .length = T, .complete = false};
      }
      block.id = src_blocks;
      block_map[src_blocks] = block;
    } else {
      block_map[src_blocks] = create_block(&buffer[*bytes_read], bytes_read, src_blocks);
    }
  }
  return block_map;
}


bool LibFlute::RaptorFEC::parse_fdt_info(tinyxml2::XMLElement *file) {
  is_encoder = false;

  const char* val = 0;
  val = file->Attribute("Transfer-Length");
  if (val != nullptr) {
    F = strtoul(val, nullptr, 0);
  } else {
    throw "Required field \"Transfer-Length\" is missing for an object in the FDT";
  }
  
  val = file->Attribute("FEC-OTI-Number-Of-Source-Blocks");
  if (val != nullptr) {
    Z = strtoul(val, nullptr, 0);
  } else {
    throw "Required field \"FEC-OTI-Number-Of-Source-Blocks\" is missing for an object in the FDT";
  }
  
  val = file->Attribute("FEC-OTI-Number-Of-Sub-Blocks");
  if (val != nullptr) {
    N = strtoul(val, nullptr, 0);
  } else {
    throw "Required field \"FEC-OTI-Number-Of-Sub-Blocks\" is missing for an object in the FDT";
  }

  val = file->Attribute("FEC-OTI-Encoding-Symbol-Length");
  if (val != nullptr) {
    T = strtoul(val, nullptr, 0);
  } else {
    throw "Required field \"FEC-OTI-Encoding-Symbol-Length\" is missing for an object in the FDT";
  }
  
  val = file->Attribute("FEC-OTI-Symbol-Alignment-Parameter");
  if (val != nullptr) {
    Al = strtoul(val, nullptr, 0);
  } else {
    throw "Required field \"FEC-OTI-Symbol-Alignment-Parameter\" is missing for an object in the FDT";
  }

  if (T % Al) {
    throw "Symbol size T is not a multiple of Al. Invalid configuration from sender";
  }

  // Set the values that are missing that we or the File class may need, follows the same logic as in calculate_partitioning()
  nof_source_symbols = ceil((double)F / (double)T);
  K = (nof_source_symbols > 8192) ? 8192 : nof_source_symbols;
  Kt = ceil((double)F/(double)T); // total symbols

  nof_source_blocks = Z;
  small_source_block_length = (Z * K - nof_source_symbols) * T;
  nof_large_source_blocks = 0;
  large_source_block_length = 0;

  return true;
}

bool LibFlute::RaptorFEC::add_fdt_info(tinyxml2::XMLElement *file) {
  //TODO: do we need to set transfer length too? I already gets set earlier. Does it change based on FecScheme?
  file->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned) FecScheme::Raptor);
  file->SetAttribute("FEC-OTI-Encoding-Symbol-Length", T);
  file->SetAttribute("FEC-OTI-Symbol-Alignment-Parameter", Al);
  file->SetAttribute("FEC-OTI-Number-Of-Source-Blocks", Z);
  file->SetAttribute("FEC-OTI-Number-Of-Sub-Blocks", N);
  file->SetAttribute("FEC-OTI-Symbol-Alignment-Parameter", Al);

  is_encoder = true;

  return true;
}
