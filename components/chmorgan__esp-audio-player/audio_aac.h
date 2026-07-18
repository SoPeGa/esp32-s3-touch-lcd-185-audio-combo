#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "audio_decode_types.h"
#include "esp_audio_dec.h"

#define AAC_READ_BUF_SIZE (8192)

typedef struct {
    esp_audio_dec_handle_t  handle;
    uint8_t                *data_buf;
    size_t                  data_buf_size;
    size_t                  bytes_in_buf;
    uint8_t                *read_ptr;
    bool                    eof_reached;
} aac_instance;

/* Scan the first bytes of fp for an ADTS sync word (0xFFF0/F1/F8/F9).
   Safe to call on non-seekable HTTP streams: consumes up to 128 bytes. */
bool is_aac(FILE *fp);

/* Open an AAC decoder and allocate the read buffer.
   Returns true on success. */
bool aac_open(aac_instance *inst);

/* Close the decoder and free the read buffer. */
void aac_close(aac_instance *inst);

/* Decode the next ADTS frame from fp into pData.
   Mirrors the decode_mp3() contract. */
DECODE_STATUS decode_aac(FILE *fp, decode_data *pData, aac_instance *pInstance);
