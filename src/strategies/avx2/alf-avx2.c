/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (C) 2013-2021 Tampere University of Technology and others (see
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

#include "global.h"

#include "strategies/avx2/alf-avx2.h"

#if COMPILE_INTEL_AVX2
#include "kvazaar.h"
#if KVZ_BIT_DEPTH == 8

#include <immintrin.h>
#include <stdlib.h>

#include "strategyselector.h"

static int16_t clip_alf(const int16_t clip, const int16_t ref, const int16_t val0, const int16_t val1)
{
  return CLIP(-clip, +clip, val0 - ref) + CLIP(-clip, +clip, val1 - ref);
}


static void alf_calc_covariance_avx2(int16_t e_local[MAX_NUM_ALF_LUMA_COEFF][MAX_ALF_NUM_CLIPPING_VALUES],
  const kvz_pixel* rec,
  const int stride,
  const channel_type channel,
  const int transpose_idx,
  int vb_distance,
  short alf_clipping_values[MAX_NUM_CHANNEL_TYPE][MAX_ALF_NUM_CLIPPING_VALUES])
{
  static const int alf_pattern_5[13] = {
              0,
          1,  2,  3,
      4,  5,  6,  5,  4,
          3,  2,  1,
              0
  };

  static const int alf_pattern_7[25] = {
                0,
            1,  2,  3,
        4,  5,  6,  7,  8,
    9, 10, 11, 12, 11, 10, 9,
        8,  7,  6,  5,  4,
            3,  2,  1,
                0
  };

  int clip_top_row = -4;
  int clip_bot_row = 4;
  if (vb_distance >= -3 && vb_distance < 0)
  {
    clip_bot_row = -vb_distance - 1;
    clip_top_row = -clip_bot_row; // symmetric
  }
  else if (vb_distance >= 0 && vb_distance < 3)
  {
    clip_top_row = -vb_distance;
    clip_bot_row = -clip_top_row; // symmetric
  }

  const bool is_luma = channel == CHANNEL_TYPE_LUMA;
  const int* filter_pattern = is_luma ? alf_pattern_7 : alf_pattern_5;
  const int half_filter_length = (is_luma ? 7 : 5) >> 1;
  const short* clip = alf_clipping_values[channel];
  const int num_bins = MAX_ALF_NUM_CLIPPING_VALUES;

  int k = 0;

  const int16_t curr = rec[0];

  const __m128i negate = _mm_setr_epi16(-1, -1, -1, -1, -1, -1, -1, -1);

  if (transpose_idx == 0)
  {
    for (int i = -half_filter_length; i < 0; i++)
    {
      const kvz_pixel* rec0 = rec + MAX(i, clip_top_row) * stride;
      const kvz_pixel* rec1 = rec - MAX(i, -clip_bot_row) * stride;
      for (int j = -half_filter_length - i; j <= half_filter_length + i; j++, k++)
      {
        __m128i clips = _mm_loadl_epi64((__m128i*) clip);
        __m128i neg_clips = _mm_sign_epi16(clips, negate);
        __m128i val0 = _mm_set1_epi16((rec0[j] - curr));
        __m128i val1 = _mm_set1_epi16((rec1[-j] - curr));          
        __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
        __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);
          
        __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
        __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

        __m128i e_local_original = _mm_loadl_epi64((__m128i*) &e_local[filter_pattern[k]][0]);          
        __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
        _mm_storel_epi64((__m128i*) & e_local[filter_pattern[k]][0], result);
      }
    }
    for (int j = -half_filter_length; j < 0; j++, k++)
    {
      __m128i clips = _mm_loadl_epi64((__m128i*) clip);
      __m128i neg_clips = _mm_sign_epi16(clips, negate);
      __m128i val0 = _mm_set1_epi16((rec[j] - curr));
      __m128i val1 = _mm_set1_epi16((rec[-j] - curr));
      __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
      __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);

      __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
      __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

      __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
      __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
      _mm_storel_epi64((__m128i*) & e_local[filter_pattern[k]][0], result);
    }
  }
  else if (transpose_idx == 1)
  {
    for (int j = -half_filter_length; j < 0; j++)
    {
      const kvz_pixel* rec0 = rec + j;
      const kvz_pixel* rec1 = rec - j;

      for (int i = -half_filter_length - j; i <= half_filter_length + j; i++, k++)
      {
        __m128i clips = _mm_loadl_epi64((__m128i*) clip);
        __m128i neg_clips = _mm_sign_epi16(clips, negate);
        __m128i val0 = _mm_set1_epi16((rec0[MAX(i, clip_top_row) * stride] - curr));
        __m128i val1 = _mm_set1_epi16((rec1[-MAX(i, -clip_bot_row) * stride] - curr));
        __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
        __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);

        __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
        __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

        __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
        __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
        _mm_storel_epi64((__m128i*) & e_local[filter_pattern[k]][0], result);
      }
    }
    for (int i = -half_filter_length; i < 0; i++, k++)
    {
      __m128i clips = _mm_loadl_epi64((__m128i*) clip);
      __m128i neg_clips = _mm_sign_epi16(clips, negate);
      __m128i val0 = _mm_set1_epi16((rec[MAX(i, clip_top_row) * stride] - curr));
      __m128i val1 = _mm_set1_epi16((rec[-MAX(i, -clip_bot_row) * stride] - curr));
      __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
      __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);

      __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
      __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

      __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
      __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
      _mm_storel_epi64((__m128i*)& e_local[filter_pattern[k]][0], result);
    }
  }
  else if (transpose_idx == 2)
  {
    for (int i = -half_filter_length; i < 0; i++)
    {
      const kvz_pixel* rec0 = rec + MAX(i, clip_top_row) * stride;
      const kvz_pixel* rec1 = rec - MAX(i, -clip_bot_row) * stride;

      for (int j = half_filter_length + i; j >= -half_filter_length - i; j--, k++)
      {
        __m128i clips = _mm_loadl_epi64((__m128i*) clip);
        __m128i neg_clips = _mm_sign_epi16(clips, negate);
        __m128i val0 = _mm_set1_epi16((rec0[j] - curr));
        __m128i val1 = _mm_set1_epi16((rec1[-j] - curr));
        __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
        __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);

        __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
        __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

        __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
        __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
        _mm_storel_epi64((__m128i*) & e_local[filter_pattern[k]][0], result);
      }
    }
    for (int j = -half_filter_length; j < 0; j++, k++)
    {
      __m128i clips = _mm_loadl_epi64((__m128i*) clip);
      __m128i neg_clips = _mm_sign_epi16(clips, negate);
      __m128i val0 = _mm_set1_epi16((rec[j] - curr));
      __m128i val1 = _mm_set1_epi16((rec[-j] - curr));
      __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
      __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);

      __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
      __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

      __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
      __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
      _mm_storel_epi64((__m128i*)& e_local[filter_pattern[k]][0], result);
    }
  }
  else
  {
    for (int j = -half_filter_length; j < 0; j++)
    {
      const kvz_pixel* rec0 = rec + j;
      const kvz_pixel* rec1 = rec - j;

      for (int i = half_filter_length + j; i >= -half_filter_length - j; i--, k++)
      {
        __m128i clips = _mm_loadl_epi64((__m128i*) clip);
        __m128i neg_clips = _mm_sign_epi16(clips, negate);
        __m128i val0 = _mm_set1_epi16((rec0[MAX(i, clip_top_row) * stride] - curr));
        __m128i val1 = _mm_set1_epi16((rec1[-MAX(i, -clip_bot_row) * stride] - curr));
        __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
        __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);

        __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
        __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

        __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
        __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
        _mm_storel_epi64((__m128i*)& e_local[filter_pattern[k]][0], result);
      }
    }
    for (int i = -half_filter_length; i < 0; i++, k++)
    {
      __m128i clips = _mm_loadl_epi64((__m128i*) clip);
      __m128i neg_clips = _mm_sign_epi16(clips, negate);
      __m128i val0 = _mm_set1_epi16((rec[MAX(i, clip_top_row) * stride] - curr));
      __m128i val1 = _mm_set1_epi16((rec[-MAX(i, -clip_bot_row) * stride] - curr));
      __m128i min_clips_val0 = _mm_min_epi16(clips, val0);
      __m128i max_clips_val0 = _mm_max_epi16(min_clips_val0, neg_clips);

      __m128i min_clips_val1 = _mm_min_epi16(clips, val1);
      __m128i max_clips_val1 = _mm_max_epi16(min_clips_val1, neg_clips);

      __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
      __m128i result = _mm_add_epi16(e_local_original, _mm_add_epi16(max_clips_val0, max_clips_val1));
      _mm_storel_epi64((__m128i*)& e_local[filter_pattern[k]][0], result);
    }
  }

  __m128i e_local_original = _mm_loadl_epi64((__m128i*) & e_local[filter_pattern[k]][0]);
  __m128i result = _mm_add_epi16(e_local_original, _mm_set1_epi16(curr));
  _mm_storel_epi64((__m128i*)& e_local[filter_pattern[k]][0], result);

}

