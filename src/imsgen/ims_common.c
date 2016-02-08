/*
 * Copyright (c) 2015 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 *
 * @brief: This file contains the signing code for "sign-tftf" a Linux
 * command-line app used to sign one or more Trusted Firmware Transfer
 * Format (TFTF) files used by the secure bootloader.
 *
 */

#include <sys/types.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <libgen.h>
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>
#include "util.h"
#include "mcl_arch.h"
#include "mcl_oct.h"
#include "mcl_big.h"
#include "mcl_ecdh.h"
#include "mcl_rand.h"
#include "mcl_rsa.h"
#include "crypto.h"
#include "db.h"
#include "ims_common.h"
#include "ims.h"

/* Uncomment the following define to enable IMS diagnostic messages */
/*#define IMS_DEBUGMSG*/

/* MSb mask for a byte */
#define BYTE_MASK_MSB               0x80

/* The maximum number of bytes to read from a prng_seed_file */
#define DEFAULT_PRNG_SEED_LENGTH    128

/* The PRNG seed is stored in this buffer */
uint8_t  prng_seed_buffer[EVP_MAX_MD_SIZE];
mcl_octet prng_seed = {0, sizeof(prng_seed_buffer), prng_seed_buffer};


/* Cryptographically Secure Random Number Generator */
csprng  rng;        /* Generic */


/* SHA256 working variable */
typedef struct {
    unsign32 length[2];
    unsign32 h[8];
    unsign32 w[80];
} sha256;


/* IMS working set */
#define IMS_SIZE            35
#define IMS_HAMMING_SIZE    32
#define IMS_HAMMING_WEIGHT  (IMS_HAMMING_SIZE * 8 / 2)
uint8_t  ims[IMS_SIZE];     /* TODO: convert to an octet for consistency? */

/* Endpoint Unique ID (EP_UID) working set */
uint8_t  ep_uid_buf[EP_UID_SIZE];
mcl_octet ep_uid = {0, sizeof(ep_uid_buf), ep_uid_buf};


/* Hash value used in calculating EPSK, MPDK ERRK */
uint8_t  y2[Y2_SIZE];       /* TODO: convert to an octet for consistency? */

/* Scratch octet for generating terms to feed sha256 */
uint8_t  scratch_buf[128];
mcl_octet scratch = {0, sizeof(scratch_buf), scratch_buf};


/* Endpoint Primary Signing/Verification Keys (EPSK, EPVK) */
uint8_t  epsk_buf[EPSK_SIZE];
mcl_octet epsk = {0, sizeof(epsk_buf), epsk_buf};

uint8_t  epvk_buf[EPVK_SIZE];
mcl_octet epvk = {0, sizeof(epvk_buf), epvk_buf};


/* Endpoint Secondary Signing/Verification Keys (ESSK/ESVK) */
uint8_t  essk_buf[ESSK_SIZE];
mcl_octet essk = {0, sizeof(essk_buf), essk_buf};
uint8_t  esvk_buf[ESVK_SIZE];
mcl_octet esvk = {0, sizeof(esvk_buf), esvk_buf};



/* Endpoint Rsa pRivate Key (ERRK/ERPK) */
uint8_t errk_pbuf[ERRK_PQ_SIZE];
mcl_octet errk_p = {0, sizeof(errk_pbuf), errk_pbuf};

uint8_t errk_qbuf[ERRK_PQ_SIZE];
mcl_octet errk_q = {0, sizeof(errk_qbuf), errk_qbuf};

uint8_t  erpk_mod_buf[ERRK_PQ_SIZE*2];
mcl_octet erpk_mod = {0, sizeof(erpk_mod_buf), erpk_mod_buf};

uint8_t  errk_d_buf[RSA2048_PUBLIC_KEY_SIZE];
mcl_octet errk_d = {0, sizeof(errk_d_buf), errk_d_buf};

