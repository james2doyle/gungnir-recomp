// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// atrac3p_selftest.cpp — regression suite for the standalone ATRAC3+ decoder.
// Adapted from Nakagawa recomp src/rt/atrac3p_selftest.c (LGPL-2.1-or-later);
// the SHA-256 determinism checks are memcmp, and the private fixture hook is
// not compiled here.
//
// Public, source-owned tests (no game inputs, no copyrighted fixtures):
//   - create() validation (channel set {1,2,3,4,6,7,8}, block_align > 0,
//     NULL out),
//   - decode() NULL-argument and oversized-frame rejection,
//   - garbage-frame rejection with the *samples_out = 0 contract,
//   - a deterministic positive decode through the PRODUCTION entry point:
//     a frame whose first byte is 0x60 (start bit 0, then the
//     CH_UNIT_TERMINATOR channel-unit id) exits the unit loop immediately,
//     which drives init_get_bits8 + start-bit check + terminator detection +
//     the nb_samples contract + FFMIN(block_align, buf_size) return and
//     yields exactly ATRAC3P_FRAME_SAMPLES all-zero samples per channel
//     (the zeroed output buffer is never written when no unit is decoded),
//   - determinism: the same frame decodes to byte-identical PCM across
//     instances, after reset, and after flush,
//   - destroy(NULL) no-op,
//   - transform-path canary: a deterministic MONO channel-unit frame drives a
//     real unit through decode_residual_spectrum() + reconstruct_frame()
//     (IMDCT/IPQF) and must decode with the nb_samples=2048 contract and
//     byte-identical PCM across instances/reset/flush (fails-before/after the
//     CONFIG_MDCT configuration fix; see PROVENANCE.md).
//
// Private fixture hook (optional, never required for the public suite):
// if the environment variable ATRAC3P_FIXTURE points to a directory
// containing stream.bin (concatenated ATRAC3+ frames, each exactly
// block_align bytes) and meta.txt (lines: channels=N, block_align=N,
// frames=N, pcm_sha256=<64 hex> of the full interleaved s16 PCM produced by
// atrac3p_decode()), the whole stream is decoded and hashed with the same
// SHA-256 used by the VFPU table loader (sr_vfpu_sha256). Mismatch or a
// malformed fixture is a hard failure; when the variable is unset the
// fixture portion prints a SKIP line and is not counted. The fixture must
// be sourced from license-compatible material (e.g. samples.ffmpeg.org
// ATRAC3+ files or PSP rips) and must never be committed to this
// repository.
//
// Exit code 0 = all public checks passed (fixture portion SKIP or passed).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atrac3p_api.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);           \
        }                                                                      \
    } while (0)

/* Terminator-only frame: start bit 0, then channel-unit id 3
 * (CH_UNIT_TERMINATOR) in MSB-first order -> 0b01100000 = 0x60. */
static const uint8_t TERMINATOR_FRAME[] = { 0x60 };

/* Deterministic MONO channel-unit frame (start bit 0, unit id 0). This exact
 * byte pattern passes the decoder's structural checks for 1 channel and
 * drives a real unit through decode_residual_spectrum() + reconstruct_frame()
 * (i.e. the IMDCT/IPQF transform pipeline). It decodes to deterministic
 * all-zero PCM (no coded spectral data in the garbage). It is the canary for
 * the transform path: without CONFIG_MDCT the imdct_calc/imdct_half function
 * pointers are never assigned (fft_template.c assigns them under
 * #if CONFIG_MDCT) and this decode crashes with a NULL indirect call, so this
 * test fails-before/passes-after that configuration fix. */
static const uint8_t MONO_UNIT_FRAME[] = { 0x16, 0x0c, 0xf6, 0x86, 0x87, 0x35, 0xd3, 0x1c, 0xb8 };

/* Positive-path check: the terminator-only frame must decode successfully
 * through the production entry with nb_samples = ATRAC3P_FRAME_SAMPLES and
 * byte-identical all-zero PCM. Runs twice plus after reset and flush to
 * prove determinism and state restoration. */
