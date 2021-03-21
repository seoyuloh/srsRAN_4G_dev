/**
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srslte/phy/phch/uci_nr.h"
#include "srslte/phy/fec/block/block.h"
#include "srslte/phy/fec/polar/polar_chanalloc.h"
#include "srslte/phy/phch/csi.h"
#include "srslte/phy/phch/uci_cfg.h"
#include "srslte/phy/utils/bit.h"
#include "srslte/phy/utils/vector.h"

#define UCI_NR_INFO_TX(...) INFO("UCI-NR Tx: " __VA_ARGS__)
#define UCI_NR_INFO_RX(...) INFO("UCI-NR Rx: " __VA_ARGS__)

#define UCI_NR_MAX_L 11U
#define UCI_NR_POLAR_MAX 2048U
#define UCI_NR_POLAR_RM_IBIL 1
#define UCI_NR_PUCCH_POLAR_N_MAX 10
#define UCI_NR_BLOCK_DEFAULT_CORR_THRESHOLD 0.5f
#define UCI_NR_ONE_BIT_CORR_THRESHOLD 0.5f

uint32_t srslte_uci_nr_crc_len(uint32_t A)
{
  return (A <= 11) ? 0 : (A < 20) ? 6 : 11;
}

int srslte_uci_nr_init(srslte_uci_nr_t* q, const srslte_uci_nr_args_t* args)
{
  if (q == NULL || args == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  srslte_polar_encoder_type_t polar_encoder_type = SRSLTE_POLAR_ENCODER_PIPELINED;
  srslte_polar_decoder_type_t polar_decoder_type = SRSLTE_POLAR_DECODER_SSC_C;
#ifdef LV_HAVE_AVX2
  if (!args->disable_simd) {
    polar_encoder_type = SRSLTE_POLAR_ENCODER_AVX2;
    polar_decoder_type = SRSLTE_POLAR_DECODER_SSC_C_AVX2;
  }
#endif // LV_HAVE_AVX2

  if (srslte_polar_code_init(&q->code)) {
    ERROR("Initialising polar code");
    return SRSLTE_ERROR;
  }

  if (srslte_polar_encoder_init(&q->encoder, polar_encoder_type, NMAX_LOG) < SRSLTE_SUCCESS) {
    ERROR("Initialising polar encoder");
    return SRSLTE_ERROR;
  }

  if (srslte_polar_decoder_init(&q->decoder, polar_decoder_type, NMAX_LOG) < SRSLTE_SUCCESS) {
    ERROR("Initialising polar encoder");
    return SRSLTE_ERROR;
  }

  if (srslte_polar_rm_tx_init(&q->rm_tx) < SRSLTE_SUCCESS) {
    ERROR("Initialising polar RM");
    return SRSLTE_ERROR;
  }

  if (srslte_polar_rm_rx_init_c(&q->rm_rx) < SRSLTE_SUCCESS) {
    ERROR("Initialising polar RM");
    return SRSLTE_ERROR;
  }

  if (srslte_crc_init(&q->crc6, SRSLTE_LTE_CRC6, 6) < SRSLTE_SUCCESS) {
    ERROR("Initialising CRC");
    return SRSLTE_ERROR;
  }

  if (srslte_crc_init(&q->crc11, SRSLTE_LTE_CRC11, 11) < SRSLTE_SUCCESS) {
    ERROR("Initialising CRC");
    return SRSLTE_ERROR;
  }

  // Allocate bit sequence with space for the CRC
  q->bit_sequence = srslte_vec_u8_malloc(SRSLTE_UCI_NR_MAX_NOF_BITS);
  if (q->bit_sequence == NULL) {
    ERROR("Error malloc");
    return SRSLTE_ERROR;
  }

  // Allocate c with space for a and the CRC
  q->c = srslte_vec_u8_malloc(SRSLTE_UCI_NR_MAX_NOF_BITS + UCI_NR_MAX_L);
  if (q->c == NULL) {
    ERROR("Error malloc");
    return SRSLTE_ERROR;
  }

  q->allocated = srslte_vec_u8_malloc(UCI_NR_POLAR_MAX);
  if (q->allocated == NULL) {
    ERROR("Error malloc");
    return SRSLTE_ERROR;
  }

  q->d = srslte_vec_u8_malloc(UCI_NR_POLAR_MAX);
  if (q->d == NULL) {
    ERROR("Error malloc");
    return SRSLTE_ERROR;
  }

  if (isnormal(args->block_code_threshold)) {
    q->block_code_threshold = args->block_code_threshold;
  } else {
    q->block_code_threshold = UCI_NR_BLOCK_DEFAULT_CORR_THRESHOLD;
  }
  if (isnormal(args->one_bit_threshold)) {
    q->one_bit_threshold = args->one_bit_threshold;
  } else {
    q->one_bit_threshold = UCI_NR_ONE_BIT_CORR_THRESHOLD;
  }

  return SRSLTE_SUCCESS;
}

void srslte_uci_nr_free(srslte_uci_nr_t* q)
{
  if (q == NULL) {
    return;
  }

  srslte_polar_code_free(&q->code);
  srslte_polar_encoder_free(&q->encoder);
  srslte_polar_decoder_free(&q->decoder);
  srslte_polar_rm_tx_free(&q->rm_tx);
  srslte_polar_rm_rx_free_c(&q->rm_rx);

  if (q->bit_sequence != NULL) {
    free(q->bit_sequence);
  }
  if (q->c != NULL) {
    free(q->c);
  }
  if (q->allocated != NULL) {
    free(q->allocated);
  }
  if (q->d != NULL) {
    free(q->d);
  }

  SRSLTE_MEM_ZERO(q, srslte_uci_nr_t, 1);
}

static int uci_nr_pack_ack_sr(const srslte_uci_cfg_nr_t* cfg, const srslte_uci_value_nr_t* value, uint8_t* sequence)
{
  int A = 0;

  // Append ACK bits
  srslte_vec_u8_copy(&sequence[A], value->ack, cfg->o_ack);
  A += cfg->o_ack;

  // Append SR bits
  uint8_t* bits = &sequence[A];
  srslte_bit_unpack(value->sr, &bits, cfg->o_sr);
  A += cfg->o_sr;

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_TX("Packed UCI bits: ");
    srslte_vec_fprint_byte(stdout, sequence, A);
  }

  return A;
}

static int uci_nr_unpack_ack_sr(const srslte_uci_cfg_nr_t* cfg, uint8_t* sequence, srslte_uci_value_nr_t* value)
{
  int A = 0;

  // Append ACK bits
  srslte_vec_u8_copy(value->ack, &sequence[A], cfg->o_ack);
  A += cfg->o_ack;

  // Append SR bits
  uint8_t* bits = &sequence[A];
  value->sr     = srslte_bit_pack(&bits, cfg->o_sr);
  A += cfg->o_sr;

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_RX("Unpacked UCI bits: ");
    srslte_vec_fprint_byte(stdout, sequence, A);
  }

  return A;
}

static int uci_nr_pack_ack_sr_csi(const srslte_uci_cfg_nr_t* cfg, const srslte_uci_value_nr_t* value, uint8_t* sequence)
{
  int A = 0;

  // Append ACK bits
  srslte_vec_u8_copy(&sequence[A], value->ack, cfg->o_ack);
  A += cfg->o_ack;

  // Append SR bits
  uint8_t* bits = &sequence[A];
  srslte_bit_unpack(value->sr, &bits, cfg->o_sr);
  A += cfg->o_sr;

  // Append CSI bits
  int n = srslte_csi_part1_pack(cfg->csi, value->csi, cfg->nof_csi, bits, SRSLTE_UCI_NR_MAX_NOF_BITS - A);
  if (n < SRSLTE_SUCCESS) {
    ERROR("Packing CSI part 1");
    return SRSLTE_ERROR;
  }
  A += n;

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_TX("Packed UCI bits: ");
    srslte_vec_fprint_byte(stdout, sequence, A);
  }

  return A;
}

static int uci_nr_unpack_ack_sr_csi(const srslte_uci_cfg_nr_t* cfg, uint8_t* sequence, srslte_uci_value_nr_t* value)
{
  int A = 0;

  // Append ACK bits
  srslte_vec_u8_copy(value->ack, &sequence[A], cfg->o_ack);
  A += cfg->o_ack;

  // Append SR bits
  uint8_t* bits = &sequence[A];
  value->sr     = srslte_bit_pack(&bits, cfg->o_sr);
  A += cfg->o_sr;

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_RX("Unpacked UCI bits: ");
    srslte_vec_fprint_byte(stdout, sequence, A);
  }

  // Append CSI bits
  int n = srslte_csi_part1_unpack(cfg->csi, cfg->nof_csi, bits, SRSLTE_UCI_NR_MAX_NOF_BITS - A, value->csi);
  if (n < SRSLTE_SUCCESS) {
    ERROR("Packing CSI part 1");
    return SRSLTE_ERROR;
  }

  return A;
}

static int uci_nr_A(const srslte_uci_cfg_nr_t* cfg)
{
  int o_csi = srslte_csi_part1_nof_bits(cfg->csi, cfg->nof_csi);

  // 6.3.1.1.1 HARQ-ACK/SR only UCI bit sequence generation
  if (o_csi == 0) {
    return cfg->o_ack + cfg->o_sr;
  }

  // 6.3.1.1.2 CSI only
  if (cfg->o_ack == 0 && cfg->o_sr == 0) {
    return o_csi;
  }

  // 6.3.1.1.3 HARQ-ACK/SR and CSI
  ERROR("HARQ-ACK/SR and CSI encoding are not implemented");
  return SRSLTE_ERROR;
}

static int uci_nr_pack_pucch(const srslte_uci_cfg_nr_t* cfg, const srslte_uci_value_nr_t* value, uint8_t* sequence)
{
  int o_csi = srslte_csi_part1_nof_bits(cfg->csi, cfg->nof_csi);

  // 6.3.1.1.1 HARQ-ACK/SR only UCI bit sequence generation
  if (o_csi == 0) {
    return uci_nr_pack_ack_sr(cfg, value, sequence);
  }

  // 6.3.1.1.2 CSI only
  if (cfg->o_ack == 0 && cfg->o_sr == 0) {
    return srslte_csi_part1_pack(cfg->csi, value->csi, cfg->nof_csi, sequence, SRSLTE_UCI_NR_MAX_NOF_BITS);
  }

  // 6.3.1.1.3 HARQ-ACK/SR and CSI
  return uci_nr_pack_ack_sr_csi(cfg, value, sequence);
}

static int uci_nr_unpack_pucch(const srslte_uci_cfg_nr_t* cfg, uint8_t* sequence, srslte_uci_value_nr_t* value)
{
  int o_csi = srslte_csi_part1_nof_bits(cfg->csi, cfg->nof_csi);

  // 6.3.1.1.1 HARQ-ACK/SR only UCI bit sequence generation
  if (o_csi == 0) {
    return uci_nr_unpack_ack_sr(cfg, sequence, value);
  }

  // 6.3.1.1.2 CSI only
  if (cfg->o_ack == 0 && cfg->o_sr == 0) {
    ERROR("CSI only are not implemented");
    return SRSLTE_ERROR;
  }

  // 6.3.1.1.3 HARQ-ACK/SR and CSI
  return uci_nr_unpack_ack_sr_csi(cfg, sequence, value);
}

static int uci_nr_encode_1bit(srslte_uci_nr_t* q, const srslte_uci_cfg_nr_t* cfg, uint8_t* o, uint32_t E)
{
  uint32_t              i  = 0;
  srslte_uci_bit_type_t c0 = (q->bit_sequence[0] == 0) ? UCI_BIT_0 : UCI_BIT_1;

  switch (cfg->pusch.modulation) {
    case SRSLTE_MOD_BPSK:
      while (i < E) {
        o[i++] = c0;
      }
      break;
    case SRSLTE_MOD_QPSK:
      while (i < E) {
        o[i++] = c0;
        o[i++] = (uint8_t)UCI_BIT_REPETITION;
      }
      break;
    case SRSLTE_MOD_16QAM:
      while (i < E) {
        o[i++] = c0;
        o[i++] = (uint8_t)UCI_BIT_REPETITION;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
      }
      break;
    case SRSLTE_MOD_64QAM:
      while (i < E) {
        while (i < E) {
          o[i++] = c0;
          o[i++] = (uint8_t)UCI_BIT_REPETITION;
          o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
          o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
          o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
          o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        }
      }
      break;
    case SRSLTE_MOD_256QAM:
      while (i < E) {
        o[i++] = c0;
        o[i++] = (uint8_t)UCI_BIT_REPETITION;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
      }
      break;
    case SRSLTE_MOD_NITEMS:
    default:
      ERROR("Invalid modulation");
      return SRSLTE_ERROR;
  }

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_TX("One bit encoded NR-UCI; o=");
    srslte_vec_fprint_b(stdout, o, E);
  }

  return E;
}

static int uci_nr_decode_1_bit(srslte_uci_nr_t*           q,
                               const srslte_uci_cfg_nr_t* cfg,
                               uint32_t                   A,
                               const int8_t*              llr,
                               uint32_t                   E,
                               bool*                      decoded_ok)
{
  uint32_t Qm = srslte_mod_bits_x_symbol(cfg->pusch.modulation);
  if (Qm == 0) {
    ERROR("Invalid modulation (%s)", srslte_mod_string(cfg->pusch.modulation));
    return SRSLTE_ERROR;
  }

  // Correlate LLR
  float corr = 0.0f;
  float pwr  = 0.0f;
  for (uint32_t i = 0; i < E; i += Qm) {
    float t = (float)llr[i];
    corr += t;
    pwr += t * t;
  }

  // Normalise correlation
  float norm_corr = Qm * corr / (E * sqrtf(pwr));

  // Take decoded decision with threshold
  *decoded_ok = (norm_corr > q->one_bit_threshold);

  // Save decoded bit
  q->bit_sequence[0] = (corr < 0) ? 0 : 1;

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_RX("One bit decoding NR-UCI llr=");
    srslte_vec_fprint_bs(stdout, llr, E);
    UCI_NR_INFO_RX("One bit decoding NR-UCI A=%d; E=%d; pwr=%f; corr=%f; norm=%f; thr=%f; %s",
                   A,
                   E,
                   pwr,
                   corr,
                   norm_corr,
                   q->block_code_threshold,
                   *decoded_ok ? "OK" : "KO");
  }

  return SRSLTE_SUCCESS;
}

static int uci_nr_encode_2bit(srslte_uci_nr_t* q, const srslte_uci_cfg_nr_t* cfg, uint8_t* o, uint32_t E)
{
  uint32_t i  = 0;
  uint8_t  c0 = (uint8_t)((q->bit_sequence[0] == 0) ? UCI_BIT_0 : UCI_BIT_1);
  uint8_t  c1 = (uint8_t)((q->bit_sequence[1] == 0) ? UCI_BIT_0 : UCI_BIT_1);
  uint8_t  c2 = (uint8_t)(((q->bit_sequence[0] ^ q->bit_sequence[1]) == 0) ? UCI_BIT_0 : UCI_BIT_1);

  switch (cfg->pusch.modulation) {
    case SRSLTE_MOD_BPSK:
    case SRSLTE_MOD_QPSK:
      while (i < E) {
        o[i++] = c0;
        o[i++] = c1;
        o[i++] = c2;
      }
      break;
    case SRSLTE_MOD_16QAM:
      while (i < E) {
        o[i++] = c0;
        o[i++] = c1;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = c2;
        o[i++] = c0;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = c1;
        o[i++] = c2;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
      }
      break;
    case SRSLTE_MOD_64QAM:
      while (i < E) {
        o[i++] = c0;
        o[i++] = c1;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = c2;
        o[i++] = c0;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = c1;
        o[i++] = c2;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
      }
      break;
    case SRSLTE_MOD_256QAM:

      while (i < E) {
        o[i++] = c0;
        o[i++] = c1;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = c2;
        o[i++] = c0;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = c1;
        o[i++] = c2;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
        o[i++] = (uint8_t)UCI_BIT_PLACEHOLDER;
      }
      break;
    case SRSLTE_MOD_NITEMS:
    default:
      ERROR("Invalid modulation");
      return SRSLTE_ERROR;
  }

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_TX("Two bit encoded NR-UCI; E=%d; o=", E);
    srslte_vec_fprint_b(stdout, o, E);
  }

  return E;
}

static int uci_nr_decode_2_bit(srslte_uci_nr_t*           q,
                               const srslte_uci_cfg_nr_t* cfg,
                               uint32_t                   A,
                               const int8_t*              llr,
                               uint32_t                   E,
                               bool*                      decoded_ok)
{
  uint32_t Qm = srslte_mod_bits_x_symbol(cfg->pusch.modulation);
  if (Qm == 0) {
    ERROR("Invalid modulation (%s)", srslte_mod_string(cfg->pusch.modulation));
    return SRSLTE_ERROR;
  }

  // Correlate LLR
  float corr[3] = {};
  if (Qm == 1) {
    for (uint32_t i = 0; i < E / Qm; i++) {
      corr[i % 3] = llr[i];
    }
  } else {
    for (uint32_t i = 0, j = 0; i < E; i += Qm) {
      corr[(j++) % 3] = llr[i + 0];
      corr[(j++) % 3] = llr[i + 1];
    }
  }

  // Take decoded decision
  bool c0 = corr[0] > 0.0f;
  bool c1 = corr[1] > 0.0f;
  bool c2 = corr[2] > 0.0f;

  // Check redundancy bit
  *decoded_ok = (c2 == (c0 ^ c1));

  // Save decoded bits
  q->bit_sequence[0] = c0 ? 1 : 0;
  q->bit_sequence[1] = c1 ? 1 : 0;

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_RX("Two bit decoding NR-UCI llr=");
    srslte_vec_fprint_bs(stdout, llr, E);
    UCI_NR_INFO_RX("Two bit decoding NR-UCI A=%d; E=%d; Qm=%d; c0=%d; c1=%d; c2=%d %s",
                   A,
                   E,
                   Qm,
                   c0,
                   c1,
                   c2,
                   *decoded_ok ? "OK" : "KO");
  }

  return SRSLTE_SUCCESS;
}

static int
uci_nr_encode_3_11_bit(srslte_uci_nr_t* q, const srslte_uci_cfg_nr_t* cfg, uint32_t A, uint8_t* o, uint32_t E)
{
  srslte_block_encode(q->bit_sequence, A, o, E);

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_TX("Block encoded UCI bits; o=");
    srslte_vec_fprint_b(stdout, o, E);
  }

  return E;
}

static int uci_nr_decode_3_11_bit(srslte_uci_nr_t*           q,
                                  const srslte_uci_cfg_nr_t* cfg,
                                  uint32_t                   A,
                                  const int8_t*              llr,
                                  uint32_t                   E,
                                  bool*                      decoded_ok)
{
  // Check E for avoiding zero division
  if (E < 1) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  if (A == 11 && E <= 16) {
    ERROR("NR-UCI Impossible to decode A=%d; E=%d", A, E);
    return SRSLTE_ERROR;
  }

  // Compute average LLR power
  float pwr = srslte_vec_avg_power_bf(llr, E);
  if (!isnormal(pwr)) {
    ERROR("Received all zeros");
    return SRSLTE_ERROR;
  }

  // Decode
  float corr = (float)srslte_block_decode_i8(llr, E, q->bit_sequence, A);

  // Normalise correlation
  float norm_corr = corr / (sqrtf(pwr) * E);

  // Take decoded decision with threshold
  *decoded_ok = (corr > q->block_code_threshold);

  if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
    UCI_NR_INFO_RX("Block decoding NR-UCI llr=");
    srslte_vec_fprint_bs(stdout, llr, E);
    UCI_NR_INFO_RX("Block decoding NR-UCI A=%d; E=%d; pwr=%f; corr=%f; norm=%f; thr=%f; %s",
                   A,
                   E,
                   pwr,
                   corr,
                   norm_corr,
                   q->block_code_threshold,
                   *decoded_ok ? "OK" : "KO");
  }

  return SRSLTE_SUCCESS;
}

static int
uci_nr_encode_11_1706_bit(srslte_uci_nr_t* q, const srslte_uci_cfg_nr_t* cfg, uint32_t A, uint8_t* o, uint32_t E_uci)
{
  // If ( A ≥ 360 and E ≥ 1088 ) or if A ≥ 1013 , I seg = 1 ; otherwise I seg = 0
  uint32_t I_seg = 0;
  if ((A >= 360 && E_uci >= 1088) || A >= 1013) {
    I_seg = 1;
  }

  // Select CRC
  uint32_t      L   = srslte_uci_nr_crc_len(A);
  srslte_crc_t* crc = (L == 6) ? &q->crc6 : &q->crc11;

  // Segmentation
  uint32_t C = 1;
  if (I_seg == 1) {
    C = 2;
  }
  uint32_t A_prime = SRSLTE_CEIL(A, C) * C;

  // Get polar code
  uint32_t K_r = A_prime / C + L;
  uint32_t E_r = E_uci / C;
  if (srslte_polar_code_get(&q->code, K_r, E_r, UCI_NR_PUCCH_POLAR_N_MAX) < SRSLTE_SUCCESS) {
    ERROR("Error computing Polar code");
    return SRSLTE_ERROR;
  }

  // Write codeword
  for (uint32_t r = 0, s = 0; r < C; r++) {
    uint32_t k = 0;

    // Prefix (A_prime - A) zeros for the first CB only
    if (r == 0) {
      for (uint32_t i = 0; i < (A_prime - A); i++) {
        q->c[k++] = 0;
      }
    }

    // Load codeword bits
    while (k < A_prime / C) {
      q->c[k++] = q->bit_sequence[s++];
    }

    // Attach CRC
    srslte_crc_attach(crc, q->c, A_prime / C);
    UCI_NR_INFO_TX("Attaching %d/%d CRC%d=%" PRIx64, r, C, L, srslte_crc_checksum_get(crc));

    if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
      UCI_NR_INFO_TX("Polar cb %d/%d c=", r, C);
      srslte_vec_fprint_byte(stdout, q->c, K_r);
    }

    // Allocate channel
    srslte_polar_chanalloc_tx(q->c, q->allocated, q->code.N, q->code.K, q->code.nPC, q->code.K_set, q->code.PC_set);

    if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
      UCI_NR_INFO_TX("Polar alloc %d/%d ", r, C);
      srslte_vec_fprint_byte(stdout, q->allocated, q->code.N);
    }

    // Encode bits
    if (srslte_polar_encoder_encode(&q->encoder, q->allocated, q->d, q->code.n) < SRSLTE_SUCCESS) {
      return SRSLTE_ERROR;
    }

    if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
      UCI_NR_INFO_TX("Polar encoded %d/%d ", r, C);
      srslte_vec_fprint_byte(stdout, q->d, q->code.N);
    }

    // Rate matching
    srslte_polar_rm_tx(&q->rm_tx, q->d, &o[E_r * r], q->code.n, E_r, K_r, UCI_NR_POLAR_RM_IBIL);

    if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
      UCI_NR_INFO_TX("Polar RM cw %d/%d ", r, C);
      srslte_vec_fprint_byte(stdout, &o[E_r * r], E_r);
    }
  }

  return E_uci;
}

static int uci_nr_decode_11_1706_bit(srslte_uci_nr_t*           q,
                                     const srslte_uci_cfg_nr_t* cfg,
                                     uint32_t                   A,
                                     int8_t*                    llr,
                                     uint32_t                   E_uci,
                                     bool*                      decoded_ok)
{
  *decoded_ok = true;

  // If ( A ≥ 360 and E ≥ 1088 ) or if A ≥ 1013 , I seg = 1 ; otherwise I seg = 0
  uint32_t I_seg = 0;
  if ((A >= 360 && E_uci >= 1088) || A >= 1013) {
    I_seg = 1;
  }

  // Select CRC
  uint32_t      L   = srslte_uci_nr_crc_len(A);
  srslte_crc_t* crc = (L == 6) ? &q->crc6 : &q->crc11;

  // Segmentation
  uint32_t C = 1;
  if (I_seg == 1) {
    C = 2;
  }
  uint32_t A_prime = SRSLTE_CEIL(A, C) * C;

  // Get polar code
  uint32_t K_r = A_prime / C + L;
  uint32_t E_r = E_uci / C;
  if (srslte_polar_code_get(&q->code, K_r, E_r, UCI_NR_PUCCH_POLAR_N_MAX) < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  // Negate all LLR
  for (uint32_t i = 0; i < E_r; i++) {
    llr[i] *= -1;
  }

  // Write codeword
  for (uint32_t r = 0, s = 0; r < C; r++) {
    uint32_t k = 0;

    if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
      UCI_NR_INFO_RX("Polar LLR %d/%d ", r, C);
      srslte_vec_fprint_bs(stdout, &llr[E_r * r], q->code.N);
    }

    // Undo rate matching
    int8_t* d = (int8_t*)q->d;
    srslte_polar_rm_rx_c(&q->rm_rx, &llr[E_r * r], d, E_r, q->code.n, K_r, UCI_NR_POLAR_RM_IBIL);

    // Decode bits
    if (srslte_polar_decoder_decode_c(&q->decoder, d, q->allocated, q->code.n, q->code.F_set, q->code.F_set_size) <
        SRSLTE_SUCCESS) {
      return SRSLTE_ERROR;
    }

    if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
      UCI_NR_INFO_RX("Polar alloc %d/%d ", r, C);
      srslte_vec_fprint_byte(stdout, q->allocated, q->code.N);
    }

    // Undo channel allocation
    srslte_polar_chanalloc_rx(q->allocated, q->c, q->code.K, q->code.nPC, q->code.K_set, q->code.PC_set);

    if (SRSLTE_DEBUG_ENABLED && srslte_verbose >= SRSLTE_VERBOSE_INFO && !handler_registered) {
      UCI_NR_INFO_RX("Polar cb %d/%d c=", r, C);
      srslte_vec_fprint_byte(stdout, q->c, K_r);
    }

    // Calculate checksum
    uint8_t* ptr       = &q->c[A_prime / C];
    uint32_t checksum1 = srslte_crc_checksum(crc, q->c, A_prime / C);
    uint32_t checksum2 = srslte_bit_pack(&ptr, L);
    (*decoded_ok)      = ((*decoded_ok) && (checksum1 == checksum2));
    UCI_NR_INFO_RX("Checking %d/%d CRC%d={%02x,%02x}", r, C, L, checksum1, checksum2);

    // Prefix (A_prime - A) zeros for the first CB only
    if (r == 0) {
      for (uint32_t i = 0; i < (A_prime - A); i++) {
        k++;
      }
    }

    // Load codeword bits
    while (k < A_prime / C) {
      q->bit_sequence[s++] = q->c[k++];
    }
  }

  return SRSLTE_SUCCESS;
}

static int uci_nr_encode(srslte_uci_nr_t* q, const srslte_uci_cfg_nr_t* uci_cfg, uint32_t A, uint8_t* o, uint32_t E_uci)
{
  // 5.3.3.1 Encoding of 1-bit information
  if (A == 1) {
    return uci_nr_encode_1bit(q, uci_cfg, o, E_uci);
  }

  // 5.3.3.2 Encoding of 2-bit information
  if (A == 2) {
    return uci_nr_encode_2bit(q, uci_cfg, o, E_uci);
  }

  // 5.3.3.3 Encoding of other small block lengths
  if (A <= SRSLTE_FEC_BLOCK_MAX_NOF_BITS) {
    return uci_nr_encode_3_11_bit(q, uci_cfg, A, o, E_uci);
  }

  // Encoding of other sizes up to 1906
  if (A < SRSLTE_UCI_NR_MAX_NOF_BITS) {
    return uci_nr_encode_11_1706_bit(q, uci_cfg, A, o, E_uci);
  }

  return SRSLTE_ERROR;
}

static int uci_nr_decode(srslte_uci_nr_t*           q,
                         const srslte_uci_cfg_nr_t* uci_cfg,
                         int8_t*                    llr,
                         uint32_t                   A,
                         uint32_t                   E_uci,
                         bool*                      valid)
{
  if (q == NULL || uci_cfg == NULL || valid == NULL || llr == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  // Decode LLR
  if (A == 1) {
    if (uci_nr_decode_1_bit(q, uci_cfg, A, llr, E_uci, valid) < SRSLTE_SUCCESS) {
      return SRSLTE_ERROR;
    }
  } else if (A == 2) {
    if (uci_nr_decode_2_bit(q, uci_cfg, A, llr, E_uci, valid) < SRSLTE_SUCCESS) {
      return SRSLTE_ERROR;
    }
  } else if (A <= 11) {
    if (uci_nr_decode_3_11_bit(q, uci_cfg, A, llr, E_uci, valid) < SRSLTE_SUCCESS) {
      return SRSLTE_ERROR;
    }
  } else if (A < SRSLTE_UCI_NR_MAX_NOF_BITS) {
    if (uci_nr_decode_11_1706_bit(q, uci_cfg, A, llr, E_uci, valid) < SRSLTE_SUCCESS) {
      return SRSLTE_ERROR;
    }
  } else {
    ERROR("Invalid number of bits (A=%d)", A);
  }

  return SRSLTE_SUCCESS;
}

int srslte_uci_nr_pucch_format_2_3_4_E(const srslte_pucch_nr_resource_t* resource)
{
  if (resource == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  switch (resource->format) {
    case SRSLTE_PUCCH_NR_FORMAT_2:
      return (int)(16 * resource->nof_symbols * resource->nof_prb);
    case SRSLTE_PUCCH_NR_FORMAT_3:
      if (!resource->enable_pi_bpsk) {
        return (int)(24 * resource->nof_symbols * resource->nof_prb);
      }
      return (int)(12 * resource->nof_symbols * resource->nof_prb);
    case SRSLTE_PUCCH_NR_FORMAT_4:
      if (resource->occ_lenth != 1 && resource->occ_lenth != 2) {
        ERROR("Invalid spreading factor (%d)", resource->occ_lenth);
        return SRSLTE_ERROR;
      }
      if (!resource->enable_pi_bpsk) {
        return (int)(24 * resource->nof_symbols / resource->occ_lenth);
      }
      return (int)(12 * resource->nof_symbols / resource->occ_lenth);
    default:
      ERROR("Invalid case");
  }
  return SRSLTE_ERROR;
}

// Implements TS 38.212 Table 6.3.1.4.1-1: Rate matching output sequence length E UCI
static int
uci_nr_pucch_E_uci(const srslte_pucch_nr_resource_t* pucch_cfg, const srslte_uci_cfg_nr_t* uci_cfg, uint32_t E_tot)
{
  //  if (uci_cfg->o_csi1 != 0 && uci_cfg->o_csi2) {
  //    ERROR("Simultaneous CSI part 1 and CSI part 2 is not implemented");
  //    return SRSLTE_ERROR;
  //  }

  return E_tot;
}

int srslte_uci_nr_encode_pucch(srslte_uci_nr_t*                  q,
                               const srslte_pucch_nr_resource_t* pucch_resource_cfg,
                               const srslte_uci_cfg_nr_t*        uci_cfg,
                               const srslte_uci_value_nr_t*      value,
                               uint8_t*                          o)
{
  int E_tot = srslte_uci_nr_pucch_format_2_3_4_E(pucch_resource_cfg);
  if (E_tot < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of bits");
    return SRSLTE_ERROR;
  }

  int E_uci = uci_nr_pucch_E_uci(pucch_resource_cfg, uci_cfg, E_tot);
  if (E_uci < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of bits");
    return SRSLTE_ERROR;
  }

  // 6.3.1.1 UCI bit sequence generation
  int A = uci_nr_pack_pucch(uci_cfg, value, q->bit_sequence);
  if (A < SRSLTE_SUCCESS) {
    ERROR("Generating bit sequence");
    return SRSLTE_ERROR;
  }

  return uci_nr_encode(q, uci_cfg, A, o, E_uci);
}

int srslte_uci_nr_decode_pucch(srslte_uci_nr_t*                  q,
                               const srslte_pucch_nr_resource_t* pucch_resource_cfg,
                               const srslte_uci_cfg_nr_t*        uci_cfg,
                               int8_t*                           llr,
                               srslte_uci_value_nr_t*            value)
{
  int E_tot = srslte_uci_nr_pucch_format_2_3_4_E(pucch_resource_cfg);
  if (E_tot < SRSLTE_SUCCESS) {
    return SRSLTE_ERROR;
  }

  int E_uci = uci_nr_pucch_E_uci(pucch_resource_cfg, uci_cfg, E_tot);
  if (E_uci < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of encoded PUCCH UCI bits");
    return SRSLTE_ERROR;
  }

  // 6.3.1.1 UCI bit sequence generation
  int A = uci_nr_A(uci_cfg);
  if (A < SRSLTE_SUCCESS) {
    ERROR("Error getting number of bits");
    return SRSLTE_ERROR;
  }

  if (uci_nr_decode(q, uci_cfg, llr, A, E_uci, &value->valid) < SRSLTE_SUCCESS) {
    ERROR("Error decoding UCI bits");
    return SRSLTE_ERROR;
  }

  // Unpack bits
  if (uci_nr_unpack_pucch(uci_cfg, q->bit_sequence, value) < SRSLTE_SUCCESS) {
    ERROR("Error unpacking PUCCH UCI bits");
    return SRSLTE_ERROR;
  }

  return SRSLTE_SUCCESS;
}

uint32_t srslte_uci_nr_total_bits(const srslte_uci_cfg_nr_t* uci_cfg)
{
  if (uci_cfg == NULL) {
    return 0;
  }

  return uci_cfg->o_ack + uci_cfg->o_sr + srslte_csi_part1_nof_bits(uci_cfg->csi, uci_cfg->nof_csi);
}

uint32_t srslte_uci_nr_info(const srslte_uci_data_nr_t* uci_data, char* str, uint32_t str_len)
{
  uint32_t len = 0;

  len = srslte_print_check(str, str_len, len, "rnti=0x%x", uci_data->cfg.pucch.rnti);

  if (uci_data->cfg.o_ack > 0) {
    char str2[10];
    srslte_vec_sprint_bin(str2, 10, uci_data->value.ack, uci_data->cfg.o_ack);
    len = srslte_print_check(str, str_len, len, ", ack=%s", str2);
  }

  if (uci_data->cfg.nof_csi > 0) {
    len += srslte_csi_str(uci_data->cfg.csi, uci_data->value.csi, uci_data->cfg.nof_csi, &str[len], str_len - len);
  }

  if (uci_data->cfg.o_sr > 0) {
    len = srslte_print_check(str, str_len, len, ", sr=%d", uci_data->value.sr);
  }

  return len;
}

static int uci_nr_pusch_Q_prime_ack(const srslte_uci_nr_pusch_cfg_t* cfg, uint32_t O_ack)
{
  if (cfg == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  uint32_t L_ack = srslte_uci_nr_crc_len(O_ack);              // Number of CRC bits
  uint32_t Qm    = srslte_mod_bits_x_symbol(cfg->modulation); // modulation order of the PUSCH

  uint32_t M_uci_sum    = 0;
  uint32_t M_uci_l0_sum = 0;
  for (uint32_t l = 0; l < SRSLTE_NSYMB_PER_SLOT_NR; l++) {
    M_uci_sum += cfg->M_uci_sc[l];
    if (l >= cfg->l0) {
      M_uci_l0_sum += cfg->M_uci_sc[l];
    }
  }

  if (!isnormal(cfg->R)) {
    ERROR("Invalid Rate (%f)", cfg->R);
    return SRSLTE_ERROR;
  }

  if (cfg->K_sum == 0) {
    return (int)SRSLTE_MIN(ceilf(((O_ack + L_ack) * cfg->beta_harq_ack_offset) / (Qm * cfg->R)),
                           cfg->alpha * M_uci_l0_sum);
  }
  return (int)SRSLTE_MIN(ceilf(((O_ack + L_ack) * cfg->beta_harq_ack_offset * M_uci_sum) / cfg->K_sum),
                         cfg->alpha * M_uci_l0_sum);
}

int srslte_uci_nr_pusch_ack_nof_bits(const srslte_uci_nr_pusch_cfg_t* cfg, uint32_t O_ack)
{
  // Check inputs
  if (cfg == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  if (cfg->nof_layers == 0) {
    ERROR("Invalid number of layers (%d)", cfg->nof_layers);
    return SRSLTE_ERROR;
  }

  int Q_ack_prime = uci_nr_pusch_Q_prime_ack(cfg, O_ack);
  if (Q_ack_prime < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of RE");
    return Q_ack_prime;
  }

  return (int)(Q_ack_prime * cfg->nof_layers * srslte_mod_bits_x_symbol(cfg->modulation));
}

int srslte_uci_nr_encode_pusch_ack(srslte_uci_nr_t*             q,
                                   const srslte_uci_cfg_nr_t*   cfg,
                                   const srslte_uci_value_nr_t* value,
                                   uint8_t*                     o)
{
  int A = cfg->o_ack;

  // Check inputs
  if (q == NULL || cfg == NULL || value == NULL || o == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  // 6.3.2.1 UCI bit sequence generation
  // 6.3.2.1.1 HARQ-ACK
  bool has_csi_part2 = srslte_csi_has_part2(cfg->csi, cfg->nof_csi);
  if (cfg->pusch.K_sum == 0 && cfg->nof_csi > 1 && !has_csi_part2 && A < 2) {
    q->bit_sequence[0] = (A == 0) ? 0 : value->ack[0];
    q->bit_sequence[1] = 0;
    A                  = 2;
  } else if (A == 0) {
    UCI_NR_INFO_TX("No HARQ-ACK to mux");
    return SRSLTE_SUCCESS;
  } else {
    srslte_vec_u8_copy(q->bit_sequence, value->ack, cfg->o_ack);
  }

  // Compute total of encoded bits according to 6.3.2.4 Rate matching
  int E_uci = srslte_uci_nr_pusch_ack_nof_bits(&cfg->pusch, A);
  if (E_uci < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of encoded bits");
    return SRSLTE_ERROR;
  }

  return uci_nr_encode(q, cfg, A, o, E_uci);
}

int srslte_uci_nr_decode_pusch_ack(srslte_uci_nr_t*           q,
                                   const srslte_uci_cfg_nr_t* cfg,
                                   int8_t*                    llr,
                                   srslte_uci_value_nr_t*     value)
{
  int A = cfg->o_ack;

  // Check inputs
  if (q == NULL || cfg == NULL || llr == NULL || value == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  // 6.3.2.1 UCI bit sequence generation
  // 6.3.2.1.1 HARQ-ACK
  bool has_csi_part2 = srslte_csi_has_part2(cfg->csi, cfg->nof_csi);
  if (cfg->pusch.K_sum == 0 && cfg->nof_csi > 1 && !has_csi_part2 && cfg->o_ack < 2) {
    A = 2;
  }

  // Compute total of encoded bits according to 6.3.2.4 Rate matching
  int E_uci = srslte_uci_nr_pusch_ack_nof_bits(&cfg->pusch, A);
  if (E_uci < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of encoded bits");
    return SRSLTE_ERROR;
  }

  // Decode
  if (uci_nr_decode(q, cfg, llr, A, E_uci, &value->valid) < SRSLTE_SUCCESS) {
    ERROR("Error decoding UCI");
    return SRSLTE_ERROR;
  }

  // Unpack
  srslte_vec_u8_copy(value->ack, q->bit_sequence, A);

  return SRSLTE_SUCCESS;
}

static int uci_nr_pusch_Q_prime_csi1(const srslte_uci_nr_pusch_cfg_t* cfg, uint32_t O_csi1, uint32_t O_ack)
{
  if (cfg == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  uint32_t L_ack = srslte_uci_nr_crc_len(O_csi1);             // Number of CRC bits
  uint32_t Qm    = srslte_mod_bits_x_symbol(cfg->modulation); // modulation order of the PUSCH

  int Q_prime_ack = uci_nr_pusch_Q_prime_ack(cfg, SRSLTE_MAX(2, O_ack));
  if (Q_prime_ack < SRSLTE_ERROR) {
    ERROR("Calculating Q_prime_ack");
    return SRSLTE_ERROR;
  }

  uint32_t M_uci_sum = 0;
  for (uint32_t l = 0; l < SRSLTE_NSYMB_PER_SLOT_NR; l++) {
    M_uci_sum += cfg->M_uci_sc[l];
  }

  if (!isnormal(cfg->R)) {
    ERROR("Invalid Rate (%f)", cfg->R);
    return SRSLTE_ERROR;
  }

  if (cfg->K_sum == 0) {
    if (cfg->csi_part2_present) {
      return (int)SRSLTE_MIN(ceilf(((O_csi1 + L_ack) * cfg->beta_csi1_offset) / (Qm * cfg->R)),
                             cfg->alpha * M_uci_sum - Q_prime_ack);
    }
    return (int)(M_uci_sum - Q_prime_ack);
  }
  return (int)SRSLTE_MIN(ceilf(((O_csi1 + L_ack) * cfg->beta_csi1_offset * M_uci_sum) / cfg->K_sum),
                         ceilf(cfg->alpha * M_uci_sum) - Q_prime_ack);
}

int srslte_uci_nr_pusch_csi1_nof_bits(const srslte_uci_cfg_nr_t* cfg)
{
  // Check inputs
  if (cfg == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  int O_csi1 = srslte_csi_part1_nof_bits(cfg->csi, cfg->nof_csi);
  if (O_csi1 < SRSLTE_SUCCESS) {
    ERROR("Errpr calculating CSI part 1 number of bits");
    return SRSLTE_ERROR;
  }
  uint32_t O_ack = SRSLTE_MAX(2, cfg->o_ack);

  int Q_csi1_prime = uci_nr_pusch_Q_prime_csi1(&cfg->pusch, (uint32_t)O_csi1, O_ack);
  if (Q_csi1_prime < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of RE");
    return Q_csi1_prime;
  }

  return (int)(Q_csi1_prime * cfg->pusch.nof_layers * srslte_mod_bits_x_symbol(cfg->pusch.modulation));
}

int srslte_uci_nr_encode_pusch_csi1(srslte_uci_nr_t*             q,
                                    const srslte_uci_cfg_nr_t*   cfg,
                                    const srslte_uci_value_nr_t* value,
                                    uint8_t*                     o)
{
  // Check inputs
  if (q == NULL || cfg == NULL || value == NULL || o == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  int A = srslte_csi_part1_pack(cfg->csi, value->csi, cfg->nof_csi, q->bit_sequence, SRSLTE_UCI_NR_MAX_NOF_BITS);
  if (A < SRSLTE_SUCCESS) {
    ERROR("Error packing CSI part 1 report");
    return SRSLTE_ERROR;
  }

  if (A == 0) {
    UCI_NR_INFO_TX("No CSI part 1 to mux");
    return SRSLTE_SUCCESS;
  }

  // Compute total of encoded bits according to 6.3.2.4 Rate matching
  int E_uci = srslte_uci_nr_pusch_csi1_nof_bits(cfg);
  if (E_uci < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of encoded bits");
    return SRSLTE_ERROR;
  }

  return uci_nr_encode(q, cfg, A, o, E_uci);
}

int srslte_uci_nr_decode_pusch_csi1(srslte_uci_nr_t*           q,
                                    const srslte_uci_cfg_nr_t* cfg,
                                    int8_t*                    llr,
                                    srslte_uci_value_nr_t*     value)
{
  // Check inputs
  if (q == NULL || cfg == NULL || llr == NULL || value == NULL) {
    return SRSLTE_ERROR_INVALID_INPUTS;
  }

  // Compute total of encoded bits according to 6.3.2.4 Rate matching
  int E_uci = srslte_uci_nr_pusch_csi1_nof_bits(cfg);
  if (E_uci < SRSLTE_SUCCESS) {
    ERROR("Error calculating number of encoded bits");
    return SRSLTE_ERROR;
  }

  int A = srslte_csi_part1_nof_bits(cfg->csi, cfg->nof_csi);
  if (A < SRSLTE_SUCCESS) {
    ERROR("Error getting number of CSI part 1 bits");
    return SRSLTE_ERROR;
  }

  // Decode
  if (uci_nr_decode(q, cfg, llr, (uint32_t)A, (uint32_t)E_uci, &value->valid) < SRSLTE_SUCCESS) {
    ERROR("Error decoding UCI");
    return SRSLTE_ERROR;
  }

  // Unpack
  if (srslte_csi_part1_unpack(cfg->csi, cfg->nof_csi, q->bit_sequence, A, value->csi) < SRSLTE_SUCCESS) {
    ERROR("Error unpacking CSI");
    return SRSLTE_ERROR;
  }

  return SRSLTE_SUCCESS;
}