#define ERPK_EXPONENT   65537
mcl_chunk p_ff[MCL_HFLEN][MCL_BS];
mcl_chunk q_ff[MCL_HFLEN][MCL_BS];

MCL_rsa_private_key rsa_private;
MCL_rsa_public_key  rsa_public;

static int get_prng_seed(const char * prng_seed_file,
                  const char * prng_seed_string);


/**
 * @brief Perform any common IMS initialization
 *
 * @param prng_seed_file Filename from which to read the seed
 * @param prng_seed_string Raw seed string
 *
 * @returns Zero if successful, errno otherwise.
 */
int ims_common_init(const char * prng_seed_file,
                    const char * prng_seed_string) {
    int status = 0;

    /* Initialize the cryptographically strong random number generators */
    status = get_prng_seed(prng_seed_file, prng_seed_string);
    if (status == 0) {
        MCL_RAND_seed(&rng, prng_seed.len, prng_seed.val);
    }

    return status;
}


/**
 * @brief Perform any common IMS de-initialization
 */
void ims_common_deinit(void) {
    /* De-initialize the cryptographically strong random number generators */
    MCL_RAND_clean(&rng);
}


/**
 * @brief Parse a raw seed string
 *
 * Allocates a buffer for the seed string and points the prng_seed mcl_octet
 * at it
 *
 * @param prng_seed_file Filename from which to read the seed
 * @param prng_seed_string Raw seed string
 *
 * @returns Zero if successful, errno otherwise.
 */
static int get_prng_seed(const char * prng_seed_file,
                         const char * prng_seed_string) {
    int status = 0;
    uint8_t raw_seed_buffer[DEFAULT_PRNG_SEED_LENGTH];
    size_t raw_seed_buffer_length;

    if (prng_seed_file) {
        /**
         * Read the first 128 bytes from the specified file (e.g. /dev/random
         * or Hitchhiker's Guide to the Galaxy)
         */
        int fd = 0;

        fd = open("/dev/urandom", O_RDONLY);
        if (fd <= -1) {
            fprintf(stderr, "ERROR: Unable to open '%s'(err %d)\n", prng_seed_file, errno);
            raw_seed_buffer_length = -1;
        } else {
            raw_seed_buffer_length = read(fd, raw_seed_buffer, sizeof(raw_seed_buffer));
            prng_seed_string = raw_seed_buffer;
            close(fd);
        }
    } else if (prng_seed_string) {
        /* Use all of the supplied string */
        raw_seed_buffer_length = strlen(prng_seed_string);
    }

    /* Hash the string obtained above to get the PRNG seed */
    if (!prng_seed_string || (raw_seed_buffer_length < 1)) {
        errno = EINVAL;
    } else {
        hash_it(prng_seed_string, raw_seed_buffer_length, prng_seed.val);
        prng_seed.len = SHA256_HASH_DIGEST_SIZE;
    }

    return errno;
}


/**
 * @brief Implement a canonical "X = sha256(Y || copy(b, n)" operation
 *
 * This combines hash_init, hash_update and has_final into a single call for
 * those times when you only have a single blob to hash.
 *
 * @param digest_x Pointer to the output digest buffer ("X" above)
 * @param hash_y Pointer to the input digest ("Y" above)
 * @param extend_byte The extension byte ("b" above)
 * @param extend_count The number of extension bytes to concatenate ("n" above)
 */
void sha256_concat(uint8_t * digest_x,
                   uint8_t * hash_y,
                   const uint8_t extend_byte,
                   uint32_t extend_count) {
    /* Scratch octet for generating terms to feed sha256 */
    uint8_t scratch_buf[256];
    mcl_octet scratch = {0, sizeof(scratch_buf), scratch_buf};

    MCL_OCT_jbytes(&scratch, hash_y, SHA256_HASH_DIGEST_SIZE);
    MCL_OCT_jbyte(&scratch, extend_byte, extend_count);
    hash_it(scratch.val, scratch.len, digest_x);
}


