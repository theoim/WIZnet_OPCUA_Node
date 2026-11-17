/*
 * Copyright (c) 2026 WIZnet Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file    hs_marker.c
 * @brief   Handshake timing markers: two GPIOs and one console line per OPN.
 *
 * Built only with -DOPCUA_HS_MARKERS=ON. Answers one question: while a client
 * opens a secure channel, how long does the poll loop stop serving everyone
 * else, and how much of that is the private-key operation?
 *
 *   GP2 (HS_PIN_OPN)   high while an asymmetric OPN is being delivered to the
 *                      server - from its first byte to the return of the call
 *                      that sent the response. Delivery is synchronous, so this
 *                      is the loop stall a new client or a renewal causes.
 *   GP3 (HS_PIN_PRIV)  high during each private-key operation: RSA-OAEP
 *                      decrypt, signature, ECDH. Inside the OPN window it is
 *                      the asymmetric decrypt + sign; outside it, it is the
 *                      CreateSession signature (the server signs the client's
 *                      certificate and nonce), the second stall a new client
 *                      costs.
 *
 * Probe both pins on a logic analyser together with the Ethernet activity, or
 * read the console:
 *
 *   [hs] begin n=3 s1 new t_us=12345678
 *   [hs] end   n=3 s1 new opn_us=1234567 priv_us=1180000 priv_ops=2
 *              pub_us=41000 pub_ops=2 aborted=0
 *   [hs] priv  outside-opn us=590000 op=pk_sign
 *
 * The console line is printed after the window closes, so it does not lengthen
 * the window it reports. "begin" is printed at the start so a PC-side tool can
 * mark the window in real time; one printf before the RSA work is a few tens
 * of microseconds against a second.
 *
 * Private and public key work is caught with the linker's --wrap on the
 * mbedTLS entry points open62541 calls. --wrap only redirects calls that cross
 * object files: open62541 (amalgamation) -> mbedTLS crosses, mbedTLS calling
 * itself does not, so each operation is counted once, at the outermost call.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "hs_marker.h"
#include "wiz_hs_marker.h"

#ifndef HS_PIN_OPN
#define HS_PIN_OPN   2u
#endif
#ifndef HS_PIN_PRIV
#define HS_PIN_PRIV  3u
#endif

static uint32_t s_count;          /* asymmetric OPNs seen since boot */
static bool     s_in_opn;
static uint8_t  s_sock;
static bool     s_renew;
static uint64_t s_opn_start;
static uint32_t s_priv_us, s_pub_us;
static uint16_t s_priv_ops, s_pub_ops;
static int      s_priv_depth;

void hs_marker_init(void)
{
    gpio_init(HS_PIN_OPN);
    gpio_set_dir(HS_PIN_OPN, GPIO_OUT);
    gpio_put(HS_PIN_OPN, 0);
    gpio_init(HS_PIN_PRIV);
    gpio_set_dir(HS_PIN_PRIV, GPIO_OUT);
    gpio_put(HS_PIN_PRIV, 0);
    printf("[hs] markers on: OPN=GP%u PRIV=GP%u crypto-wrap=%s\r\n",
           HS_PIN_OPN, HS_PIN_PRIV,
#if defined(OPCUA_HS_WRAP_CRYPTO)
           "on"
#else
           "off"
#endif
           );
}

void wiz_hs_opn_begin(uint8_t sock, bool renew)
{
    s_count++;
    s_in_opn = true;
    s_sock = sock;
    s_renew = renew;
    s_priv_us = s_pub_us = 0u;
    s_priv_ops = s_pub_ops = 0u;
    s_opn_start = time_us_64();
    gpio_put(HS_PIN_OPN, 1);
    printf("[hs] begin n=%lu s%u %s t_us=%llu\r\n", (unsigned long)s_count,
           sock, renew ? "renew" : "new", (unsigned long long)s_opn_start);
}

void wiz_hs_opn_end(uint8_t sock, bool aborted)
{
    uint64_t now = time_us_64();
    gpio_put(HS_PIN_OPN, 0);
    if(!s_in_opn)
        return;
    s_in_opn = false;
    printf("[hs] end   n=%lu s%u %s opn_us=%lu priv_us=%lu priv_ops=%u "
           "pub_us=%lu pub_ops=%u aborted=%d\r\n",
           (unsigned long)s_count, sock, s_renew ? "renew" : "new",
           (unsigned long)(now - s_opn_start),
           (unsigned long)s_priv_us, s_priv_ops,
           (unsigned long)s_pub_us, s_pub_ops, aborted ? 1 : 0);
    (void)s_sock;
}

#if defined(OPCUA_HS_WRAP_CRYPTO)

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ecdh.h"

static uint64_t priv_enter(void)
{
    if(s_priv_depth++ == 0)
        gpio_put(HS_PIN_PRIV, 1);
    return time_us_64();
}

static void priv_leave(uint64_t t0, const char *op)
{
    uint32_t us = (uint32_t)(time_us_64() - t0);
    if(--s_priv_depth == 0)
        gpio_put(HS_PIN_PRIV, 0);
    if(s_priv_depth != 0)
        return;                     /* nested: the outer call counts it */
    if(s_in_opn) {
        s_priv_us += us;
        s_priv_ops++;
    } else {
        printf("[hs] priv  outside-opn us=%lu op=%s\r\n", (unsigned long)us, op);
    }
}

