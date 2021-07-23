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
 * DESCRIPTION: WaveFront aligner data structure attributes
 */

#include "WFA/wavefront/wavefront_attributes.h"


#ifdef WFA_NAMESPACE
namespace wfa {
#endif


/*
 * Default parameters
 */
wavefront_aligner_attr_t wavefront_aligner_attr_default = {
    // Distance model & Penalties
    .distance_metric = gap_affine, // TODO: DU wants lineal
    .alignment_scope = compute_alignment, // TODO: I don't know what DU wants, honestly
    .alignment_form = {
        .span = alignment_end2end,
        .pattern_begin_free = 0,
        .pattern_end_free = 0,
        .text_begin_free = 0,
        .text_end_free = 0,
        .max_alignment_score = WF_MAX_SCORE, // Unlimited
    },
    // Penalties
    .lineal_penalties = {
        .match = 0,
        .mismatch = 4,
        .insertion = 2,
        .deletion  = 2,
    },
    .affine_penalties = {
        .match = 0,
        .mismatch = 4,
        .gap_opening = 6,
        .gap_extension = 2, // 1
    },
    .affine2p_penalties = {
        .match = 0,
        .mismatch = 4,
        .gap_opening1 = 6,
        .gap_extension1 = 2,
        .gap_opening2 = 24,
        .gap_extension2 = 1,
    },
    // Reduction
    .reduction = {
        .reduction_strategy = wavefront_reduction_adaptive,
        .min_wavefront_length = 10,
        .max_distance_threshold = 50,
    },
    // Memory model
    .low_memory = false,
    // MM
    .mm_allocator = NULL, // Use private MM
    // Limits
    .max_memory_used = WF_MAX_MEMORY_DEFAULT // Unlimited
};

#ifdef WFA_NAMESPACE
}
#endif