/**
 * @brief Calculate the EP_UID from the IMS (ES3 version)
 *
 * A mistake in ES3 boot ROM makes the EPUID of ES3 different
 * than the spec says. We must use this for the foreseeable future
 *
 * @param ims_value A pointer to the 35-byte IMS value
 * @param ep_uid A pointer to the octet EP_UID output value
 */
void calculate_epuid_es3(uint8_t * ims_value,
                         mcl_octet * ep_uid) {
    /* same code used in ES3 boot ROM to generate the EUID */
    int i;
    uint8_t ep_uid_calc[SHA256_HASH_DIGEST_SIZE];
    uint8_t y1[SHA256_HASH_DIGEST_SIZE];
    uint8_t z0[SHA256_HASH_DIGEST_SIZE];
    uint32_t temp;
    uint32_t *pims = (uint32_t *)ims_value;

    hash_start();
    /*** grab IMS 4 bytes at a time and feed that to hash_update */
    for (i = 0; i < 4; i++) {
        temp = pims[i] ^ 0x3d3d3d3d;
        hash_update((uint8_t *)&temp, 1);
    }
    hash_final(y1);

    hash_start();
    hash_update(y1, SHA256_HASH_DIGEST_SIZE);
    temp = 0x01010101;
    for (i = 0; i < 8; i++) {;
        hash_update((uint8_t *)&temp, 1);
    }
    hash_final(z0);

    hash_it(z0, SHA256_HASH_DIGEST_SIZE, ep_uid_calc);

    memcpy(ep_uid->val, ep_uid_calc, EP_UID_SIZE);
    ep_uid->len = EP_UID_SIZE;
}


/**
 * @brief Calculate the EP_UID from the IMS (correct version)
 *
 * This is the EP_UID calculation, correctly implemented - usable
 * on post-ES3 chips
 *
 * @param ims_value A pointer to the 35-byte IMS value
 * @param ep_uid A pointer to the octet EP_UID output value
 */
void calculate_epuid(uint8_t * ims_value,
                     mcl_octet * ep_uid) {
    /* same code used in ES3 boot ROM to generate the EUID */
    int i;
    static uint8_t ep_uid_calc[SHA256_HASH_DIGEST_SIZE];
    static uint8_t y1[SHA256_HASH_DIGEST_SIZE];
    static uint8_t z0[SHA256_HASH_DIGEST_SIZE];
    uint32_t temp;
    uint32_t *pims = (uint32_t *)ims_value;

    hash_start();
    /* grab IMS 4 bytes at a time and feed that to hash_update */
    for (i = 0; i < 4; i++) {
        temp = pims[i] ^ 0x3d3d3d3d;
        hash_update((uint8_t *)&temp, sizeof(temp));
    }
    hash_final(y1);

    hash_start();
    hash_update(y1, SHA256_HASH_DIGEST_SIZE);
    temp = 0x01010101;
    for (i = 0; i < 8; i++) {;
        hash_update((uint8_t *)&temp, sizeof(temp));
    }
    hash_final(z0);

    hash_it(z0, SHA256_HASH_DIGEST_SIZE, ep_uid_calc);

    memcpy(ep_uid->val, ep_uid_calc, EP_UID_SIZE);
    ep_uid->len = EP_UID_SIZE;
}


/**
 * @brief Calculate the "Y2" value
 *
 * "Y2" is a term used in all of the subsequent key generation routines.
 *
 * @param ims_value A pointer to the 35-byte IMS value
 * @param y2 A pointer to the 8-byte "Y2 output value
 */
void calculate_y2(uint8_t * ims_value, uint8_t * y2) {
    uint8_t  scratch_buf[128];
    mcl_octet scratch = {0, sizeof(scratch_buf), scratch_buf};

    /**
     *  Y2 = sha256(IMS[0:31] xor copy(0x5a, 32))
     */
    MCL_OCT_jbytes(&scratch, ims_value, IMS_HAMMING_SIZE);
    MCL_OCT_xorbyte(&scratch, 0x5a);
    hash_it(scratch.val, scratch.len, y2);
}