static void test_terminator_decode(void)
{
    Atrac3pHandle *h1 = NULL, *h2 = NULL;
    int16_t pcm1[2 * ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[2 * ATRAC3P_FRAME_SAMPLES];
    int ret, samples = -1, i;
    const size_t n = 2 * (size_t)ATRAC3P_FRAME_SAMPLES;

    CHECK(atrac3p_create(2, 1536, &h1) == 0, "create h1");
    CHECK(atrac3p_create(2, 1536, &h2) == 0, "create h2");
    if (!h1 || !h2)
        return;

    ret = atrac3p_decode(h1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "terminator frame consumed");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "nb_samples = 2048 on success");
    for (i = 0; i < (int)n; i++)
        CHECK(pcm1[i] == 0, "terminator frame PCM is all-zero");

    /* determinism: a second fresh instance produces identical PCM */
    ret = atrac3p_decode(h2, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm2, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "h2 terminator consumed");
    CHECK(memcmp(pcm1, pcm2, n * sizeof(int16_t)) == 0, "deterministic PCM across instances");

    /* reset restores post-create state */
    CHECK(atrac3p_reset(h1) == 0, "reset h1");
    ret = atrac3p_decode(h1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "decode after reset");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "nb_samples after reset");
    CHECK(memcmp(pcm1, pcm2, n * sizeof(int16_t)) == 0, "PCM after reset identical");

    /* flush is the same primitive */
    CHECK(atrac3p_flush(h1) == 0, "flush h1");
    ret = atrac3p_decode(h1, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "decode after flush");
    CHECK(memcmp(pcm1, pcm2, n * sizeof(int16_t)) == 0, "PCM after flush identical");

    atrac3p_destroy(h1);
    atrac3p_destroy(h2);
}

/* Transform-path canary: the MONO unit frame must decode through the
 * production entry point (ret = frame length, samples = ATRAC3P_FRAME_SAMPLES)
 * and produce byte-identical PCM across instances, reset and flush. The
 * decode reaches reconstruct_frame(), so a broken transform configuration
 * (missing CONFIG_MDCT) crashes here instead of passing silently. */
static void test_imdct_path_decode(void)
{
    Atrac3pHandle *h1 = NULL, *h2 = NULL;
    int16_t pcm1[ATRAC3P_FRAME_SAMPLES];
    int16_t pcm2[ATRAC3P_FRAME_SAMPLES];
    int ret, samples = -1;

    CHECK(atrac3p_create(1, 3071, &h1) == 0, "create h1 (mono unit)");
    CHECK(atrac3p_create(1, 3071, &h2) == 0, "create h2 (mono unit)");
    if (!h1 || !h2)
        return;

    ret = atrac3p_decode(h1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "mono unit frame consumed");
    CHECK(samples == ATRAC3P_FRAME_SAMPLES, "mono unit nb_samples");

    ret = atrac3p_decode(h2, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm2, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "h2 mono unit consumed");
    CHECK(memcmp(pcm1, pcm2, sizeof(pcm1)) == 0, "mono unit PCM deterministic across instances");

    CHECK(atrac3p_reset(h1) == 0, "reset h1 (mono unit)");
    ret = atrac3p_decode(h1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "mono unit decode after reset");
    CHECK(memcmp(pcm1, pcm2, sizeof(pcm1)) == 0, "mono unit PCM deterministic after reset");

    CHECK(atrac3p_flush(h1) == 0, "flush h1 (mono unit)");
    ret = atrac3p_decode(h1, MONO_UNIT_FRAME, sizeof(MONO_UNIT_FRAME),
                         pcm1, &samples);
    CHECK(ret == (int)sizeof(MONO_UNIT_FRAME), "mono unit decode after flush");
    CHECK(memcmp(pcm1, pcm2, sizeof(pcm1)) == 0, "mono unit PCM deterministic after flush");

    atrac3p_destroy(h1);
    atrac3p_destroy(h2);
}

/* Plain AT3 output polarity: the PSP decoder's sign (#1096), with the same scale and
 * saturation as ATRAC3+. MUTANT: the unnegated conversion fails all three sign checks. */
