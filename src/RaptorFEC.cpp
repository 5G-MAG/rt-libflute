#include "flute_types.h"
#include "spdlog/spdlog.h"

#ifdef RAPTOR_ENABLED

LibFlute::RaptorFEC::RaptorFEC(){}


LibFlute::RaptorFEC::RaptorFEC(unsigned int transfer_length, unsigned int max_payload, unsigned long target_sub_block_size) 
    : F(transfer_length)
    , P(max_payload)
    , W(target_sub_block_size)
{
  Al = 4;
  calculate_partitioning();
}

LibFlute::RaptorFEC::~RaptorFEC() = default;

bool LibFlute::RaptorFEC::calculate_partitioning() {
  // TODO: print debug statements and test
  double G = fmin( fmin(ceil((double)P*1024/(double)F), (double)P/(double)Al), 10.0f);
  spdlog::debug("double G = fmin( fmin(ceil((double)P*1024/F), (double)P/(double)Al), 10.0f");
  spdlog::debug("G = {} = min( ceil({}*1024/{}), {}/{}, 10.0f)",G,P,F,P,Al);

  T = (unsigned int) floor((double)P/(double)(Al*G)) * Al;
  spdlog::debug("T = (unsigned int) floor((double)P/(double)(Al*G)) * Al");
  spdlog::debug("T = {} = floor({}/({}*{})) * {}",T,P,Al,G,Al);

  assert(T % Al == 0); // Symbol size T should be a multiple of symbol alignment parameter Al
  
  double Kt = ceil((double)F/(double)T);
  spdlog::debug("double Kt = ceil((double)F/(double)T)");
  spdlog::debug("Kt = {} = ceil({}/{})",Kt,F,T);

  Z = (unsigned int) ceil(Kt/8192);
  spdlog::debug("Z = (unsigned int) ceil(Kt/8192)");
  spdlog::debug("Z = {} = ceil({}/8192)",Z,Kt);


  N = fmin( ceil( ceil(Kt/(double)Z) * (double)T/(double)W ) , (double)T/(double)Al );
  spdlog::warn("N = fmin( ceil( ceil(Kt/(double)Z) * (double)T/(double)W ) , (double)T/(double)Al )");
  spdlog::warn("N = {} = min( ceil( ceil({}/{}) * {}/{} ) , {}/{} )",N,Kt,Z,T,W,T,Al);
  
  //TODO set the values that the File class needs...

  return true;
}

bool LibFlute::RaptorFEC::check_source_block_completion(LibFlute::SourceBlock& srcblk) {
  // TODO: try to decode srcblk using the symbols it contains...
  return true;
}

unsigned int LibFlute::RaptorFEC::target_K() { return K * surplus_packet_ratio; }

LibFlute::Symbol LibFlute::RaptorFEC::translate_symbol(struct enc_context *encoder_ctx) {
    struct Symbol symbol { new char[T], T };
    struct LT_packet *lt_packet = encode_LT_packet(encoder_ctx);

    memcpy(symbol.data, &lt_packet->syms, T);

    free_LT_packet(lt_packet);
    return symbol;
}

LibFlute::SourceBlock LibFlute::RaptorFEC::create_block(unsigned char *buffer, int *bytes_read) {
    struct SourceBlock source_block;
    struct enc_context *encoder_ctx = create_encoder_context(buffer, K, T);
    unsigned int symbols_to_read = target_K();

    for(unsigned int symbol_id = 1; symbol_id < symbols_to_read; symbol_id++) {
        source_block.symbols[symbol_id] = translate_symbol(encoder_ctx);
        *bytes_read += T;
    }

    free_encoder_context(encoder_ctx);
    return source_block;
}


// TODO: Reformat to K and T
std::map<uint16_t, LibFlute::SourceBlock> LibFlute::RaptorFEC::create_blocks(unsigned char *buffer, int *bytes_read) {
    if(!bytes_read)
        throw std::invalid_argument("bytes_read pointer shouldn't be null");
    if(N != 1)
      throw std::invalid_argument("Currently the encoding only supports 1 sub-block per block");
  std::map<uint16_t, LibFlute::SourceBlock> block_map;

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

  //TODO: calculate other relevant values from these

  return true;
}

bool LibFlute::RaptorFEC::add_fdt_info(tinyxml2::XMLElement *file) {
  //TODO: do we need transfer length too? Does it change based on FecScheme?
  file->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned) FecScheme::Raptor);
  file->SetAttribute("FEC-OTI-Encoding-Symbol-Length", T);
  file->SetAttribute("FEC-OTI-Symbol-Alignment-Parameter", Al);
  file->SetAttribute("FEC-OTI-Number-Of-Source-Blocks", Z);
  file->SetAttribute("FEC-OTI-Number-Of-Sub-Blocks", N);
  file->SetAttribute("FEC-OTI-Symbol-Alignment-Parameter", Al);

  return true;
}

#endif