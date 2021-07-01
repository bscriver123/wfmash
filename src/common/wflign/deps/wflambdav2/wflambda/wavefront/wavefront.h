/*
 *                             The MIT License
 *
 * Wavefront Alignments Algorithms
 * Copyright (c) 2017 by Santiago Marco-Sola  <santiagomsola@gmail.com>
 *
 * This file is part of Wavefront Alignments Algorithms.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * PROJECT: Wavefront Alignments Algorithms
 * AUTHOR(S): Santiago Marco-Sola <santiagomsola@gmail.com>
 * DESCRIPTION: Individual WaveFront data structure
 */

#pragma once

#include "wflambda/utils/commons.h"
#include "wflambda/system/mm_allocator.h"
#include "wflambda/wavefront/wavefront_backtrace_buffer.h"

#ifdef WFLAMBDA_NAMESPACE
namespace wflambda {
#endif

/*
 * Wavefront Offset
 */
typedef int32_t wf_offset_t;

/*
 * Constants
 */
#define WAVEFRONT_OFFSET_NULL (INT32_MIN/2) // TODO (+1 for I/D) Silently increases NULL offsets

/*
 * Translate k and offset to coordinates h,v
 */
#define WAVEFRONT_V(k,offset) ((offset)-(k))
#define WAVEFRONT_H(k,offset) (offset)

#define WAVEFRONT_DIAGONAL(h,v) ((h)-(v))
#define WAVEFRONT_OFFSET(h,v)   (h)

#define WAVEFRONT_LENGTH(lo,hi) ((hi)-(lo)+2)

/*
 * Wavefront
 */
typedef enum {
  wavefront_status_free,
  wavefront_status_busy,
  wavefront_status_deallocated,
} wavefront_status_type;
typedef struct {
  // Dimensions
  bool null;                           // Is null interval?
  int lo;                              // Effective lowest diagonal (inclusive)
  int hi;                              // Effective highest diagonal (inclusive)
  int lo_base;                         // Lowest diagonal before reduction (inclusive)
  int hi_base;                         // Highest diagonal before reduction (inclusive)
  // Wavefront elements
  wf_offset_t* offsets;                // Offsets (k-centered)
  pcigar_t* bt_pcigar;                 // Backtrace-block (k-centered)
  block_idx_t* bt_prev;                // Backtrace-block previous index (k-centered)
  // Internals
  wavefront_status_type status;        // Wavefront status (memory state)
  int max_wavefront_elements;          // Maximum wf-elements allocated (max. wf. size)
  wf_offset_t* offsets_mem;            // Offsets base memory
  pcigar_t* bt_pcigar_mem;             // Backtrace-block (base memory)
  block_idx_t* bt_prev_mem;            // Backtrace-block previous index (base memory)
} wavefront_t;

/*
 * Wavefront Set
 */
typedef struct {
  /* In Wavefronts*/
  wavefront_t* in_mwavefront_sub;
  wavefront_t* in_mwavefront_gap1;
  wavefront_t* in_mwavefront_gap2;
  wavefront_t* in_i1wavefront_ext;
  wavefront_t* in_i2wavefront_ext;
  wavefront_t* in_d1wavefront_ext;
  wavefront_t* in_d2wavefront_ext;
  /* Out Wavefronts */
  wavefront_t* out_mwavefront;
  wavefront_t* out_i1wavefront;
  wavefront_t* out_i2wavefront;
  wavefront_t* out_d1wavefront;
  wavefront_t* out_d2wavefront;
} wavefront_set_t;

/*
 * Setup
 */
void wavefront_allocate(
    wavefront_t* const wavefront,
    const int max_wavefront_elements,
    const bool allocate_backtrace,
    mm_allocator_t* const mm_allocator);
void wavefront_resize(
    wavefront_t* const wavefront,
    const int max_wavefront_elements,
    mm_allocator_t* const mm_allocator);
void wavefront_free(
    wavefront_t* const wavefront,
    mm_allocator_t* const mm_allocator);

/*
 * Initialization
 */
void wavefront_init(
    wavefront_t* const wavefront,
    const int lo,
    const int hi);
void wavefront_init_null(
    wavefront_t* const wavefront,
    const int lo,
    const int hi);
void wavefront_init_victim(
    wavefront_t* const wavefront,
    const int lo,
    const int hi);

#ifdef WFLAMBDA_NAMESPACE
}
#endif
