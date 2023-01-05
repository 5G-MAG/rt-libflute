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

  assert(T % Al == 0); // Symbol size T should be a multiple of symbol alignment parameter Al
  
  double Kt = ceil((double)F/(double)T); // total symbols
  spdlog::debug("double Kt = ceil((double)F/(double)T)");
  spdlog::debug("Kt = {} = ceil({}/{})",Kt,F,T);

  Z = (unsigned int) ceil(Kt/8192);
  spdlog::debug("Z = (unsigned int) ceil(Kt/8192)");
  spdlog::debug("Z = {} = ceil({}/8192)",Z,Kt);

  K = (Kt > 8192) ? 8192 : (unsigned int) Kt; // symbols per source block
  spdlog::debug("K = {}",K);

  N = fmin( ceil( ceil(Kt/(double)Z) * (double)T/(double)W ) , (double)T/(double)Al );
  spdlog::warn("N = fmin( ceil( ceil(Kt/(double)Z) * (double)T/(double)W ) , (double)T/(double)Al )");
  spdlog::warn("N = {} = min( ceil( ceil({}/{}) * {}/{} ) , {}/{} )",N,Kt,Z,T,W,T,Al);
  
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
  free_decoder_context(dc);  
  free_encoder_context(sc);
}

bool LibFlute::RaptorFEC::calculate_partitioning() {
  return true;
}

bool LibFlute::RaptorFEC::check_source_block_completion(LibFlute::SourceBlock& srcblk) {
  if (!dc || !transformers.size()) {
    // check source block completion for the ENcoder
    return std::all_of(srcblk.symbols.begin(), srcblk.symbols.end(), [](const auto& symbol){ return symbol.second.complete; });
  }
  // TODO: try to decode srcblk using the symbols it contains...
  // sc needs to have: snum, psize, cnum, graph
  // graph for precoding is problematic: generated based on some randomness
  //   solution 1: transfer some (ideally slim) representation of graph from sndr to rcvr
  //   solution 2: transfer random seed from sndr to rcvr reproduce randomness and create exact graph
  if(!sc)
  {
	// TODO: create slim version of encoder_context: only snum, psize, cnum and graph
	sc = create_encoder_context(NULL, Z, T); 
  }
  if(!dc) dc = create_decoder_context(sc);
  // TODO: implement transform_srcblk_to_lt (LibFlute::SourcBlock -> LT_packet)
  struct LT_packet * pkt; // = transform_srcblk_to_lt(srcblk);
  process_LT_packet(dc, pkt);
  free_LT_packet(pkt);
  return dc->finished;
}

unsigned int LibFlute::RaptorFEC::target_K() { return K * surplus_packet_ratio; }

LibFlute::Symbol LibFlute::RaptorFEC::translate_symbol(struct enc_context *encoder_ctx) {
    // TODO: Delete in the File destructor (or anywhere where applicable)
    struct Symbol symbol { new char[T], T };
    struct LT_packet *lt_packet = encode_LT_packet(encoder_ctx);

    memcpy(symbol.data, &lt_packet->syms, T);

    free_LT_packet(lt_packet);
    return symbol;
}

LibFlute::SourceBlock LibFlute::RaptorFEC::create_block(char *buffer, int *bytes_read) {
    struct SourceBlock source_block;
    struct enc_context *encoder_ctx = create_encoder_context((unsigned char *)buffer, K, T);
    *bytes_read += K * T;
    unsigned int symbols_to_read = target_K();

    for(unsigned int symbol_id = 1; symbol_id < symbols_to_read + 1; symbol_id++) {
        source_block.symbols[symbol_id] = translate_symbol(encoder_ctx);
    }

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

  for(unsigned int src_blocks = 1; src_blocks < Z + 1; src_blocks++)
      block_map[src_blocks] = create_block(&buffer[*bytes_read], bytes_read);

  return block_map;
}


bool LibFlute::RaptorFEC::parse_fdt_info(tinyxml2::XMLElement *file) {
  // TODO

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

  return true;
}