/**
 * @brief Calculate the Endpoint Primary Signing Key (EPSK)
 *
 * @param y2 A pointer to the Y2 term used by all
 * @param epsk A pointer to the output EPSK variable
 */
void calc_epsk(uint8_t * y2, mcl_octet * epsk) {
    uint8_t z1[SHA256_HASH_DIGEST_SIZE];
    uint8_t scratch_hash[SHA256_HASH_DIGEST_SIZE];
    int status;

    /**
     *  Y2 = sha256(IMS[0:31] xor copy(0x5a, 32))  // (provided)
     *  Z1 = sha256(Y2 || copy(0x01, 32))
     *  EPSK[0:31] = sha256(Z1 || copy(0x01, 32))
     *  EPSK[32:55] = sha256(Z1 || copy(0x02, 32))[0:23]
     */
    sha256_concat(z1, y2, 0x01, 32);

    sha256_concat(scratch_hash, z1, 0x01, 32);
    memcpy(&epsk->val[0], scratch_hash, SHA256_HASH_DIGEST_SIZE);

    sha256_concat(scratch_hash, z1, 0x02, 32);
    memcpy(&epsk->val[SHA256_HASH_DIGEST_SIZE],
           scratch_hash,
           (EPSK_SIZE - SHA256_HASH_DIGEST_SIZE));
    epsk->len = EPSK_SIZE;
#ifdef IMS_DEBUGMSG
    display_binary_data(epsk->val, epsk->len, true, "epsk ");
#endif
}


/**
 * @brief Calculate the Endpoint Primary Verification Key (EPVK)
 *
 * @param epsk A pointer to the input EPSK variable
 * @param epvk A pointer to the output EPVK variable
 *
 * @returns Zero if successful, MIRACL status otherwise (EPVK didn't validate)
 */
int calc_epvk(mcl_octet * epsk, mcl_octet * epvk) {
    int status = 0;

    /* Generate the corresponding EPVK public key, an Ed488-Goldilocks ECC */
    MCL_ECP_KEY_PAIR_GENERATE_C488(NULL, epsk, epvk);
    status = MCL_ECP_PUBLIC_KEY_VALIDATE_C488(1, epvk);
    if (status != 0) {
        printf("EPVK is invalid!\r\n");
    }
#ifdef IMS_DEBUGMSG
    display_binary_data(epvk->val, epvk->len, true, "epvk ");
#endif
    return status;
}


/**
 * @brief Calculate the Endpoint Secondary Signing Key (ESSK)
 *
 * @param y2 A pointer to the Y2 term used by all
 * @param essk A pointer to the output ESSK variable
 * @param ims_sample_compatibility If true, generate IMS values that are
 *        compatible with the original (incorrect) 100 sample values sent
 *        to Toshiba 2016/01/14. If false, generate the IMS value using
 *        the correct form.
 */
void calc_essk(uint8_t * y2, mcl_octet * essk,
               bool ims_sample_compatibility) {
    uint8_t z2[SHA256_HASH_DIGEST_SIZE];
    uint8_t scratch_hash[SHA256_HASH_DIGEST_SIZE];
    int status;


    if (ims_sample_compatibility) {
        /**
         *  Y2 = sha256(IMS[0:31] xor copy(0x5a, 32))  // (provided)
         *  EPSK[0:31] = sha256(Y2 || copy(0x01, 32))
         */
        sha256_concat(essk->val, y2, 0x01, 32);
    } else {
        /**
         *  Y2 = sha256(IMS[0:31] xor copy(0x5a, 32))  // (provided)
         *  Z2 = sha256(Y2 || copy(0x02, 32))
         *  ESSK[0:31] = sha256(Z2 || copy(0x01, 32))
         */
        sha256_concat(z2, y2, 0x02, 32);
        sha256_concat(scratch_hash, z2, 0x01, 32);

        memcpy(essk->val, scratch_hash, SHA256_HASH_DIGEST_SIZE);
        essk->len = SHA256_HASH_DIGEST_SIZE;
    }
    essk->len = SHA256_HASH_DIGEST_SIZE;
#ifdef IMS_DEBUGMSG
    display_binary_data(essk->val, essk->len, true, "essk ");
#endif
}


