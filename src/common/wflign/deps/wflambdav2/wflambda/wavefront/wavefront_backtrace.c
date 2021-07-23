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
 * DESCRIPTION: WaveFront-Alignment module for backtracing alignments
 */

#include <functional>
#include "wflambda/wavefront/wavefront_backtrace.h"
#include "wflambda/gap_affine2p/affine2p_matrix.h"

#ifdef WFLAMBDA_NAMESPACE
namespace wflambda {
#endif

/*
 * Wavefront type
 */
#define BACKTRACE_TYPE_BITS                   4 // 4-bits for piggyback
#define BACKTRACE_TYPE_MASK 0x000000000000000Fl // Extract mask

#define BACKTRACE_PIGGYBACK_SET(offset,backtrace_type) \
  (( ((int64_t)(offset)) << BACKTRACE_TYPE_BITS) | backtrace_type)

#define BACKTRACE_PIGGYBACK_GET_TYPE(offset) \
  ((offset) & BACKTRACE_TYPE_MASK)
#define BACKTRACE_PIGGYBACK_GET_OFFSET(offset) \
  ((offset) >> BACKTRACE_TYPE_BITS)

typedef enum {
  backtrace_M       = 9,
  backtrace_D2_ext  = 8,
  backtrace_D2_open = 7,
  backtrace_D1_ext  = 6,
  backtrace_D1_open = 5,
  backtrace_I2_ext  = 4,
  backtrace_I2_open = 3,
  backtrace_I1_ext  = 2,
  backtrace_I1_open = 1,
} backtrace_type;

/*
 * Backtrace Detect Limits
 */
void wavefronts_backtrace_valid_location(
    const int k,
    const wf_offset_t offset,
    const int pattern_length,
    const int text_length) {
  // Locate offset (remember that backtrace is always +1 offset ahead)
  const int v = WAVEFRONT_V(k,offset);
  const int h = WAVEFRONT_H(k,offset);
  const bool valid_location =
      (v > 0 && v <= pattern_length &&
       h > 0 && h <= text_length);
  if (!valid_location) {
    fprintf(stderr,"wavefronts_backtrace_valid_location:: Impossible situation\n"); exit(1);
  }
}
void wavefronts_backtrace_add_trailing_gap(
    cigar_t* const cigar,
    const int k,
    const int alignment_k) {
  // Parameters
  char* const operations = cigar->operations;
  int op_sentinel = cigar->begin_offset;
  // Add trailing gap
  int i;
  if (k < alignment_k) {
    for (i=k;i<alignment_k;++i) operations[op_sentinel--] = 'I';
  } else if (k > alignment_k) {
    for (i=alignment_k;i<k;++i) operations[op_sentinel--] = 'D';
  }
  cigar->begin_offset = op_sentinel;
}
/*
 * Backtrace Trace Patch Match/Mismsmatch
 */
int64_t wavefronts_backtrace_misms(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const mwavefront = wf_aligner->mwavefronts[score];
  if (mwavefront != NULL &&
      mwavefront->lo <= k &&
      k <= mwavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(mwavefront->offsets[k]+1,backtrace_M);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
void wavefronts_backtrace_matches(
    wavefront_aligner_t* const wf_aligner,
    const std::function<bool(const int&, const int&)>& traceback_lambda,
    const int k,
    wf_offset_t offset,
    const int num_matches,
    cigar_t* const cigar) {
  int i;
  for (i=0;i<num_matches;++i) {
    // DEBUG
    const int v = WAVEFRONT_V(k,offset);
    const int h = WAVEFRONT_H(k,offset);

//#ifdef WAVEFRONT_DEBUG
    if (!traceback_lambda(v-1, h-1)) { // Check match
      fprintf(stderr,"Backtrace error: Not a match traceback\n");
      exit(1);
    }
//#endif

    // Set Match
    cigar->operations[(cigar->begin_offset)--] = 'M';
    // Update state
    --offset;
  }
}
/*
 * Backtrace Trace Patch Deletion
 */
int64_t wavefronts_backtrace_del1_open(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const mwavefront = wf_aligner->mwavefronts[score];
  if (mwavefront != NULL &&
      mwavefront->lo <= k+1 &&
      k+1 <= mwavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(mwavefront->offsets[k+1],backtrace_D1_open);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
int64_t wavefronts_backtrace_del2_open(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const mwavefront = wf_aligner->mwavefronts[score];
  if (mwavefront != NULL &&
      mwavefront->lo <= k+1 &&
      k+1 <= mwavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(mwavefront->offsets[k+1],backtrace_D2_open);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
int64_t wavefronts_backtrace_del1_ext(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const d1wavefront = wf_aligner->d1wavefronts[score];
  if (d1wavefront != NULL &&
      d1wavefront->lo <= k+1 &&
      k+1 <= d1wavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(d1wavefront->offsets[k+1],backtrace_D1_ext);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
int64_t wavefronts_backtrace_del2_ext(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const d2wavefront = wf_aligner->d2wavefronts[score];
  if (d2wavefront != NULL &&
      d2wavefront->lo <= k+1 &&
      k+1 <= d2wavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(d2wavefront->offsets[k+1],backtrace_D2_ext);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
/*
 * Backtrace Trace Patch Insertion
 */
int64_t wavefronts_backtrace_ins1_open(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const mwavefront = wf_aligner->mwavefronts[score];
  if (mwavefront != NULL &&
      mwavefront->lo <= k-1 &&
      k-1 <= mwavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(mwavefront->offsets[k-1]+1,backtrace_I1_open);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
int64_t wavefronts_backtrace_ins2_open(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const mwavefront = wf_aligner->mwavefronts[score];
  if (mwavefront != NULL &&
      mwavefront->lo <= k-1 &&
      k-1 <= mwavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(mwavefront->offsets[k-1]+1,backtrace_I2_open);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
int64_t wavefronts_backtrace_ins1_ext(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const i1wavefront = wf_aligner->i1wavefronts[score];
  if (i1wavefront != NULL &&
      i1wavefront->lo <= k-1 &&
      k-1 <= i1wavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(i1wavefront->offsets[k-1]+1,backtrace_I1_ext);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
int64_t wavefronts_backtrace_ins2_ext(
    wavefront_aligner_t* const wf_aligner,
    const int score,
    const int k) {
  if (score < 0) return WAVEFRONT_OFFSET_NULL;
  wavefront_t* const i2wavefront = wf_aligner->i2wavefronts[score];
  if (i2wavefront != NULL &&
      i2wavefront->lo <= k-1 &&
      k-1 <= i2wavefront->hi) {
    return BACKTRACE_PIGGYBACK_SET(i2wavefront->offsets[k-1]+1,backtrace_I2_ext);
  } else {
    return WAVEFRONT_OFFSET_NULL;
  }
}
/*
 * Backtrace
 */
void wavefront_backtrace_lineal(
    wavefront_aligner_t* const wf_aligner,
    char* const pattern,
    const int pattern_length,
    char* const text,
    const int text_length,
    const int alignment_score) {
  // TODO
}
void wavefront_backtrace_affine(
    wavefront_aligner_t* const wf_aligner,
    const std::function<bool(const int&, const int&)>& traceback_lambda,
    const int pattern_length,
    const int text_length,
    const int alignment_score) {
  // Parameters
  const distance_metric_t distance_metric = wf_aligner->distance_metric;
  const wavefronts_penalties_t* const wavefront_penalties = &(wf_aligner->penalties);
  cigar_t* const cigar = &wf_aligner->cigar;
  const int alignment_k = WAVEFRONT_DIAGONAL(text_length,pattern_length);
  // Compute starting location
  int score = alignment_score;
  int k = alignment_k;
  wf_offset_t offset = wf_aligner->mwavefronts[alignment_score]->offsets[k];
  wavefronts_backtrace_valid_location(k,offset,pattern_length,text_length); // FIXME: Remove it
  // Trace the alignment back
  affine2p_matrix_type matrix_type = affine2p_matrix_M;
  int v = WAVEFRONT_V(k,offset);
  int h = WAVEFRONT_H(k,offset);
  while (v > 0 && h > 0 && score > 0) {
    // Compute scores
    const int mismatch = score - wavefront_penalties->mismatch;
    const int gap_open1 = score - wavefront_penalties->gap_opening1 - wavefront_penalties->gap_extension1;
    const int gap_open2 = score - wavefront_penalties->gap_opening2 - wavefront_penalties->gap_extension2;
    const int gap_extend1 = score - wavefront_penalties->gap_extension1;
    const int gap_extend2 = score - wavefront_penalties->gap_extension2;
    // Compute source offsets
    int64_t max_all;
    switch (matrix_type) {
      case affine2p_matrix_M: {
        const int64_t misms = wavefronts_backtrace_misms(wf_aligner,mismatch,k);
        const int64_t ins1_open = wavefronts_backtrace_ins1_open(wf_aligner,gap_open1,k);
        const int64_t ins1_ext  = wavefronts_backtrace_ins1_ext(wf_aligner,gap_extend1,k);
        const int64_t max_ins1 = MAX(ins1_open,ins1_ext);
        const int64_t del1_open = wavefronts_backtrace_del1_open(wf_aligner,gap_open1,k);
        const int64_t del1_ext  = wavefronts_backtrace_del1_ext(wf_aligner,gap_extend1,k);
        const int64_t max_del1 = MAX(del1_open,del1_ext);
        if (distance_metric == gap_affine) {
          max_all = MAX(misms,MAX(max_ins1,max_del1)); break;
        }
        const int64_t ins2_open = wavefronts_backtrace_ins2_open(wf_aligner,gap_open2,k);
        const int64_t ins2_ext  = wavefronts_backtrace_ins2_ext(wf_aligner,gap_extend2,k);
        const int64_t max_ins2 = MAX(ins2_open,ins2_ext);
        const int64_t del2_open = wavefronts_backtrace_del2_open(wf_aligner,gap_open2,k);
        const int64_t del2_ext  = wavefronts_backtrace_del2_ext(wf_aligner,gap_extend2,k);
        const int64_t max_del2 = MAX(del2_open,del2_ext);
        const int64_t max_ins = MAX(max_ins1,max_ins2);
        const int64_t max_del = MAX(max_del1,max_del2);
        max_all = MAX(misms,MAX(max_ins,max_del));
        break;
      }
      case affine2p_matrix_I1: {
        const int64_t ins1_open = wavefronts_backtrace_ins1_open(wf_aligner,gap_open1,k);
        const int64_t ins1_ext  = wavefronts_backtrace_ins1_ext(wf_aligner,gap_extend1,k);
        max_all = MAX(ins1_open,ins1_ext);
        break;
      }
      case affine2p_matrix_I2: {
        const int64_t ins2_open = wavefronts_backtrace_ins2_open(wf_aligner,gap_open2,k);
        const int64_t ins2_ext  = wavefronts_backtrace_ins2_ext(wf_aligner,gap_extend2,k);
        max_all = MAX(ins2_open,ins2_ext);
        break;
      }
      case affine2p_matrix_D1: {
        const int64_t del1_open = wavefronts_backtrace_del1_open(wf_aligner,gap_open1,k);
        const int64_t del1_ext  = wavefronts_backtrace_del1_ext(wf_aligner,gap_extend1,k);
        max_all = MAX(del1_open,del1_ext);
        break;
      }
      case affine2p_matrix_D2: {
        const int64_t del2_open = wavefronts_backtrace_del2_open(wf_aligner,gap_open2,k);
        const int64_t del2_ext  = wavefronts_backtrace_del2_ext(wf_aligner,gap_extend2,k);
        max_all = MAX(del2_open,del2_ext);
        break;
      }
      default:
        fprintf(stderr,"Wavefront-Backtrace. Wrong type trace.1\n");
        exit(1);
        break;
    }
    // Traceback Matches
    if (matrix_type == affine2p_matrix_M) {
      const int max_offset = BACKTRACE_PIGGYBACK_GET_OFFSET(max_all);
      const int num_matches = offset - max_offset;
      wavefronts_backtrace_matches(wf_aligner,traceback_lambda,k,offset,num_matches,cigar);
      offset = max_offset;
      // Update coordinates
      v = WAVEFRONT_V(k,offset);
      h = WAVEFRONT_H(k,offset);
      if (v <= 0 || h <= 0) break;
    }
    // Traceback Operation
    const int backtrace_type = BACKTRACE_PIGGYBACK_GET_TYPE(max_all);
    switch (backtrace_type) {
      case backtrace_M:
        score = mismatch;
        matrix_type = affine2p_matrix_M;
        break;
      case backtrace_I1_open:
        score = gap_open1;
        matrix_type = affine2p_matrix_M;
        break;
      case backtrace_I1_ext:
        score = gap_extend1;
        matrix_type = affine2p_matrix_I1;
        break;
      case backtrace_I2_open:
        score = gap_open2;
        matrix_type = affine2p_matrix_M;
        break;
      case backtrace_I2_ext:
        score = gap_extend2;
        matrix_type = affine2p_matrix_I2;
        break;
      case backtrace_D1_open:
        score = gap_open1;
        matrix_type = affine2p_matrix_M;
        break;
      case backtrace_D1_ext:
        score = gap_extend1;
        matrix_type = affine2p_matrix_D1;
        break;
      case backtrace_D2_open:
        score = gap_open2;
        matrix_type = affine2p_matrix_M;
        break;
      case backtrace_D2_ext:
        score = gap_extend2;
        matrix_type = affine2p_matrix_D2;
        break;
      default:
        fprintf(stderr,"Wavefront-Backtrace. Wrong type trace.2 \n");
        exit(1);
        break;
    }
    switch (backtrace_type) {
      case backtrace_M:
        cigar->operations[(cigar->begin_offset)--] = 'X';
        --offset;
        break;
      case backtrace_I1_open:
      case backtrace_I1_ext:
      case backtrace_I2_open:
      case backtrace_I2_ext:
        cigar->operations[(cigar->begin_offset)--] = 'I';
        --k; --offset;
        break;
      case backtrace_D1_open:
      case backtrace_D1_ext:
      case backtrace_D2_open:
      case backtrace_D2_ext:
        cigar->operations[(cigar->begin_offset)--] = 'D';
        ++k;
        break;
      default:
        fprintf(stderr,"Wavefront-Backtrace. Wrong type trace.3\n");
        exit(1);
        break;
    }
    // Update coordinates
    v = WAVEFRONT_V(k,offset);
    h = WAVEFRONT_H(k,offset);
  }
  // Account for last operations
  if (score == 0) {
    // Account for last stroke of matches
    wavefronts_backtrace_matches(wf_aligner,traceback_lambda,k,offset,offset,cigar);
  } else {
    // Account for last stroke of insertion/deletion
    while (v > 0) {cigar->operations[(cigar->begin_offset)--] = 'D'; --v;};
    while (h > 0) {cigar->operations[(cigar->begin_offset)--] = 'I'; --h;};
  }
  ++(cigar->begin_offset); // Set CIGAR length
}

#ifdef WFLAMBDA_NAMESPACE
}
#endif