static void alf_get_blk_stats_avx2(encoder_state_t* const state,
  channel_type channel,
  alf_covariance* alf_covariance,
  alf_classifier** g_classifier,
  kvz_pixel* org,
  int32_t org_stride,
  kvz_pixel* rec,
  int32_t rec_stride,
  const int x_pos,
  const int y_pos,
  const int x_dst,
  const int y_dst,
  const int width,
  const int height,
  int vb_ctu_height,
  int vb_pos,
  short alf_clipping_values[MAX_NUM_CHANNEL_TYPE][MAX_ALF_NUM_CLIPPING_VALUES])
{
  int16_t e_local[MAX_NUM_ALF_LUMA_COEFF][MAX_ALF_NUM_CLIPPING_VALUES];

  const int num_bins = MAX_ALF_NUM_CLIPPING_VALUES;

  int num_coeff = channel == CHANNEL_TYPE_LUMA ? 13 : 7;
  int transpose_idx = 0;
  int class_idx = 0;

  for (int i = 0; i < height; i++)
  {
    int vb_distance = ((y_dst + i) % vb_ctu_height) - vb_pos;
    for (int j = 0; j < width; j++)
    {
      if (g_classifier && g_classifier[y_dst + i][x_dst + j].class_idx == ALF_UNUSED_CLASS_IDX && g_classifier[y_dst + i][x_dst + j].transpose_idx == ALF_UNUSED_TRANSPOSE_IDX)
      {
        continue;
      }
      memset(e_local, 0, sizeof(e_local));
      if (g_classifier)
      {
        alf_classifier* cl = &g_classifier[y_dst + i][x_dst + j];
        transpose_idx = cl->transpose_idx;
        class_idx = cl->class_idx;
      }

      int16_t y_local = org[j] - rec[j];

      __m256d y_local_d = _mm256_set1_pd((double)y_local);
      alf_calc_covariance_avx2(e_local, rec + j, rec_stride, channel, transpose_idx, vb_distance, alf_clipping_values);
      for (int k = 0; k < num_coeff; k++)
      {
        for (int l = k; l < num_coeff; l++)
        {
          for (int b0 = 0; b0 < 4; b0++)
          {
            __m256d e_local_b0_d = _mm256_set1_pd((double)e_local[k][b0]);
            //for (int b1 = 0; b1 < 4; b1++)
            {

              //__m256d _mm256_fmadd_pd (__m256d a, __m256d b, __m256d c)
              __m128i e_local_1 = _mm_loadl_epi64((__m128i*) & e_local[l][0]);
              __m128i e_local_32 = _mm_cvtepi16_epi32(e_local_1);
              __m256d e_local_b1_d = _mm256_cvtepi32_pd(e_local_32);
              
              __m256d multiplied = _mm256_mul_pd(e_local_b0_d, e_local_b1_d);
              double data[4];
              _mm256_store_pd(data, multiplied);

              //alf_covariance[class_idx].ee[b0][b1][k][l] += e_local[k][b0] * (double)e_local[l][b1];
              alf_covariance[class_idx].ee[b0][0][k][l] += data[0];
              alf_covariance[class_idx].ee[b0][1][k][l] += data[1];
              alf_covariance[class_idx].ee[b0][2][k][l] += data[2];
              alf_covariance[class_idx].ee[b0][3][k][l] += data[3];
            }
          }
        }
        //for (int b = 0; b < 4; b++)
        {
          __m128i e_local_1 = _mm_loadl_epi64((__m128i*) & e_local[k][0]);
          __m128i e_local_32 = _mm_cvtepi16_epi32(e_local_1);
          __m256d e_local_b1_d = _mm256_cvtepi32_pd(e_local_32);

          __m256d multiplied = _mm256_mul_pd(y_local_d, e_local_b1_d);

          __m128i output = _mm256_cvtpd_epi32(multiplied);

          int32_t data[4];
          _mm_storeu_si128((__m128i*)data, output);
          //alf_covariance[class_idx].y[b][k] += e_local[k][b] * (double)y_local;
          alf_covariance[class_idx].y[0][k] += data[0];
          alf_covariance[class_idx].y[1][k] += data[1];
          alf_covariance[class_idx].y[2][k] += data[2];
          alf_covariance[class_idx].y[3][k] += data[3];
        }
      }
      alf_covariance[class_idx].pix_acc += y_local * (double)y_local;
    }
    org += org_stride;
    rec += rec_stride;
  }

  int num_classes = g_classifier ? MAX_NUM_ALF_CLASSES : 1;
  for (class_idx = 0; class_idx < num_classes; class_idx++)
  {
    for (int k = 1; k < num_coeff; k++)
    {
      for (int l = 0; l < k; l++)
      {
        for (int b0 = 0; b0 < 4; b0++)
        {
          for (int b1 = 0; b1 < 4; b1++)
          {
            alf_covariance[class_idx].ee[b0][b1][k][l] = alf_covariance[class_idx].ee[b1][b0][l][k];
          }
        }
      }
    }
  }
}

#endif // KVZ_BIT_DEPTH == 8
#endif //COMPILE_INTEL_AVX2


int kvz_strategy_register_alf_avx2(void* opaque, uint8_t bitdepth) {
  bool success = true;
#if COMPILE_INTEL_AVX2
#if KVZ_BIT_DEPTH == 8
  if (bitdepth == 8){
    success &= kvz_strategyselector_register(opaque, "alf_get_blk_stats", "generic", 0, &alf_get_blk_stats_avx2);
  }
#endif // KVZ_BIT_DEPTH == 8
#endif
  return success;
}