/**
 * @brief Calculate the Endpoint Secondary Verification Key (ESVK)
 *
 * @param essk A pointer to the input ESSK variable
 * @param esvk A pointer to the output ESVK variable
 *
 * @returns Zero if successful, MIRACL status otherwise (ESVK didn't validate)
 */
int calc_esvk(mcl_octet * essk, mcl_octet * esvk) {
    int status = 0;

    /* Generate the corresponding EPVK public key, a djb25519 ECC */
    MCL_ECP_KEY_PAIR_GENERATE_C25519(NULL, essk, esvk);
    status = MCL_ECP_PUBLIC_KEY_VALIDATE_C25519(1, esvk);
    if (status != 0) {
        printf("EPVK is invalid!\r\n");
    }
#ifdef IMS_DEBUGMSG
    display_binary_data(esvk->val, esvk->len, true, "esvk ");
#endif
    return status;
}


/**
 * @brief Calculate the Endpoint Rsa pRivate Key (ERRK) P & Q factors
 *
 * Calculates the ERRK_P & ERRK_Q, up to and including the bias-to-odd
 * step (i.e., that which can be extracted from IMS[0:31]).
 *
 *
 * @param y2 A pointer to the Y2 term used by all
 * @param ims A pointer to the ims (the upper 3 bytes will be modified)
 * @param erpk_p A pointer to a buffer to store the ERRK P coefficient
 * @param errk_q A pointer to a buffer to store the ERPK Q coefficient
 * @param ims_sample_compatibility If true, generate IMS values that are
 *        compatible with the original (incorrect) 100 sample values sent
 *        to Toshiba 2016/01/14. If false, generate the IMS value using
 *        the correct form.
 *
 * @returns Zero if successful, errno otherwise.
 */
void calc_errk_pq_bias_odd(uint8_t * y2,
                           uint8_t * ims,
                           mcl_octet * errk_p,
                           mcl_octet * errk_q,
                           bool ims_sample_compatibility) {
    int status = 0;
    uint8_t z3[SHA256_HASH_DIGEST_SIZE];
    uint8_t odd_mod_bitmask;
    int pq_len;

    /**
     * Define constants based on compatibility with the original 100 IMS samples
     * delivered to Toshiba or the correct production form.
     */
    if (ims_sample_compatibility) {
        odd_mod_bitmask = ODD_3_MOD_4_BITMASK;
        pq_len = SHA256_HASH_DIGEST_SIZE;
    } else {
        odd_mod_bitmask = ODD_MOD_BITMASK(ODD_MOD_PRODUCTION);
        pq_len = ERRK_PQ_SIZE;
    }

    /**
     *  Y2 = sha256(IMS[0:31] xor copy(0x5a, 32))  // (provided)
     *  Z3 = sha256(Y2 || copy(0x03, 32))
     *   :
     */
    sha256_concat(z3, y2, 0x03, 32);

    /**
     *   :
     *  ERRK_P[0:31] = sha256(Z3 || copy(0x01, 32))
     *  ERRK_P[32:63] = sha256(Z3 || copy(0x02, 32))
     *  ERRK_P[64:95] = sha256(Z3 || copy(0x03, 32))
     *  ERRK_P[96:127] = sha256(Z3 || copy(0x41, 32))
     *   :
     */
    sha256_concat(&errk_p->val[0],  z3, 0x01, 32);
    sha256_concat(&errk_p->val[32], z3, 0x02, 32);
    sha256_concat(&errk_p->val[64], z3, 0x03, 32);
    sha256_concat(&errk_p->val[96], z3, 0x04, 32);
    errk_p->len = pq_len;

    /**
     *   :
     *  ERRK_Q[0:31] = sha256(Z3 || copy(0x05, 32))
     *  ERRK_Q[32:63] = sha256(Z3 || copy(0x06, 32))
     *  ERRK_Q[64:95] = sha256(Z3 || copy(0x07, 32))
     *  ERRK_Q[96:127] = sha256(Z3 || copy(0x8, 32))
     *   :
     */
    sha256_concat(&errk_q->val[0],  z3, 0x05, 32);
    sha256_concat(&errk_q->val[32], z3, 0x06, 32);
    sha256_concat(&errk_q->val[64], z3, 0x07, 32);
    sha256_concat(&errk_q->val[96], z3, 0x08, 32);
    errk_q->len = pq_len;

    /* Force P, Q to be suitably odd */
    errk_p->val[0] |= odd_mod_bitmask;
    errk_q->val[0] |= odd_mod_bitmask;
}


