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
 * DESCRIPTION: WaveFront-Alignment module for display and report
 */

#ifndef WAVEFRONT_DISPLAY_H_
#define WAVEFRONT_DISPLAY_H_

#include "utils/commons.h"
#include "utils/heatmap.h"
#include "system/profiler_timer.h"

// Wavefront ahead definition
typedef struct _wavefront_aligner_t wavefront_aligner_t;

/*
 * Display
 */
void wavefront_aligner_print(
    FILE* const stream,
    wavefront_aligner_t* const wf_aligner,
    const int score_begin,
    const int score_end,
    const int num_wfs_per_row,
    const int backtrace_length);

/*
 * Debug
 */
void wavefront_report_lite(
    FILE* const stream,
    wavefront_aligner_t* const wf_aligner,
    const char* const pattern,
    const int pattern_length,
    const char* const text,
    const int text_length,
    const int wf_status,
    const uint64_t wf_memory_used,
    profiler_timer_t* const timer);
void wavefront_report_verbose_begin(
    FILE* const stream,
    wavefront_aligner_t* const wf_aligner,
    const char* const pattern,
    const int pattern_length,
    const char* const text,
    const int text_length);
void wavefront_report_verbose_end(
    FILE* const stream,
    wavefront_aligner_t* const wf_aligner,
    const int wf_status,
    const uint64_t wf_memory_used,
    profiler_timer_t* const timer);

#endif /* WAVEFRONT_DISPLAY_H_ */
