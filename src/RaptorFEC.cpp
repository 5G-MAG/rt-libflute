
#include "RaptorFEC.h"
#include "flute_types.h"
#include "spdlog/spdlog.h"

void test_raptor()
{
#ifdef RAPTOR_ENABLED
  printf("Testing gf2matrix\n");

  // Create a Raptor10 object and fill it w/ all known needed params
  Raptor10 coder;

  coder.K = 	 22;
  coder.Kmin = 1024;
  coder.Kmax = 8192;
  coder.Gmax =   10;
  coder.Al =      4;
  coder.N =      24;
  coder.T =       4;
  r10_compute_params(&coder);
  printf("K=%u, S=%u, H=%u, L=%u\n", coder.K, coder.S, coder.H, coder.L);

  // Allocate and calculate the constraints matrix
  gf2matrix A;
  allocate_gf2matrix(&A, coder.L, coder.L);
  r10_build_constraints_mat(&coder, &A);

  // LT encode
  uint8_t enc_s[coder.L * coder.T];
  uint8_t src_s[coder.K * coder.T];
  r10_encode(src_s, enc_s, &coder, &A);

  // Now, enc_s should contain the encoded symbols
  // Still, doesn't allow to decide the size of the symbols

  printf("Constraints matrix:\n");
  print_matrix(&A);

  return;
#endif
}


 void LibFlute::RaptorFEC::void calculate_partioning() {
  // TODO
  return;
 }


 bool LibFlute::RaptorFEC::check_source_block_completion(SourceBlock& srcblk) {
  // TODO: try to decode srcblk using the symbols it contains...
 }

 std::map<uint16_t, SourceBlock> LibFlute::RaptorFEC::create_blocks(char *buffer, int *bytes_read) {
  // TODO: encode buffer into a number of symbols
 }