/**
 * @brief Convert a big-endian octet into an FF
 *
 * @param ff The output ff
 * @param octet The input octet
 * @param n size of FF in MCL_BIGs
 */
void ff_from_big_endian_octet(mcl_chunk ff[][MCL_BS],
                              mcl_octet * octet,
                              int n) {
    /* pack it into an FF */
    MCL_FF_fromOctet_C25519(ff, octet, n);
}


/**
 * @brief Reverse the byte order of a buffer
 *
 * @param buf
 * @param length
 */
void reverse_buf(uint8_t *buf, size_t length) {
    uint8_t * front = buf;
    uint8_t * rear = &buf[length - 1];
    uint8_t   temp;

    while (front < rear) {
        temp = *front;
        *front++ = *rear;
        *rear-- = temp;
    }
}


/**
 * @brief Convert a little-endian octet into an FF
 *
 * @param ff The output ff
 * @param octet The input octet
 * @param n size of FF in MCL_BIGs
 */
void ff_from_little_endian_octet(mcl_chunk ff[][MCL_BS],
                                 mcl_octet * octet,
                                 int n) {
    static uint8_t scratch_buf[1024];
    mcl_octet scratch = {0, sizeof(scratch_buf), scratch_buf};
    int i, j;

    /* Make a scratch copy of the octet with the byte order reversed */
    for (i = 0, j = octet->len - 1; i < octet->len; i++, j--) {
        scratch_buf[i] = octet->val[j];
    }
    scratch.len = octet->len;

    /* Convert the big-endian scratch octet into an FF */
    MCL_FF_fromOctet_C25519(ff, &scratch, n);
 }


/**
 * @brief Calculate the private decryption exponent
 *
 * NOTE: This is largely copied from MCL_RSA_KEY_PAIR and retains its syntax
 * for compatability. The plan-of-record is that MIRACL will provide their own
 * version of this function at a later date.
 *
 * @param PRIV Pointer to private key (P & Q must be filled in)
 * @param PUB Pointer to public key (blank)
 * @param e Constant public exponent FF instance
 * @param ims_sample_compatibility If true, generate IMS values that are
 *        compatible with the original (incorrect) 100 sample values sent
 *        to Toshiba 2016/01/14. If false, generate the IMS value using
 *        the correct form.
 */
