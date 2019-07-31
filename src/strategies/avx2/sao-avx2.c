/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (C) 2013-2015 Tampere University of Technology and others (see
 * COPYING file).
 *
 * Kvazaar is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#include "strategies/avx2/sao-avx2.h"

#if COMPILE_INTEL_AVX2
#include <immintrin.h>
#include <nmmintrin.h>

#include "strategies/generic/sao_band_ddistortion.h"
#include "cu.h"
#include "encoder.h"
#include "encoderstate.h"
#include "kvazaar.h"
#include "sao.h"
#include "strategyselector.h"


// These optimizations are based heavily on sao-generic.c.
// Might be useful to check that if (when) this file
// is difficult to understand.

static INLINE __m128i load_6_pixels(const kvz_pixel* data)
{
  return _mm_insert_epi16(_mm_cvtsi32_si128(*(int32_t*)&(data[0])), *(int16_t*)&(data[4]), 2);
}


// Mapping of edge_idx values to eo-classes.
static int sao_calc_eo_cat(kvz_pixel a, kvz_pixel b, kvz_pixel c)
{
  // Mapping relationships between a, b and c to eo_idx.
  static const int sao_eo_idx_to_eo_category[] = { 1, 2, 0, 3, 4 };

  int eo_idx = 2 + SIGN3((int)c - (int)a) + SIGN3((int)c - (int)b);

  //printf("%d ", SIGN3((int)c - (int)a));
  return sao_eo_idx_to_eo_category[eo_idx];
}

// 32x8b version of the one below
static __m256i calc_eo_cat(__m256i a, __m256i b, __m256i c)
{
  // Subtract 0x80 from unsigneds to use the signed compare on them
  const __m256i epu2epi    = _mm256_set1_epi8  (0x80);
  const __m256i ones       = _mm256_set1_epi8  (0x01);
  const __m256i twos       = _mm256_set1_epi8  (0x02);
  const __m256i idx_to_cat = _mm256_setr_epi64x(0x0403000201, 0,
                                                0x0403000201, 0);

  __m256i a_signed  = _mm256_sub_epi8    (a,          epu2epi);
  __m256i b_signed  = _mm256_sub_epi8    (b,          epu2epi);
  __m256i c_signed  = _mm256_sub_epi8    (c,          epu2epi);

  __m256i c_minus_a = _mm256_subs_epi8   (c_signed,   a_signed);
  __m256i c_minus_b = _mm256_subs_epi8   (c_signed,   b_signed);

  __m256i c_a_sign  = _mm256_sign_epi8   (ones,       c_minus_a);
  __m256i c_b_sign  = _mm256_sign_epi8   (ones,       c_minus_b);

  __m256i signsum   = _mm256_add_epi8    (c_a_sign,   c_b_sign);
  __m256i eo_idx    = _mm256_add_epi8    (signsum,    twos);

  return              _mm256_shuffle_epi8(idx_to_cat, eo_idx);
}

// Mapping of edge_idx values to eo-classes.
static __m256i sao_calc_eo_cat_avx2(__m128i* vector_a_epi8, __m128i* vector_b_epi8, __m128i* vector_c_epi8)
{
  // Mapping relationships between a, b and c to eo_idx.
  __m256i vector_sao_eo_idx_to_eo_category_epi32 = _mm256_setr_epi32(1, 2, 0, 3, 4, 0, 0, 0);

  __m256i eo_idx_epi32 = _mm256_set1_epi32(2);
  __m256i vector_a_epi32 = _mm256_cvtepu8_epi32(*vector_a_epi8);
  __m256i vector_b_epi32 = _mm256_cvtepu8_epi32(*vector_b_epi8);
  __m256i vector_c_epi32 = _mm256_cvtepu8_epi32(*vector_c_epi8);

  __m256i temp1_epi32 = _mm256_sign_epi32(_mm256_set1_epi32(1), _mm256_sub_epi32(vector_c_epi32, vector_a_epi32));
  __m256i temp2_epi32 = _mm256_sign_epi32(_mm256_set1_epi32(1), _mm256_sub_epi32(vector_c_epi32, vector_b_epi32));


  eo_idx_epi32 = _mm256_add_epi32(eo_idx_epi32, temp1_epi32);
  eo_idx_epi32 = _mm256_add_epi32(eo_idx_epi32, temp2_epi32);

  __m256i v_cat_epi32 = _mm256_permutevar8x32_epi32(vector_sao_eo_idx_to_eo_category_epi32, eo_idx_epi32);
  return v_cat_epi32;
}

static INLINE __m256i srli_epi8(__m256i v, const uint32_t shift)
{
  const uint8_t hibit_mask     = 0xff >> shift;
  const __m256i hibit_mask_256 = _mm256_set1_epi8(hibit_mask);

  __m256i v_shifted = _mm256_srli_epi32(v,         shift);
  __m256i v_masked  = _mm256_and_si256 (v_shifted, hibit_mask_256);

  return v_masked;
}

static INLINE void cvt_epu8_epi16(const __m256i v, __m256i *res_lo, __m256i *res_hi)
{
  const __m256i zero  = _mm256_setzero_si256();
             *res_lo  = _mm256_unpacklo_epi8(v, zero);
             *res_hi  = _mm256_unpackhi_epi8(v, zero);
}

static INLINE void cvt_epi8_epi16(const __m256i v, __m256i *res_lo, __m256i *res_hi)
{
  const __m256i zero  = _mm256_setzero_si256();
        __m256i signs = _mm256_cmpgt_epi8   (zero, v);
             *res_lo  = _mm256_unpacklo_epi8(v,    signs);
             *res_hi  = _mm256_unpackhi_epi8(v,    signs);
}

