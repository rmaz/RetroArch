/*  SSNES - A Super Nintendo Entertainment System (SNES) Emulator frontend for libsnes.
 *  Copyright (C) 2010-2012 - Hans-Kristian Arntzen
 *
 *  Some code herein may be based on code found in BSNES.
 * 
 *  SSNES is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  SSNES is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with SSNES.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

// Bog-standard windowed SINC implementation.
// Only suitable as an upsampler, as there is no low-pass filter stage.

#include "resampler.h"
#include "../general.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#if __SSE__
#include <xmmintrin.h>
#endif

#if __SSE3__
#include <pmmintrin.h>
#endif

#define PHASE_BITS 8
#define SUBPHASE_BITS 16

#define PHASES (1 << PHASE_BITS)
#define PHASES_SHIFT (SUBPHASE_BITS)
#define SUBPHASES (1 << SUBPHASE_BITS)
#define SUBPHASES_SHIFT 0
#define SUBPHASES_MASK ((1 << SUBPHASE_BITS) - 1)
#define PHASES_WRAP (1 << (PHASE_BITS + SUBPHASE_BITS))
#define FRAMES_SHIFT (PHASE_BITS + SUBPHASE_BITS)

#define SIDELOBES 8
#define TAPS (SIDELOBES * 2)

#define PHASE_INDEX 0
#define DELTA_INDEX 1

struct ssnes_resampler
{
   float phase_table[PHASES][2][2 * SIDELOBES];
   float buffer_l[2 * SIDELOBES];
   float buffer_r[2 * SIDELOBES];

   uint32_t time;
};

static inline double sinc(double val)
{
   if (fabs(val) < 0.00001)
      return 1.0;
   else
      return sin(val) / val;
}

static inline double blackman(double index)
{
   index *= 0.5;
   index += 0.5;

   double alpha = 0.16;
   double a0 = (1.0 - alpha) / 2.0;
   double a1 = 0.5;
   double a2 = alpha / 2.0;

   return a0 - a1 * cos(2.0 * M_PI * index) + a2 * cos(4.0 * M_PI * index);
}

static void init_sinc_table(ssnes_resampler_t *resamp)
{
   // Sinc phases: [..., p + 3, p + 2, p + 1, p + 0, p - 1, p - 2, p - 3, p - 4, ...]
   for (int i = 0; i < PHASES; i++)
   {
      for (int j = 0; j < 2 * SIDELOBES; j++)
      {
         double p = (double)i / PHASES;
         double sinc_phase = M_PI * (p + (SIDELOBES - 1 - j));
         resamp->phase_table[i][PHASE_INDEX][j] = sinc(sinc_phase) * blackman(sinc_phase / SIDELOBES);
      }
   }

   // Optimize linear interpolation.
   for (int i = 0; i < PHASES - 1; i++)
   {
      for (int j = 0; j < 2 * SIDELOBES; j++)
      {
         resamp->phase_table[i][DELTA_INDEX][j] =
            (resamp->phase_table[i + 1][PHASE_INDEX][j] - resamp->phase_table[i][PHASE_INDEX][j]) / SUBPHASES;
      }
   }
}

ssnes_resampler_t *resampler_new(void)
{
   ssnes_resampler_t *re = (ssnes_resampler_t*)memalign(16, sizeof(*re));
   if (!re)
      return NULL;

   memset(re, 0, sizeof(*re));

   init_sinc_table(re);

#if __SSE3__
   SSNES_LOG("Sinc resampler [SSE3]\n");
#elif __SSE__
   SSNES_LOG("Sinc resampler [SSE]\n");
#else
   SSNES_LOG("Sinc resampler [C]\n");
#endif

   return re;
}

#if __SSE__
static void process_sinc(ssnes_resampler_t *resamp, float *out_buffer)
{
   __m128 sum_l = _mm_setzero_ps();
   __m128 sum_r = _mm_setzero_ps();

   const float *buffer_l = resamp->buffer_l;
   const float *buffer_r = resamp->buffer_r;

   unsigned phase = resamp->time >> PHASES_SHIFT;
   unsigned delta = (resamp->time >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   __m128 delta_f = _mm_set1_ps(delta);

   const float *phase_table = resamp->phase_table[phase][PHASE_INDEX];
   const float *delta_table = resamp->phase_table[phase][DELTA_INDEX];

   for (unsigned i = 0; i < TAPS; i += 4)
   {
      __m128 buf_l  = _mm_load_ps(buffer_l + i);
      __m128 buf_r  = _mm_load_ps(buffer_r + i);

      __m128 phases = _mm_load_ps(phase_table + i);
      __m128 deltas = _mm_load_ps(delta_table + i);

      __m128 sinc   = _mm_add_ps(phases, _mm_mul_ps(deltas, delta_f));

      sum_l         = _mm_add_ps(sum_l, _mm_mul_ps(buf_l, sinc));
      sum_r         = _mm_add_ps(sum_r, _mm_mul_ps(buf_r, sinc));
   }

#if __SSE3__
   __m128 res = _mm_hadd_ps(_mm_hadd_ps(sum_l, sum_r), _mm_setzero_ps());
   _mm_storeu_ps(out_buffer, res); // Overwriting, but this is safe.
#else // Meh, compiler should optimize this to something sane.
   union
   {
      float f[4];
      __m128 v;
   } u[2] = {
      [0] = { .v = sum_l },
      [1] = { .v = sum_r },
   };

   out_buffer[0] = u[0].f[0] + u[0].f[1] + u[0].f[2] + u[0].f[3];
   out_buffer[1] = u[1].f[0] + u[1].f[1] + u[1].f[2] + u[1].f[3];
#endif
}
#else // Plain ol' C99
static void process_sinc(ssnes_resampler_t *resamp, float *out_buffer)
{
   float sum_l = 0.0f;
   float sum_r = 0.0f;
   const float *buffer_l = resamp->buffer_l;
   const float *buffer_r = resamp->buffer_r;

   unsigned phase = resamp->time >> PHASES_SHIFT;
   unsigned delta = (resamp->time >> SUBPHASES_SHIFT) & SUBPHASES_MASK;
   float delta_f = (float)delta;

   const float *phase_table = resamp->phase_table[phase][PHASE_INDEX];
   const float *delta_table = resamp->phase_table[phase][DELTA_INDEX];

   for (unsigned i = 0; i < TAPS; i++)
   {
      float sinc_val = phase_table[i] + delta_f * delta_table[i];
      sum_l         += buffer_l[i] * sinc_val;
      sum_r         += buffer_r[i] * sinc_val;
   }

   out_buffer[0] = sum_l;
   out_buffer[1] = sum_r;
}
#endif

void resampler_process(ssnes_resampler_t *re, struct resampler_data *data)
{
   uint32_t ratio = PHASES_WRAP / data->ratio;

   const float *input = data->data_in;
   float *output = data->data_out;
   size_t frames = data->input_frames;
   size_t out_frames = 0;

   while (frames)
   {
      process_sinc(re, output);
      output += 2;
      out_frames++;

      re->time += ratio;
      while (re->time >= PHASES_WRAP)
      {
         memmove(re->buffer_l, re->buffer_l + 1,
               sizeof(re->buffer_l) - sizeof(float));
         memmove(re->buffer_r, re->buffer_r + 1,
               sizeof(re->buffer_r) - sizeof(float));

         re->buffer_l[2 * SIDELOBES - 1] = *input++;
         re->buffer_r[2 * SIDELOBES - 1] = *input++;

         re->time -= PHASES_WRAP;
         frames--;
      }
   }

   data->output_frames = out_frames;
}

void resampler_free(ssnes_resampler_t *re)
{
   free(re);
}