void rsa_secret(MCL_rsa_private_key *PRIV,
                MCL_rsa_public_key *PUB,
                sign32 e,
                bool ims_sample_compatibility) { /* IEEE1363 A16.11/A16.12 more or less */
    /**
     * Note: PRIV FF's are[MCL_FFLEN/2][MCL_NLEN] = [4][5]
     * internal chunks are[MCL_HFLEN][MCL_BS]     = [4][5]
     * so no size mismatch occurs.
     */
    mcl_chunk t[MCL_HFLEN][MCL_BS];
    mcl_chunk p1[MCL_HFLEN][MCL_BS];
    mcl_chunk q1[MCL_HFLEN][MCL_BS];

    MCL_FF_copy_C25519(p1, PRIV->p, MCL_HFLEN);
    MCL_FF_copy_C25519(q1, PRIV->q, MCL_HFLEN);
#ifdef RSA_PQ_FACTORABILITY
    if (ims_sample_compatibility) {
        /* in MCL_RSA_KEY_PAIR, p1 = P-1, q1 = Q -1 */
        MCL_FF_dec_C25519(p1,1,MCL_HFLEN);
        MCL_FF_dec_C25519(q1,1,MCL_HFLEN);
    }
#endif

    /* Calc. ERPK_MOD (PUB.N), ERPK_E */
    MCL_FF_mul_C25519(PUB->n, PRIV->p, PRIV->q, MCL_HFLEN);
    PUB->e = e;

    MCL_FF_copy_C25519(t, p1, MCL_HFLEN);
    MCL_FF_shr_C25519(t, MCL_HFLEN);
    MCL_FF_init_C25519(PRIV->dp, e, MCL_HFLEN);
    MCL_FF_invmodp_C25519(PRIV->dp, PRIV->dp, t, MCL_HFLEN);
    if (MCL_FF_parity_C25519(PRIV->dp) == 0) {
        MCL_FF_add_C25519(PRIV->dp, PRIV->dp, t, MCL_HFLEN);
    }
    MCL_FF_norm_C25519(PRIV->dp, MCL_HFLEN);

    MCL_FF_copy_C25519(t, q1, MCL_HFLEN);
    MCL_FF_shr_C25519(t, MCL_HFLEN);
    MCL_FF_init_C25519(PRIV->dq, e, MCL_HFLEN);
    MCL_FF_invmodp_C25519(PRIV->dq, PRIV->dq, t, MCL_HFLEN);
    if (MCL_FF_parity_C25519(PRIV->dq) == 0) {
        MCL_FF_add_C25519(PRIV->dq, PRIV->dq, t, MCL_HFLEN);
    }
    MCL_FF_norm_C25519(PRIV->dq, MCL_HFLEN);

    MCL_FF_invmodp_C25519(PRIV->c, PRIV->p, PRIV->q, MCL_HFLEN);

#ifdef IMS_DEBUGMSG
    print_ff("public.n", PUB->n, MCL_FFLEN);
    printf("public.e\n%08x\n\n", PUB->e);
    print_ff("private.p", PRIV->p, MCL_HFLEN);
    print_ff("private.q", PRIV->q, MCL_HFLEN);
    print_ff("private.dp", PRIV->dp, MCL_HFLEN);
    print_ff("private.dq", PRIV->dq, MCL_HFLEN);
    print_ff("private.c", PRIV->c, MCL_HFLEN);
#endif
    return;
}


/**
 * @brief print out an FF variable
 *
 * NOTE: This is largely copied from MCL_RSA_KEY_PAIR and retains its syntax
 * for compatability. The plan-of-record is that MIRACL will provide their own
 * version of this function at a later date.
 *
 * @param title Optional title
 * @param f The FF number to print
 * @param n @param n size of FF in MCL_BIGs
 */
#define MSB_FIRST
void print_ff(char * title, mcl_chunk ff[][MCL_BS], int n) {
    static uint8_t buf[2048];
    static mcl_octet temp = {0, sizeof(buf), buf};

    if (!title) {
        title = "";
    }

    MCL_FF_toOctet_C25519(&temp, ff, n);
#ifdef MSB_FIRST
    printf("%s\n", title);
    MCL_OCT_output(&temp);
    printf("\n");
#else
    display_binary_data(temp.val, temp.len, true, title);
#endif
}