static void test_at3_pcm_polarity(void)
{
    CHECK(atrac3_pcm_sample(0.5f) == -16384, "AT3 sample +0.5 is -16384 (PSP polarity)");
    CHECK(atrac3_pcm_sample(-0.25f) == 8192, "AT3 sample -0.25 is +8192 (PSP polarity)");
    CHECK(atrac3_pcm_sample(-1.0f) == 32767, "AT3 sample -1.0 saturates to +32767");
    CHECK(atrac3_pcm_sample(2.0f) == -32768, "AT3 sample +2.0 saturates to -32768");
    CHECK(atrac3_pcm_sample(0.0f) == 0, "AT3 silence stays 0");
}

static void test_create_validation(void)
{
    Atrac3pHandle *h = NULL;

    CHECK(atrac3p_create(0, 1536, &h) < 0, "reject 0 channels");
    CHECK(atrac3p_create(5, 1536, &h) < 0, "reject 5 channels");
    CHECK(atrac3p_create(9, 1536, &h) < 0, "reject 9 channels");
    CHECK(atrac3p_create(2, 0, &h) < 0, "reject block_align 0");
    CHECK(atrac3p_create(2, -1, &h) < 0, "reject negative block_align");
    CHECK(atrac3p_create(2, 1536, NULL) < 0, "reject NULL out");
    CHECK(atrac3p_create(1, 1536, &h) == 0 && h != NULL, "accept 1 channel");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(3, 1536, &h) == 0 && h != NULL, "accept 3 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(4, 1536, &h) == 0 && h != NULL, "accept 4 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(6, 1536, &h) == 0 && h != NULL, "accept 6 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(7, 1536, &h) == 0 && h != NULL, "accept 7 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
    CHECK(atrac3p_create(8, 1536, &h) == 0 && h != NULL, "accept 8 channels");
    if (h) { atrac3p_destroy(h); h = NULL; }
}

static void test_decode_rejection(void)
{
    Atrac3pHandle *h = NULL;
    int16_t pcm[2 * ATRAC3P_FRAME_SAMPLES];
    uint8_t garbage[1536];
    int ret, samples = -1, i;

    CHECK(atrac3p_create(2, 1536, &h) == 0 && h != NULL, "create for rejection");
    if (!h)
        return;

    for (i = 0; i < (int)sizeof(garbage); i++)
        garbage[i] = (uint8_t)(i * 7 + 13);

    ret = atrac3p_decode(h, garbage, sizeof(garbage), pcm, &samples);
    CHECK(ret < 0, "garbage frame rejected");
    CHECK(samples == 0, "nb_samples stays 0 on failure");

    CHECK(atrac3p_decode(h, garbage, (int)sizeof(garbage) + 1, pcm, &samples) < 0,
          "oversized frame rejected");
    CHECK(atrac3p_decode(NULL, garbage, (int)sizeof(garbage), pcm, &samples) < 0,
          "NULL handle rejected");
    CHECK(atrac3p_decode(h, NULL, (int)sizeof(garbage), pcm, &samples) < 0,
          "NULL frame rejected");
    CHECK(atrac3p_decode(h, garbage, (int)sizeof(garbage), NULL, &samples) < 0,
          "NULL pcm rejected");
    CHECK(atrac3p_decode(h, garbage, (int)sizeof(garbage), pcm, NULL) < 0,
          "NULL samples rejected");
    CHECK(atrac3p_reset(NULL) < 0, "NULL reset rejected");
    CHECK(atrac3p_flush(NULL) < 0, "NULL flush rejected");

    /* controlled recovery after failures */
    ret = atrac3p_decode(h, TERMINATOR_FRAME, sizeof(TERMINATOR_FRAME),
                         pcm, &samples);
    CHECK(ret == (int)sizeof(TERMINATOR_FRAME), "decode succeeds after rejections");

    atrac3p_destroy(h);
    atrac3p_destroy(NULL); /* must be a no-op */
}


int main(void)
{
    test_at3_pcm_polarity();
    test_create_validation();
    test_decode_rejection();
    test_terminator_decode();
    test_imdct_path_decode();

    if (g_failures) {
        fprintf(stderr, "atrac3p_selftest: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("atrac3p_selftest: all %d checks passed\n", g_checks);
    return 0;
}