static void pub_add(uint64_t t0)
{
    if(s_in_opn) {
        s_pub_us += (uint32_t)(time_us_64() - t0);
        s_pub_ops++;
    }
}

/* ---- private-key operations ---- */

int __real_mbedtls_pk_sign(mbedtls_pk_context *ctx, mbedtls_md_type_t md_alg,
                           const unsigned char *hash, size_t hash_len,
                           unsigned char *sig, size_t sig_size, size_t *sig_len,
                           int (*f_rng)(void *, unsigned char *, size_t), void *p_rng);
int __wrap_mbedtls_pk_sign(mbedtls_pk_context *ctx, mbedtls_md_type_t md_alg,
                           const unsigned char *hash, size_t hash_len,
                           unsigned char *sig, size_t sig_size, size_t *sig_len,
                           int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    uint64_t t0 = priv_enter();
    int r = __real_mbedtls_pk_sign(ctx, md_alg, hash, hash_len, sig, sig_size,
                                   sig_len, f_rng, p_rng);
    priv_leave(t0, "pk_sign");
    return r;
}

int __real_mbedtls_pk_decrypt(mbedtls_pk_context *ctx,
                              const unsigned char *input, size_t ilen,
                              unsigned char *output, size_t *olen, size_t osize,
                              int (*f_rng)(void *, unsigned char *, size_t), void *p_rng);
int __wrap_mbedtls_pk_decrypt(mbedtls_pk_context *ctx,
                              const unsigned char *input, size_t ilen,
                              unsigned char *output, size_t *olen, size_t osize,
                              int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{
    uint64_t t0 = priv_enter();
    int r = __real_mbedtls_pk_decrypt(ctx, input, ilen, output, olen, osize,
                                      f_rng, p_rng);
    priv_leave(t0, "pk_decrypt");
    return r;
}

int __real_mbedtls_rsa_rsaes_oaep_decrypt(mbedtls_rsa_context *ctx,
                                          int (*f_rng)(void *, unsigned char *, size_t),
                                          void *p_rng,
                                          const unsigned char *label, size_t label_len,
                                          size_t *olen, const unsigned char *input,
                                          unsigned char *output, size_t output_max_len);
int __wrap_mbedtls_rsa_rsaes_oaep_decrypt(mbedtls_rsa_context *ctx,
                                          int (*f_rng)(void *, unsigned char *, size_t),
                                          void *p_rng,
                                          const unsigned char *label, size_t label_len,
                                          size_t *olen, const unsigned char *input,
                                          unsigned char *output, size_t output_max_len)
{
    uint64_t t0 = priv_enter();
    int r = __real_mbedtls_rsa_rsaes_oaep_decrypt(ctx, f_rng, p_rng, label,
                                                  label_len, olen, input, output,
                                                  output_max_len);
    priv_leave(t0, "oaep_decrypt");
    return r;
}

int __real_mbedtls_ecdh_compute_shared(mbedtls_ecp_group *grp, mbedtls_mpi *z,
                                       const mbedtls_ecp_point *Q, const mbedtls_mpi *d,
                                       int (*f_rng)(void *, unsigned char *, size_t),
                                       void *p_rng);
int __wrap_mbedtls_ecdh_compute_shared(mbedtls_ecp_group *grp, mbedtls_mpi *z,
                                       const mbedtls_ecp_point *Q, const mbedtls_mpi *d,
                                       int (*f_rng)(void *, unsigned char *, size_t),
                                       void *p_rng)
{
    uint64_t t0 = priv_enter();
    int r = __real_mbedtls_ecdh_compute_shared(grp, z, Q, d, f_rng, p_rng);
    priv_leave(t0, "ecdh");
    return r;
}

/* ---- public-key operations (time only, no pin) ---- */

int __real_mbedtls_pk_verify(mbedtls_pk_context *ctx, mbedtls_md_type_t md_alg,
                             const unsigned char *hash, size_t hash_len,
                             const unsigned char *sig, size_t sig_len);
int __wrap_mbedtls_pk_verify(mbedtls_pk_context *ctx, mbedtls_md_type_t md_alg,
                             const unsigned char *hash, size_t hash_len,
                             const unsigned char *sig, size_t sig_len)
{
    uint64_t t0 = time_us_64();
    int r = __real_mbedtls_pk_verify(ctx, md_alg, hash, hash_len, sig, sig_len);
    pub_add(t0);
    return r;
}

int __real_mbedtls_rsa_rsaes_oaep_encrypt(mbedtls_rsa_context *ctx,
                                          int (*f_rng)(void *, unsigned char *, size_t),
                                          void *p_rng,
                                          const unsigned char *label, size_t label_len,
                                          size_t ilen, const unsigned char *input,
                                          unsigned char *output);
int __wrap_mbedtls_rsa_rsaes_oaep_encrypt(mbedtls_rsa_context *ctx,
                                          int (*f_rng)(void *, unsigned char *, size_t),
                                          void *p_rng,
                                          const unsigned char *label, size_t label_len,
                                          size_t ilen, const unsigned char *input,
                                          unsigned char *output)
{
    uint64_t t0 = time_us_64();
    int r = __real_mbedtls_rsa_rsaes_oaep_encrypt(ctx, f_rng, p_rng, label,
                                                  label_len, ilen, input, output);
    pub_add(t0);
    return r;
}

#endif /* OPCUA_HS_WRAP_CRYPTO */