// Convert a byte-addressed mask for VPSHUFB into two word-addressed ones, for
// example:
// 7 3 6 2 5 1 4 0 => e f 6 7 c d 4 5 a b 2 3 8 9 0 1
static INLINE void cvt_shufmask_epi8_epi16(__m256i v, __m256i *res_lo, __m256i *res_hi)
{
  const __m256i zero = _mm256_setzero_si256();
  const __m256i ones = _mm256_set1_epi8(1);

  // There's no 8-bit shift, so highest bit could bleed into neighboring byte
  // if set. To avoid it, reset all sign bits with max. The only valid input
  // values for v are [0, 7] anyway and invalid places should be masked out by
  // caller, so it doesn't matter that we turn negative bytes into garbage.
  __m256i v_nonnegs  = _mm256_max_epi8  (zero,      v);
  __m256i v_lobytes  = _mm256_slli_epi32(v_nonnegs, 1);
  __m256i v_hibytes  = _mm256_add_epi8  (v_lobytes, ones);

          *res_lo    = _mm256_unpacklo_epi8(v_lobytes, v_hibytes);
          *res_hi    = _mm256_unpackhi_epi8(v_lobytes, v_hibytes);
}

/*
static int sao_edge_ddistortion_avx2(const kvz_pixel *orig_data,
 const kvz_pixel *rec_data,
 int block_width,
 int block_height,
 int eo_class,
 int offsets[NUM_SAO_EDGE_CATEGORIES])
{
  int y, x;
  vector2d_t a_ofs = g_sao_edge_offsets[eo_class][0];
  vector2d_t b_ofs = g_sao_edge_offsets[eo_class][1];

  __m256i offsets_epi32 = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((__m128i*) offsets)), _mm_insert_epi32(_mm_setzero_si128(), offsets[4], 0), 1);
  __m256i tmp_diff_epi32;
  __m256i tmp_sum_epi32 = _mm256_setzero_si256();
  __m256i tmp_offset_epi32;
  __m256i tmp1_vec_epi32;
  __m256i tmp2_vec_epi32;

  int sum = 0;
  for (y = 1; y < block_height - 1; ++y) {
    for (x = 1; x < block_width - 8; x+=8) {
      const kvz_pixel *c_data = &rec_data[y * block_width + x];

      __m128i vector_a_epi8 = _mm_loadl_epi64((__m128i*)&c_data[a_ofs.y * block_width + a_ofs.x]);
      __m128i vector_c_epi8 = _mm_loadl_epi64((__m128i*)&c_data[0]);
      __m128i vector_b_epi8 = _mm_loadl_epi64((__m128i*)&c_data[b_ofs.y * block_width + b_ofs.x]);


      __m256i v_cat_epi32 = sao_calc_eo_cat_avx2(&vector_a_epi8, &vector_b_epi8, &vector_c_epi8);

      tmp_diff_epi32 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i* __restrict)&(orig_data[y * block_width + x]))), _mm256_cvtepu8_epi32(vector_c_epi8));

      tmp_offset_epi32 = _mm256_permutevar8x32_epi32(offsets_epi32, v_cat_epi32);   

      // (diff - offset) * (diff - offset)
      tmp1_vec_epi32 = _mm256_mullo_epi32(_mm256_sub_epi32(tmp_diff_epi32, tmp_offset_epi32), _mm256_sub_epi32(tmp_diff_epi32, tmp_offset_epi32));

      // diff * diff
      tmp2_vec_epi32 = _mm256_mullo_epi32(tmp_diff_epi32, tmp_diff_epi32);

      // Offset is applied to reconstruction, so it is subtracted from diff.
      // sum += (diff - offset) * (diff - offset) - diff * diff;

      tmp_sum_epi32 = _mm256_add_epi32(tmp_sum_epi32, _mm256_sub_epi32(tmp1_vec_epi32, tmp2_vec_epi32));
    }

    // Load the last 6 pixels to use

    const kvz_pixel *c_data = &rec_data[y * block_width + x];

    __m128i vector_a_epi8 = load_6_pixels(&c_data[a_ofs.y * block_width + a_ofs.x]);
    __m128i vector_c_epi8 = load_6_pixels(c_data);
    __m128i vector_b_epi8 = load_6_pixels(&c_data[b_ofs.y * block_width + b_ofs.x]);

    __m256i v_cat_epi32 = sao_calc_eo_cat_avx2(&vector_a_epi8, &vector_b_epi8, &vector_c_epi8);

    const kvz_pixel* orig_ptr = &(orig_data[y * block_width + x]);

    tmp_diff_epi32 = _mm256_cvtepu8_epi32(load_6_pixels(orig_ptr));

    tmp_diff_epi32 = _mm256_sub_epi32(tmp_diff_epi32, _mm256_cvtepu8_epi32(vector_c_epi8));

    tmp_offset_epi32 = _mm256_permutevar8x32_epi32(offsets_epi32, v_cat_epi32);

    // (diff - offset) * (diff - offset)
    tmp1_vec_epi32 = _mm256_mullo_epi32(_mm256_sub_epi32(tmp_diff_epi32, tmp_offset_epi32), _mm256_sub_epi32(tmp_diff_epi32, tmp_offset_epi32));

    // diff * diff
    tmp2_vec_epi32 = _mm256_mullo_epi32(tmp_diff_epi32, tmp_diff_epi32);

    // Offset is applied to reconstruction, so it is subtracted from diff.
    // sum += (diff - offset) * (diff - offset) - diff * diff;

    tmp_sum_epi32 = _mm256_add_epi32(tmp_sum_epi32, _mm256_sub_epi32(tmp1_vec_epi32, tmp2_vec_epi32));

    tmp_sum_epi32 = _mm256_hadd_epi32(tmp_sum_epi32, tmp_sum_epi32);
    tmp_sum_epi32 = _mm256_hadd_epi32(tmp_sum_epi32, tmp_sum_epi32);

    tmp_sum_epi32 = _mm256_add_epi32(tmp_sum_epi32, _mm256_shuffle_epi32(tmp_sum_epi32, _MM_SHUFFLE(0, 1, 0, 1)));
    sum += _mm_cvtsi128_si32(_mm256_castsi256_si128(tmp_sum_epi32));

    tmp_sum_epi32 = _mm256_setzero_si256();
  }
  return sum;
}
*/




