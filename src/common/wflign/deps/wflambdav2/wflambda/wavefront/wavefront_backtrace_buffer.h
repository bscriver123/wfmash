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
 * DESCRIPTION: WaveFront backtrace buffer to store bactrace-blocks
 */

#pragma once

#include "wflambda/utils/commons.h"
#include "wflambda/utils/vector.h"
#include "wflambda/system/mm_allocator.h"
#include "wflambda/alignment/cigar.h"
#include "wflambda/wavefront/wavefront_pcigar.h"

#ifdef WFLAMBDA_NAMESPACE
namespace wflambda {
#endif

/*
 * Backtrace Block
 */
typedef uint32_t block_idx_t;
typedef struct {
  pcigar_t pcigar;
  block_idx_t prev_idx;
} __attribute__((packed)) wf_backtrace_block_t;

/*
 * Backtrace Buffer
 */
typedef struct {
  // Current pointer
  int segment_idx; // Current segment idx
  int segment_pos; // Current free position within segment
  // Buffer
  vector_t* segments;   // Memory segments (wf_backtrace_block_t*)
  vector_t* palignment; // Temporal buffer to store final alignment (pcigar_t)
  // MM
  mm_allocator_t* mm_allocator;
} wf_backtrace_buffer_t;

/*
 * Setup
 */
wf_backtrace_buffer_t* wf_backtrace_buffer_new(mm_allocator_t* const mm_allocator);
void wf_backtrace_buffer_clear(
    wf_backtrace_buffer_t* const bt_buffer);
void wf_backtrace_buffer_delete(
    wf_backtrace_buffer_t* const bt_buffer);

/*
 * Store blocks
 */
void wf_backtrace_buffer_store_block(
    wf_backtrace_buffer_t* const bt_buffer,
    pcigar_t* const pcigar,
    block_idx_t* const prev_idx);

/*
 * Recover CIGAR
 */
void wf_backtrace_buffer_recover_cigar(
    wf_backtrace_buffer_t* const bt_buffer,
    const pcigar_t pcigar_last,
    const block_idx_t prev_idx_last,
    const std::function<bool(const int&, const int&)>& traceback_lambda,
    const int pattern_length,
    const int text_length,
    cigar_t* const cigar);

#ifdef WFLAMBDA_NAMESPACE
}
#endif
