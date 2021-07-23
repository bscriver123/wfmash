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
 * DESCRIPTION: WaveFront penalties handling module
 */

#include "WFA/wavefront/wavefront_penalties.h"

#ifdef WFA_NAMESPACE
namespace wfa {
#endif

/*
 * Shift penalties
 */
    void wavefronts_penalties_shift(
            wavefronts_penalties_t* const wavefronts_penalties,
            const int match) {
        // Shift to zero match score
        wavefronts_penalties->mismatch -= match;
        wavefronts_penalties->gap_opening1 -= match;
        wavefronts_penalties->gap_extension1 -= match;
        wavefronts_penalties->gap_opening2 -= match;
        wavefronts_penalties->gap_extension2 -= match;
    }
/*
 * Penalties adjustment
 */
    void wavefronts_penalties_set_lineal(
            wavefronts_penalties_t* const wavefronts_penalties,
            lineal_penalties_t* const lineal_penalties,
            const wf_penalties_strategy_type penalties_strategy) {
        // Check base penalties
        if (lineal_penalties->match > 0) {
            fprintf(stderr,"Match score must be negative or zero (M=%d)\n",lineal_penalties->match);
            exit(1);
        } else if (lineal_penalties->mismatch <= 0 ||
                   lineal_penalties->deletion <= 0 ||
                   lineal_penalties->insertion <= 0) {
            fprintf(stderr,"Penalties must be strictly positive (X=%d,D=%d,I=%d). "
                           "Must be (X>0,D>0,I>0)\n",
                    lineal_penalties->mismatch,
                    lineal_penalties->deletion,
                    lineal_penalties->insertion);
            exit(1);
        } else if (lineal_penalties->deletion != lineal_penalties->insertion) {
            fprintf(stderr,"Currently Insertion/Deletion penalties must be equal (D==I)\n");
            exit(1);
        }
        // Copy base penalties
        wavefronts_penalties->mismatch = lineal_penalties->mismatch;
        wavefronts_penalties->gap_opening1 = lineal_penalties->deletion;
        // Adjust scores
        if (lineal_penalties->match > 0 &&
            penalties_strategy == wavefronts_penalties_shifted_penalties) {
            wavefronts_penalties_shift(wavefronts_penalties,lineal_penalties->match);
        }
        // Set unused
        wavefronts_penalties->gap_extension1 = -1;
        wavefronts_penalties->gap_opening2 = -1;
        wavefronts_penalties->gap_extension2 = -1;
    }
    void wavefronts_penalties_set_affine(
            wavefronts_penalties_t* const wavefronts_penalties,
            affine_penalties_t* const affine_penalties,
            const wf_penalties_strategy_type penalties_strategy) {
        // Check base penalties
        if (affine_penalties->match > 0) {
            fprintf(stderr,"Match score must be negative or zero (M=%d)\n",affine_penalties->match);
            exit(1);
        } else if (affine_penalties->mismatch <= 0 ||
                   affine_penalties->gap_opening <= 0 ||
                   affine_penalties->gap_extension <= 0) {
            fprintf(stderr,"Penalties must be strictly positive (X=%d,O=%d,E=%d). "
                           "Must be (X>0,O>0,E>0)\n",
                    affine_penalties->mismatch,
                    affine_penalties->gap_opening,
                    affine_penalties->gap_extension);
            exit(1);
        }
        // Copy base penalties
        wavefronts_penalties->mismatch = affine_penalties->mismatch;
        wavefronts_penalties->gap_opening1 = affine_penalties->gap_opening;
        wavefronts_penalties->gap_extension1 = affine_penalties->gap_extension;
        // Adjust scores
        if (affine_penalties->match > 0 &&
            penalties_strategy == wavefronts_penalties_shifted_penalties) {
            wavefronts_penalties_shift(wavefronts_penalties,affine_penalties->match);
        }
        // Set unused
        wavefronts_penalties->gap_opening2 = -1;
        wavefronts_penalties->gap_extension2 = -1;
    }
    void wavefronts_penalties_set_affine2p(
            wavefronts_penalties_t* const wavefronts_penalties,
            affine2p_penalties_t* const affine2p_penalties,
            const wf_penalties_strategy_type penalties_strategy) {
        // Check base penalties
        if (affine2p_penalties->match > 0) {
            fprintf(stderr,"Match score must be negative or zero (M=%d)\n",affine2p_penalties->match);
            exit(1);
        } else if (affine2p_penalties->mismatch <= 0 ||
                   affine2p_penalties->gap_opening1 <= 0 ||
                   affine2p_penalties->gap_extension1 <= 0 ||
                   affine2p_penalties->gap_opening2 <= 0 ||
                   affine2p_penalties->gap_extension2 <= 0) {
            fprintf(stderr,"Penalties must be strictly positive (X=%d,O1=%d,E1=%d,O2=%d,E2=%d). "
                           "Must be (X>0,O1>0,E1>0,O1>0,E1>0)\n",
                    affine2p_penalties->mismatch,
                    affine2p_penalties->gap_opening1,
                    affine2p_penalties->gap_extension1,
                    affine2p_penalties->gap_opening2,
                    affine2p_penalties->gap_extension2);
            exit(1);
        }
        // Copy base penalties
        wavefronts_penalties->mismatch = affine2p_penalties->mismatch;
        wavefronts_penalties->gap_opening1 = affine2p_penalties->gap_opening1;
        wavefronts_penalties->gap_extension1 = affine2p_penalties->gap_extension1;
        wavefronts_penalties->gap_opening2 = affine2p_penalties->gap_opening2;
        wavefronts_penalties->gap_extension2 = affine2p_penalties->gap_extension2;
        // Adjust scores
        if (affine2p_penalties->match > 0 &&
            penalties_strategy == wavefronts_penalties_shifted_penalties) {
            wavefronts_penalties_shift(wavefronts_penalties,affine2p_penalties->match);
        }
    }

#ifdef WFA_NAMESPACE
}
#endif