/**
* \param orig_data  Original pixel data. 64x64 for luma, 32x32 for chroma.
* \param rec_data  Reconstructed pixel data. 64x64 for luma, 32x32 for chroma.
* \param dir_offsets
* \param is_chroma  0 for luma, 1 for chroma. Indicates
*/

// For some reason this solution doesn't work currently. Bug appears while adding. Counting should work
static void calc_sao_edge_dir_avx2(const kvz_pixel *orig_data,
 const kvz_pixel *rec_data,
 int eo_class,
 int block_width,
 int block_height,
 int cat_sum_cnt[2][NUM_SAO_EDGE_CATEGORIES])
{
  int y, x;
  vector2d_t a_ofs = g_sao_edge_offsets[eo_class][0];
  vector2d_t b_ofs = g_sao_edge_offsets[eo_class][1];
  // Arrays orig_data and rec_data are quarter size for chroma.

  // Don't sample the edge pixels because this function doesn't have access to
  // their neighbours.
  __m256i zeros_epi32 = _mm256_setzero_si256();
  __m256i ones_epi32 = _mm256_set1_epi32(1);
  __m256i twos_epi32 = _mm256_set1_epi32(2);
  __m256i threes_epi32 = _mm256_set1_epi32(3);
  __m256i fours_epi32 = _mm256_set1_epi32(4);

  __m256i v_diff_accum[NUM_SAO_EDGE_CATEGORIES] = { { 0 } };



  __m256i temp_epi32 = _mm256_setzero_si256();
  __m256i temp_mem_epi32 = _mm256_setzero_si256();

  for (y = 1; y < block_height - 1; ++y) {
    for (x = 1; x < block_width - 8; x += 8) {
      const kvz_pixel *c_data = &rec_data[y * block_width + x];

      __m128i vector_a_epi8 = _mm_loadl_epi64((__m128i*)&c_data[a_ofs.y * block_width + a_ofs.x]);
      __m128i vector_c_epi8 = _mm_loadl_epi64((__m128i*)c_data);
      __m128i vector_b_epi8 = _mm_loadl_epi64((__m128i*)&c_data[b_ofs.y * block_width + b_ofs.x]);


      __m256i v_cat_epi32 = sao_calc_eo_cat_avx2(&vector_a_epi8, &vector_b_epi8, &vector_c_epi8);

      // Check wich values are right for specific cat amount.
      // It's done for every single value that cat could get {1, 2, 0, 3, 4}

      //--------------------------------------------------------------------------
      // v_cat == 0
      __m256i mask_epi32 = _mm256_cmpeq_epi32(zeros_epi32, v_cat_epi32);

      temp_mem_epi32 = _mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)&(orig_data[y * block_width + x]))), _mm256_cvtepu8_epi32(vector_c_epi8));
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[0] = _mm256_add_epi32(v_diff_accum[0], temp_epi32);
      int temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32))/ 4;
      cat_sum_cnt[1][0] += temp_cnt;

      //--------------------------------------------------------------------------
      // v_cat == 1

      mask_epi32 = _mm256_cmpeq_epi32(ones_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[1] = _mm256_add_epi32(v_diff_accum[1], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][1] += temp_cnt;

      //--------------------------------------------------------------------------
      // v_cat == 2

      mask_epi32 = _mm256_cmpeq_epi32(twos_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[2] = _mm256_add_epi32(v_diff_accum[2], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][2] += temp_cnt;

      //--------------------------------------------------------------------------
      // v_cat == 3

      mask_epi32 = _mm256_cmpeq_epi32(threes_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[3] = _mm256_add_epi32(v_diff_accum[3], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][3] += temp_cnt;

      //--------------------------------------------------------------------------
      // v_cat == 4

      mask_epi32 = _mm256_cmpeq_epi32(fours_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[4] = _mm256_add_epi32(v_diff_accum[4], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][4] += temp_cnt;

      //--------------------------------------------------------------------------


    }

    if (block_width - x - 1 >= 6) {
      const kvz_pixel *c_data = &rec_data[y * block_width + x];

      __m128i vector_a_epi8 = load_6_pixels(&c_data[a_ofs.y * block_width + a_ofs.x]);
      __m128i vector_c_epi8 = load_6_pixels(c_data);
      __m128i vector_b_epi8 = load_6_pixels(&c_data[b_ofs.y * block_width + b_ofs.x]);

      __m256i v_cat_epi32 = sao_calc_eo_cat_avx2(&vector_a_epi8, &vector_b_epi8, &vector_c_epi8);

      const kvz_pixel* orig_ptr = &(orig_data[y * block_width + x]);

      temp_mem_epi32 = _mm256_cvtepu8_epi32(load_6_pixels(orig_ptr));

      temp_mem_epi32 = _mm256_sub_epi32(temp_mem_epi32, _mm256_cvtepu8_epi32(vector_c_epi8));


      // Check wich values are right for specific cat amount.
      // It's done for every single value that cat could get {1, 2, 0, 3, 4}
      //--------------------------------------------------------------------------
      __m256i mask_epi32 = _mm256_cmpeq_epi32(zeros_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[0] = _mm256_add_epi32(v_diff_accum[0], temp_epi32);
      int temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4 - 2;
      cat_sum_cnt[1][0] += temp_cnt;
      //--------------------------------------------------------------------------

      mask_epi32 = _mm256_cmpeq_epi32(ones_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[1] = _mm256_add_epi32(v_diff_accum[1], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][1] += temp_cnt;

      //--------------------------------------------------------------------------

      mask_epi32 = _mm256_cmpeq_epi32(twos_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[2] = _mm256_add_epi32(v_diff_accum[2], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][2] += temp_cnt;

      //--------------------------------------------------------------------------

      mask_epi32 = _mm256_cmpeq_epi32(threes_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[3] = _mm256_add_epi32(v_diff_accum[3], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][3] += temp_cnt;

      //--------------------------------------------------------------------------

      mask_epi32 = _mm256_cmpeq_epi32(fours_epi32, v_cat_epi32);
      temp_epi32 = _mm256_and_si256(temp_mem_epi32, mask_epi32);
      v_diff_accum[4] = _mm256_add_epi32(v_diff_accum[4], temp_epi32);
      temp_cnt = _mm_popcnt_u32(_mm256_movemask_epi8(mask_epi32)) / 4;
      cat_sum_cnt[1][4] += temp_cnt;

      //--------------------------------------------------------------------------
      x += 6;
    }


    // If odd number of pixels left, use this
    for (; x < block_width - 1; ++x) {
      const kvz_pixel *c_data = &rec_data[y * block_width + x];
      kvz_pixel a = c_data[a_ofs.y * block_width + a_ofs.x];
      kvz_pixel c = c_data[0];
      kvz_pixel b = c_data[b_ofs.y * block_width + b_ofs.x];

      int eo_cat = sao_calc_eo_cat(a, b, c);

      cat_sum_cnt[0][eo_cat] += orig_data[y * block_width + x] - c;
      cat_sum_cnt[1][eo_cat] += 1;
    }
  }

  for (int eo_cat = 0; eo_cat < NUM_SAO_EDGE_CATEGORIES; ++eo_cat) {
    int accum = 0;

    //Full horizontal sum of accumulated values
    v_diff_accum[eo_cat] = _mm256_add_epi32(v_diff_accum[eo_cat], _mm256_castsi128_si256(_mm256_extracti128_si256(v_diff_accum[eo_cat], 1)));
    v_diff_accum[eo_cat] = _mm256_add_epi32(v_diff_accum[eo_cat], _mm256_shuffle_epi32(v_diff_accum[eo_cat], _MM_SHUFFLE(1, 0, 3, 2)));
    v_diff_accum[eo_cat] = _mm256_add_epi32(v_diff_accum[eo_cat], _mm256_shuffle_epi32(v_diff_accum[eo_cat], _MM_SHUFFLE(0, 1, 0, 1)));
    accum += _mm_cvtsi128_si32(_mm256_castsi256_si128(v_diff_accum[eo_cat]));
    cat_sum_cnt[0][eo_cat] += accum;
  }
}

/*
 * Calculate an array of intensity correlations for each intensity value.
 * Return array as 16 YMM vectors, each containing 2x16 unsigned bytes
 * (to ease array lookup from YMMs using the shuffle trick, the low and
 * high lanes of each vector are duplicates). Have fun scaling this to
 * 16-bit picture data!
 */
static void calc_sao_offset_array_avx2(const encoder_control_t *encoder,
                                       const sao_info_t        *sao,
                                             __m256i           *offsets,
                                             color_t            color_i)
{
  const uint32_t band_pos   = (color_i == COLOR_V) ? 1 : 0;
  const  int32_t cur_bp     = sao->band_position[band_pos];

  const __m256i  zero       = _mm256_setzero_si256();
  const __m256i  threes     = _mm256_set1_epi8  (  3);

  const __m256i  band_pos_v = _mm256_set1_epi8  (band_pos << 2);
  const __m256i  cur_bp_v   = _mm256_set1_epi8  (cur_bp);
  const __m256i  val_incr   = _mm256_set1_epi8  (16);
  const __m256i  band_incr  = _mm256_set1_epi8  ( 2);
        __m256i  vals       = _mm256_setr_epi8  ( 0,  1,  2,  3,  4,  5,  6,  7,
                                                  8,  9, 10, 11, 12, 13, 14, 15,
                                                  0,  1,  2,  3,  4,  5,  6,  7,
                                                  8,  9, 10, 11, 12, 13, 14, 15);

        __m256i  bands     = _mm256_setr_epi32 (0, 0, 0x01010101, 0x01010101,
                                                0, 0, 0x01010101, 0x01010101);

  // We'll only ever address SAO offsets 1, 2, 3, 4, 6, 7, 8, 9, so only load
  // them and truncate into signed 16 bits (anything out of that range will
  // anyway saturate anything they're used to do)
  __m128i sao_offs_lo  = _mm_loadu_si128((const __m128i *)(sao->offsets + 1));
  __m128i sao_offs_hi  = _mm_loadu_si128((const __m128i *)(sao->offsets + 6));

  __m128i sao_offs_xmm = _mm_packs_epi32        (sao_offs_lo, sao_offs_hi);
  __m256i sao_offs     = _mm256_castsi128_si256 (sao_offs_xmm);
          sao_offs     = _mm256_inserti128_si256(sao_offs,    sao_offs_xmm, 1);

  for (uint32_t i = 0; i < 16; i++) {
    // bands will always be in [0, 31], and cur_bp in [0, 27], so no overflow
    // can occur
    __m256i band_m_bp = _mm256_sub_epi8    (bands,  cur_bp_v);

    // If (x & ~3) != 0 for any signed x, then x < 0 or x > 3
    __m256i bmbp_bads = _mm256_andnot_si256(threes,    band_m_bp);
    __m256i in_band   = _mm256_cmpeq_epi8  (zero,      bmbp_bads);

    __m256i offset_id = _mm256_add_epi8    (band_m_bp, band_pos_v);

    __m256i val_lo, val_hi;
    cvt_epu8_epi16(vals, &val_lo, &val_hi);

    __m256i offid_lo, offid_hi;
    cvt_shufmask_epi8_epi16(offset_id, &offid_lo, &offid_hi);

    __m256i offs_lo = _mm256_shuffle_epi8(sao_offs, offid_lo);
    __m256i offs_hi = _mm256_shuffle_epi8(sao_offs, offid_hi);

    __m256i sums_lo = _mm256_adds_epi16  (val_lo,   offs_lo);
    __m256i sums_hi = _mm256_adds_epi16  (val_hi,   offs_hi);

            sums_lo = _mm256_max_epi16   (sums_lo,  zero);
            sums_hi = _mm256_max_epi16   (sums_hi,  zero);

    __m256i offs    = _mm256_packus_epi16(sums_lo,  sums_hi);

    offsets[i]      = _mm256_blendv_epi8 (vals,     offs, in_band);

            vals    = _mm256_add_epi8    (vals,     val_incr);
            bands   = _mm256_add_epi8    (bands,    band_incr);
  }
}

static __m256i lookup_color_band_ymm(__m256i curr_row, const __m256i *offsets)
{
  const __m256i select_nibble = _mm256_set1_epi8   (0x0f);
  const __m256i lo_nibbles    = _mm256_and_si256   (select_nibble, curr_row);
  const __m256i hi_nibbles    = _mm256_andnot_si256(select_nibble, curr_row);

  // Loop through the offset vectors, the 0xi'th one always holding
  // offsets 0xi0...0xif. Use shuffle to do a lookup on the current
  // offset vector, then check which pixels actually should be looked
  // up from this vector (ie. whether their values are 0xi0...0xif) and
  // mask out any but correct ones.
  __m256i result_row = _mm256_setzero_si256();
  for (uint8_t i = 0; i < 16; i += 4) {

    __m256i curr_hinib0   = _mm256_set1_epi8   ((i + 0) << 4);
    __m256i curr_hinib1   = _mm256_set1_epi8   ((i + 1) << 4);
    __m256i curr_hinib2   = _mm256_set1_epi8   ((i + 2) << 4);
    __m256i curr_hinib3   = _mm256_set1_epi8   ((i + 3) << 4);

    __m256i hinib_select0 = _mm256_cmpeq_epi8  (curr_hinib0,    hi_nibbles);
    __m256i hinib_select1 = _mm256_cmpeq_epi8  (curr_hinib1,    hi_nibbles);
    __m256i hinib_select2 = _mm256_cmpeq_epi8  (curr_hinib2,    hi_nibbles);
    __m256i hinib_select3 = _mm256_cmpeq_epi8  (curr_hinib3,    hi_nibbles);

    __m256i lonib_lookup0 = _mm256_shuffle_epi8(offsets[i + 0], lo_nibbles);
    __m256i lonib_lookup1 = _mm256_shuffle_epi8(offsets[i + 1], lo_nibbles);
    __m256i lonib_lookup2 = _mm256_shuffle_epi8(offsets[i + 2], lo_nibbles);
    __m256i lonib_lookup3 = _mm256_shuffle_epi8(offsets[i + 3], lo_nibbles);

    __m256i lookup_mskd0  = _mm256_and_si256   (hinib_select0,  lonib_lookup0);
    __m256i lookup_mskd1  = _mm256_and_si256   (hinib_select1,  lonib_lookup1);
    __m256i lookup_mskd2  = _mm256_and_si256   (hinib_select2,  lonib_lookup2);
    __m256i lookup_mskd3  = _mm256_and_si256   (hinib_select3,  lonib_lookup3);

    __m256i lookup_mskd01 = _mm256_or_si256    (lookup_mskd0,   lookup_mskd1);
    __m256i lookup_mskd23 = _mm256_or_si256    (lookup_mskd2,   lookup_mskd3);
    __m256i lookup_res    = _mm256_or_si256    (lookup_mskd01,  lookup_mskd23);

            result_row    = _mm256_or_si256    (result_row,     lookup_res);
  }
  return result_row;
}

static INLINE void reconstruct_color_band(const encoder_control_t *encoder,
                                          const kvz_pixel         *rec_data,
                                                kvz_pixel         *new_rec_data,
                                          const sao_info_t        *sao,
                                                int32_t            stride,
                                                int32_t            new_stride,
                                                int32_t            block_width,
                                                int32_t            block_height,
                                                color_t            color_i)
{
  const uint32_t width_db32 = block_width & ~31;
  const uint32_t width_db4  = block_width &  ~3;
  const uint32_t width_rest = block_width &   3;

  // Form the load&store mask
  const __m256i wdb4_256      = _mm256_set1_epi32 (width_db4 & 31);
  const __m256i indexes       = _mm256_setr_epi32 (3, 7, 11, 15, 19, 23, 27, 31);
  const __m256i db4_mask      = _mm256_cmpgt_epi32(wdb4_256, indexes);

  // Each of the 256 offsets is a byte, but only 16 are held in one YMM since
  // lanes must be duplicated to use shuffle.
  __m256i offsets[16];
  calc_sao_offset_array_avx2(encoder, sao, offsets, color_i);

  for (uint32_t y = 0; y < block_height; y++) {
    uint32_t x = 0;
    for (; x < width_db32; x += 32) {
      const uint32_t curr_srcpos = y *     stride + x;
      const uint32_t curr_dstpos = y * new_stride + x;

      __m256i curr_row = _mm256_loadu_si256((const __m256i *)(rec_data + curr_srcpos));
      __m256i result   = lookup_color_band_ymm(curr_row, offsets);
      _mm256_storeu_si256((__m256i *)(new_rec_data + curr_dstpos), result);
    }
    if (block_width > width_db32) {
      const uint32_t curr_srcpos = y *     stride + x;
      const uint32_t curr_dstpos = y * new_stride + x;
      const uint32_t rest_srcpos = y *     stride + width_db4;
      const uint32_t rest_dstpos = y * new_stride + width_db4;

      // Read the very last pixels byte by byte and pack them into one dword.
      // Piggyback said dword as the highest dword of the row vector variable,
      // that particular place can never be loaded into by the maskmove
      // (otherwise that vector would go through the divisible-by-32 code
      // path).
      uint32_t last_dword = 0;
      for (uint32_t i = 0; i < width_rest; i++) {
        uint8_t  currb = rec_data[rest_srcpos + i];
        uint32_t currd = ((uint32_t)currb) << (i * 8);
        last_dword |= currd;
      }
      const int32_t *src_ptr = (const int32_t *)(    rec_data + curr_srcpos);
            int32_t *dst_ptr = (      int32_t *)(new_rec_data + curr_dstpos);

      __m256i curr_row = _mm256_maskload_epi32(src_ptr,  db4_mask);
              curr_row = _mm256_insert_epi32  (curr_row, last_dword, 7);
      __m256i result   = lookup_color_band_ymm(curr_row, offsets);

      _mm256_maskstore_epi32(dst_ptr, db4_mask, result);
      uint32_t last_dword_dst = _mm256_extract_epi32(result, 7);

      for (uint32_t i = 0; i < width_rest; i++) {
        uint8_t currb = last_dword_dst & 0xff;
        new_rec_data[rest_dstpos + i] = currb;
        last_dword_dst >>= 8;
      }
    }
  }
}

static __m256i do_one_nonband_ymm(__m256i a,
                                  __m256i b,
                                  __m256i c,
                                  __m256i sao_offs)
{
  const __m256i   zero    = _mm256_setzero_si256();

  __m256i eo_cat = calc_eo_cat(a, b, c);
  __m256i eo_cat_lo, eo_cat_hi, c_lo, c_hi;
  cvt_shufmask_epi8_epi16(eo_cat, &eo_cat_lo, &eo_cat_hi);
  cvt_epu8_epi16         (c,      &c_lo,      &c_hi);

  __m256i offs_lo = _mm256_shuffle_epi8(sao_offs, eo_cat_lo);
  __m256i offs_hi = _mm256_shuffle_epi8(sao_offs, eo_cat_hi);

  __m256i res_lo  = _mm256_adds_epi16  (offs_lo,  c_lo);
  __m256i res_hi  = _mm256_adds_epi16  (offs_hi,  c_hi);

          res_lo  = _mm256_max_epi16   (res_lo,   zero);
          res_hi  = _mm256_max_epi16   (res_hi,   zero);

  __m256i res     = _mm256_packus_epi16(res_lo,   res_hi);
  return res;
}

static INLINE void reconstruct_color_other(const encoder_control_t *encoder,
                                           const kvz_pixel         *rec_data,
                                                 kvz_pixel         *new_rec_data,
                                           const sao_info_t        *sao,
                                                 int32_t            stride,
                                                 int32_t            new_stride,
                                                 int32_t            block_width,
                                                 int32_t            block_height,
                                                 color_t            color_i)
{
  const uint32_t   offset_v    = color_i == COLOR_V ? 5 : 0;
  const vector2d_t a_ofs       = g_sao_edge_offsets[sao->eo_class][0];
  const vector2d_t b_ofs       = g_sao_edge_offsets[sao->eo_class][1];

  // All limited to [-1, 1]
  const int32_t    a_xoff      = a_ofs.x;
  const int32_t    a_yoff      = a_ofs.y;
  const int32_t    b_xoff      = b_ofs.x;
  const int32_t    b_yoff      = b_ofs.y;

  const uint32_t   width_db32  = block_width & ~31;
  const uint32_t   width_db4   = block_width &  ~3;
  const uint32_t   width_rest  = block_width &   3;

  // Form the load&store mask
  const __m256i    wdb4_256    = _mm256_set1_epi32 (width_db4 & 31);
  const __m256i    indexes     = _mm256_setr_epi32 (3, 7, 11, 15, 19, 23, 27, 31);
  const __m256i    db4_mask    = _mm256_cmpgt_epi32(wdb4_256, indexes);

  // Again, saturate offsets to signed 16 bits, because anything outside of
  // [-255, 255] will saturate anything these are used with
  const __m128i    sao_offs_lo = _mm_loadu_si128  ((const __m128i *)(sao->offsets + offset_v + 0));
  const __m128i    sao_offs_hi = _mm_cvtsi32_si128(sao->offsets[offset_v + 4]);
  const __m128i    sao_offs_16 = _mm_packs_epi32  (sao_offs_lo, sao_offs_hi);

  const __m256i    sao_offs    = _mm256_broadcastsi128_si256(sao_offs_16);

  for (uint32_t y = 0; y < block_height; y++) {
    uint32_t x;
    for (x = 0; x < width_db32; x += 32) {
      const uint32_t  src_pos = y *     stride + x;
      const uint32_t  dst_pos = y * new_stride + x;

      // TODO: these will go negative, but that's a defect of the original
      // code already since 2013 (98f2a1aedc5f4933c2729ae15412549dea9e5549)
      const int32_t   a_pos   = (y + a_yoff) * stride + x + a_xoff;
      const int32_t   b_pos   = (y + b_yoff) * stride + x + b_xoff;

      __m256i a = _mm256_loadu_si256((const __m256i *)(rec_data + a_pos));
      __m256i b = _mm256_loadu_si256((const __m256i *)(rec_data + b_pos));
      __m256i c = _mm256_loadu_si256((const __m256i *)(rec_data + src_pos));

      __m256i res = do_one_nonband_ymm(a, b, c, sao_offs);
      _mm256_storeu_si256((__m256i *)(new_rec_data + dst_pos), res);
    }
    if (block_width > width_db32) {
      const uint32_t curr_srcpos = y * stride + x;
      const uint32_t rest_srcpos = y * stride + width_db4;

      const  int32_t curr_apos   = (y + a_yoff) * stride + a_xoff + x;
      const  int32_t rest_apos   = (y + a_yoff) * stride + a_xoff + width_db4;

      const  int32_t curr_bpos   = (y + b_yoff) * stride + b_xoff + x;
      const  int32_t rest_bpos   = (y + b_yoff) * stride + b_xoff + width_db4;

      const uint32_t curr_dstpos = y * new_stride + x;
      const uint32_t rest_dstpos = y * new_stride + width_db4;

      // Same trick to read a narrow line as there is in the band SAO routine
      uint32_t a_last = 0, b_last = 0, c_last = 0;
      for (uint32_t i = 0; i < width_rest; i++) {
        uint8_t  currb_a = rec_data[rest_apos   + (int32_t)i];
        uint8_t  currb_b = rec_data[rest_bpos   + (int32_t)i];
        uint8_t  currb_c = rec_data[rest_srcpos + (int32_t)i];

        uint32_t currd_a = ((uint32_t)currb_a) << (i * 8);
        uint32_t currd_b = ((uint32_t)currb_b) << (i * 8);
        uint32_t currd_c = ((uint32_t)currb_c) << (i * 8);

        a_last |= currd_a;
        b_last |= currd_b;
        c_last |= currd_c;
      }
      const int32_t   *a_ptr = (const int32_t *)(    rec_data + curr_apos);
      const int32_t   *b_ptr = (const int32_t *)(    rec_data + curr_bpos);
      const int32_t   *c_ptr = (const int32_t *)(    rec_data + curr_srcpos);
            int32_t *dst_ptr = (      int32_t *)(new_rec_data + curr_dstpos);

      __m256i a = _mm256_maskload_epi32(a_ptr, db4_mask);
      __m256i b = _mm256_maskload_epi32(b_ptr, db4_mask);
      __m256i c = _mm256_maskload_epi32(c_ptr, db4_mask);

              a = _mm256_insert_epi32  (a, a_last, 7);
              b = _mm256_insert_epi32  (b, b_last, 7);
              c = _mm256_insert_epi32  (c, c_last, 7);

      __m256i res = do_one_nonband_ymm(a, b, c, sao_offs);
      _mm256_maskstore_epi32(dst_ptr, db4_mask, res);

      uint32_t last_dword = _mm256_extract_epi32(res, 7);

      for (uint32_t i = 0; i < width_rest; i++) {
        uint8_t currb = last_dword & 0xff;
        new_rec_data[rest_dstpos + i] = currb;
        last_dword >>= 8;
      }
    }
  }
}

static void sao_reconstruct_color_avx2(const encoder_control_t * const encoder,
 const kvz_pixel *rec_data,
 kvz_pixel *new_rec_data,
 const sao_info_t *sao,
 int stride,
 int new_stride,
 int block_width,
 int block_height,
 color_t color_i)
{
  if (sao->type == SAO_TYPE_BAND) {
    reconstruct_color_band (encoder, rec_data, new_rec_data, sao, stride, new_stride, block_width, block_height, color_i);
  }
  else {
    reconstruct_color_other(encoder, rec_data, new_rec_data, sao, stride, new_stride, block_width, block_height, color_i);
  }
}

static int32_t sao_band_ddistortion_avx2(const encoder_state_t *state,
                                         const uint8_t         *orig_data,
                                         const uint8_t         *rec_data,
                                               int32_t          block_width,
                                               int32_t          block_height,
                                               int32_t          band_pos,
                                         const int32_t          sao_bands[4])
{
  const uint32_t bitdepth = 8;
  const uint32_t shift    = bitdepth - 5;

  // Clamp band_pos to 32 from above. It'll be subtracted from the shifted
  // rec_data values, which in 8-bit depth will always be clamped to [0, 31],
  // so if it ever exceeds 32, all the band values will be negative and
  // ignored. Ditto for less than -4.
  __m128i bp_128   = _mm_cvtsi32_si128    (band_pos);
  __m128i hilimit  = _mm_cvtsi32_si128    (32);
  __m128i lolimit  = _mm_cvtsi32_si128    (-4);

          bp_128   = _mm_min_epi8         (bp_128, hilimit);
          bp_128   = _mm_max_epi8         (bp_128, lolimit);

  __m256i bp_256  = _mm256_broadcastb_epi8(bp_128);

  // LSBs of each SAO band dword, the band values must fit in 8 bits anyway
  // (this will be checked later)
  const __m128i sb_shufmask = _mm_set1_epi32(0x0c080400);

  __m128i sbs_32   = _mm_loadu_si128((const __m128i *)sao_bands);

  __m128i sbs_8    = _mm_shuffle_epi8            (sbs_32, sb_shufmask);
  __m256i sb_256   = _mm256_broadcastsi128_si256 (sbs_8);

  // Compare most significant 25 bits of SAO bands to the sign bit to assert
  // that the band is between -128 and 127 (only comparing 24 would fail to
  // detect values of 128...255)
  __m128i sb_ms25b = _mm_srai_epi32              (sbs_32,  7);
  __m128i sb_signs = _mm_srai_epi32              (sbs_32, 31);
  __m128i sbs_ok_v = _mm_cmpeq_epi32             (sb_ms25b, sb_signs);
  uint16_t sbs_ok  = _mm_movemask_epi8           (sbs_ok_v);

  // These should trigger like, never, at least the later condition of block
  // not being a multiple of 32 wide. Rather safe than sorry though, huge SAO
  // bands are more tricky of these two because the algorithm needs a complete
  // reimplementation to work on 16-bit values.
  if (sbs_ok != 0xffff)
    goto use_generic;

  // If VVC or something will start using SAO on blocks with width a multiple
  // of 16, feel free to implement a XMM variant of this algorithm
  if ((block_width & 31) != 0)
    goto use_generic;

  const __m256i zero          = _mm256_setzero_si256();
  const __m256i threes        = _mm256_set1_epi8 (3);
  const __m256i negate_hiword = _mm256_set1_epi32(0xffff0001);

  __m256i sum = _mm256_setzero_si256();
  for (uint32_t y = 0; y < block_height; y++) {
    for (uint32_t x = 0; x < block_width; x += 32) {
      const int32_t curr_pos = y * block_width + x;

      __m256i   rd = _mm256_loadu_si256((const __m256i *)( rec_data + curr_pos));
      __m256i orig = _mm256_loadu_si256((const __m256i *)(orig_data + curr_pos));

      __m256i orig_lo, orig_hi, rd_lo, rd_hi;
      cvt_epu8_epi16(orig, &orig_lo, &orig_hi);
      cvt_epu8_epi16(rd,   &rd_lo,   &rd_hi);

      // The shift will clamp band to 0...31; band_pos on the other
      // hand is always between 0...32, so band will be -1...31. Anything
      // below zero is ignored, so we can clamp band_pos to 32.
      __m256i rd_divd      = srli_epi8           (rd,            shift);
      __m256i band         = _mm256_sub_epi8     (rd_divd,       bp_256);

      // Force all <0 or >3 bands to 0xff, which will zero the shuffle result
      __m256i band_lt_0    = _mm256_cmpgt_epi8   (zero,          band);
      __m256i band_gt_3    = _mm256_cmpgt_epi8   (band,          threes);
      __m256i band_inv     = _mm256_or_si256     (band_lt_0,     band_gt_3);

              band         = _mm256_or_si256     (band,          band_inv);

      __m256i offsets      = _mm256_shuffle_epi8 (sb_256,        band);

      __m256i offsets_lo, offsets_hi;
      cvt_epi8_epi16(offsets, &offsets_lo, &offsets_hi);

      __m256i offsets_0_lo = _mm256_cmpeq_epi16   (offsets_lo,   zero);
      __m256i offsets_0_hi = _mm256_cmpeq_epi16   (offsets_hi,   zero);

      __m256i diff_lo      = _mm256_sub_epi16     (orig_lo,      rd_lo);
      __m256i diff_hi      = _mm256_sub_epi16     (orig_hi,      rd_hi);

      __m256i delta_lo     = _mm256_sub_epi16     (diff_lo,      offsets_lo);
      __m256i delta_hi     = _mm256_sub_epi16     (diff_hi,      offsets_hi);

              diff_lo      = _mm256_andnot_si256  (offsets_0_lo, diff_lo);
              diff_hi      = _mm256_andnot_si256  (offsets_0_hi, diff_hi);
              delta_lo     = _mm256_andnot_si256  (offsets_0_lo, delta_lo);
              delta_hi     = _mm256_andnot_si256  (offsets_0_hi, delta_hi);

      __m256i dd0_lo       = _mm256_unpacklo_epi16(delta_lo,     diff_lo);
      __m256i dd0_hi       = _mm256_unpackhi_epi16(delta_lo,     diff_lo);
      __m256i dd1_lo       = _mm256_unpacklo_epi16(delta_hi,     diff_hi);
      __m256i dd1_hi       = _mm256_unpackhi_epi16(delta_hi,     diff_hi);

      __m256i dd0_lo_n     = _mm256_sign_epi16    (dd0_lo,       negate_hiword);
      __m256i dd0_hi_n     = _mm256_sign_epi16    (dd0_hi,       negate_hiword);
      __m256i dd1_lo_n     = _mm256_sign_epi16    (dd1_lo,       negate_hiword);
      __m256i dd1_hi_n     = _mm256_sign_epi16    (dd1_hi,       negate_hiword);

      __m256i sum0_lo      = _mm256_madd_epi16    (dd0_lo,       dd0_lo_n);
      __m256i sum0_hi      = _mm256_madd_epi16    (dd0_hi,       dd0_hi_n);
      __m256i sum1_lo      = _mm256_madd_epi16    (dd1_lo,       dd1_lo_n);
      __m256i sum1_hi      = _mm256_madd_epi16    (dd1_hi,       dd1_hi_n);

      __m256i sum0         = _mm256_add_epi32     (sum0_lo,      sum0_hi);
      __m256i sum1         = _mm256_add_epi32     (sum1_lo,      sum1_hi);
      __m256i curr_sum     = _mm256_add_epi32     (sum0,         sum1);

              sum          = _mm256_add_epi32     (sum,          curr_sum);
    }
  }
  // Horizontal sum of 8x32 YMM, nothing special here
  __m256i sum2 = _mm256_permute4x64_epi64(sum,  _MM_SHUFFLE(1, 0, 3, 2));
  __m256i sum3 = _mm256_add_epi32        (sum,  sum2);
  __m256i sum4 = _mm256_shuffle_epi32    (sum3, _MM_SHUFFLE(1, 0, 3, 2));
  __m256i sum5 = _mm256_add_epi32        (sum3, sum4);
  __m256i sum6 = _mm256_shuffle_epi32    (sum5, _MM_SHUFFLE(2, 3, 0, 1));
  __m256i sum7 = _mm256_add_epi32        (sum5, sum6);

  __m128i sum8 = _mm256_castsi256_si128  (sum7);
  int32_t sum9 = _mm_cvtsi128_si32       (sum8);
  return  sum9;

use_generic:
  return sao_band_ddistortion_generic(state, orig_data, rec_data, block_width,
      block_height, band_pos, sao_bands);
}

#endif //COMPILE_INTEL_AVX2

int kvz_strategy_register_sao_avx2(void* opaque, uint8_t bitdepth)
{
  bool success = true;
#if COMPILE_INTEL_AVX2
  if (bitdepth == 8) {
    //success &= kvz_strategyselector_register(opaque, "sao_edge_ddistortion", "avx2", 40, &sao_edge_ddistortion_avx2);
    success &= kvz_strategyselector_register(opaque, "calc_sao_edge_dir", "avx2", 40, &calc_sao_edge_dir_avx2);
    success &= kvz_strategyselector_register(opaque, "sao_reconstruct_color", "avx2", 40, &sao_reconstruct_color_avx2);
    success &= kvz_strategyselector_register(opaque, "sao_band_ddistortion", "avx2", 40, &sao_band_ddistortion_avx2);
  }
#endif //COMPILE_INTEL_AVX2
  return success;
}
