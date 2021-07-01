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
 * DESCRIPTION: WaveFront Slab for fast pre-allocated wavefronts' memory handling
 */

#pragma once

#include "wflambda/utils/commons.h"
#include "wflambda/utils/vector.h"
#include "wflambda/system/mm_allocator.h"
#include "wflambda/wavefront/wavefront.h"
#include "wflambda/wavefront/wavefront.h"

#ifdef WFLAMBDA_NAMESPACE
namespace wflambda {
#endif

/*
 * Memory Manager for Wavefront
 */
typedef struct {
  // Attributes
  bool allocate_backtrace;
  // Wavefront Slabs
  int max_wavefront_elements;   // Maximum wf-elements allocated (max. wf. size)
  vector_t* wavefronts;         // All wavefronts (wavefront_t*)
  vector_t* wavefronts_free;    // Free wavefronts (wavefront_t*)
  // MM
  mm_allocator_t* mm_allocator; // MM-Allocator
} wavefront_slab_t;

/*
 * Setup
 */
wavefront_slab_t* wavefront_slab_new(
    const int init_max_wavefront_elements,
    const bool allocate_backtrace,
    mm_allocator_t* const mm_allocator);
void wavefront_slab_resize(
    wavefront_slab_t* const wavefront_slab,
    const int max_wavefront_elements);
void wavefront_slab_clear(
    wavefront_slab_t* const wavefront_slab,
    const bool deallocate_oversized);
void wavefront_slab_delete(
    wavefront_slab_t* const wavefront_slab);

/*
 * Allocator
 */
wavefront_t* wavefront_slab_allocate(
    wavefront_slab_t* const wavefront_slab,
    const int lo,
    const int hi);
void wavefront_slab_free(
    wavefront_slab_t* const wavefront_slab,
    wavefront_t* const wavefront);

#ifdef WFLAMBDA_NAMESPACE
}
#endif
