#include <string.h>
#include <stdlib.h>
#include "audio_aac.h"
#include "audio_log.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_dec.h"
#include "decoder/impl/esp_aac_dec.h"

static const char *TAG = "aac";

static bool is_adts_sync(const uint8_t *p)
{
    return p[0] == 0xff && (p[1] & 0xf6) == 0xf0;
}

static size_t find_adts_sync(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (is_adts_sync(data + i)) return i;
    }
    return len;
}

static size_t adts_frame_size(const uint8_t *p)
{
    return ((size_t)(p[3] & 0x03) << 11) | ((size_t)p[4] << 3) | ((size_t)p[5] >> 5);
}

static bool valid_adts_header(const uint8_t *p, size_t available)
{
    if (available < 7 || !is_adts_sync(p)) return false;
    size_t header_size = (p[1] & 1) ? 7 : 9;
    size_t frame_size = adts_frame_size(p);
    int sample_rate_index = (p[2] >> 2) & 0x0f;
    return sample_rate_index != 0x0f && frame_size >= header_size && frame_size <= AAC_READ_BUF_SIZE;
}

bool is_aac(FILE *fp)
{
    /* ADTS syncword: 0xFF followed by 0xF0/F1/F8/F9
     * Byte 1 pattern: 1111 x 00 y  (x=ID, y=protection_absent)
     * Mask 0xF6 zeros out the ID and protection bits; result must be 0xF0.
     *
     * Scan 4096 bytes: is_mp3() already consumed bytes 0-2 on non-seekable
     * HTTP streams, so the first ADTS frame sync is gone.  The second frame
     * starts at byte ≈ frame_size.  At 128 kbps/44100 Hz that's ~370 bytes;
     * at 64 kbps it's ~186 bytes.  4096 bytes covers all practical bitrates
     * up to ~700 kbps while staying well inside the decoder's read buffer. */
    uint8_t *buf = static_cast<uint8_t *>(malloc(4096));
    if (!buf) return false;
    size_t n = fread(buf, 1, 4096, fp);
    fseek(fp, 0, SEEK_SET);
    bool found = false;
    for (size_t i = 0; i + 1 < n; i++) {
        if (is_adts_sync(buf + i)) {
            found = true;
            break;
        }
    }
    free(buf);
    return found;
}

bool aac_open(aac_instance *inst)
{
    inst->data_buf = static_cast<uint8_t *>(malloc(AAC_READ_BUF_SIZE));
    if (!inst->data_buf) {
        ESP_LOGE(TAG, "aac buf alloc failed");
        return false;
    }
    inst->data_buf_size = AAC_READ_BUF_SIZE;
    inst->bytes_in_buf  = 0;
    inst->read_ptr      = inst->data_buf;
    inst->eof_reached   = false;

    /* Register AAC decoder with the common framework (idempotent). */
    esp_aac_dec_register();

    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_cfg.no_adts_header  = false;  /* ADTS framing present */
    aac_cfg.aac_plus_enable = true;   /* support HE-AAC / HE-AACv2 */

    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type   = ESP_AUDIO_TYPE_AAC;
    dec_cfg.cfg    = &aac_cfg;
    dec_cfg.cfg_sz = sizeof(aac_cfg);

    esp_audio_err_t err = esp_audio_dec_open(&dec_cfg, &inst->handle);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_dec_open err %d", err);
        free(inst->data_buf);
        inst->data_buf = nullptr;
        inst->handle   = nullptr;
        return false;
    }
    return true;
}

void aac_close(aac_instance *inst)
{
    if (inst->handle) {
        esp_audio_dec_close(inst->handle);
        inst->handle = nullptr;
    }
    if (inst->data_buf) {
        free(inst->data_buf);
        inst->data_buf = nullptr;
    }
}

DECODE_STATUS decode_aac(FILE *fp, decode_data *pData, aac_instance *pInstance)
{
    size_t offset = (size_t)(pInstance->read_ptr - pInstance->data_buf);
    if (offset > pInstance->bytes_in_buf || pInstance->bytes_in_buf > pInstance->data_buf_size) {
        ESP_LOGE(TAG, "AAC buffer state invalid: offset=%u bytes=%u capacity=%u",
                 (unsigned)offset, (unsigned)pInstance->bytes_in_buf,
                 (unsigned)pInstance->data_buf_size);
        pInstance->read_ptr = pInstance->data_buf;
        pInstance->bytes_in_buf = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    /* Refill the read buffer when it drops below half-full. */
    size_t unread = pInstance->bytes_in_buf - offset;

    if (unread < pInstance->data_buf_size / 2 && !pInstance->eof_reached) {
        memmove(pInstance->data_buf, pInstance->read_ptr, unread);
        size_t free_space = pInstance->data_buf_size - unread;
        size_t nRead = fread(pInstance->data_buf + unread, 1, free_space, fp);
        pInstance->bytes_in_buf = unread + nRead;
        pInstance->read_ptr     = pInstance->data_buf;
        if (nRead == 0 || feof(fp)) {
            pInstance->eof_reached = true;
        }
        unread = pInstance->bytes_in_buf;
    }

    if (unread == 0) {
        return DECODE_STATUS_DONE;
    }

    size_t sync_offset = find_adts_sync(pInstance->read_ptr, unread);
    while (sync_offset < unread &&
           !valid_adts_header(pInstance->read_ptr + sync_offset, unread - sync_offset)) {
        sync_offset++;
        size_t next = find_adts_sync(pInstance->read_ptr + sync_offset, unread - sync_offset);
        sync_offset += next;
    }
    if (sync_offset >= unread) {
        /* Keep a possible first sync byte for the next refill. */
        pInstance->read_ptr += unread > 1 ? unread - 1 : unread;
        pData->frame_count = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }
    pInstance->read_ptr += sync_offset;
    unread -= sync_offset;

    if (unread < 7) {
        pData->frame_count = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    size_t frame_size = adts_frame_size(pInstance->read_ptr);
    size_t header_size = (pInstance->read_ptr[1] & 1) ? 7 : 9;
    if (frame_size < header_size || frame_size > pInstance->data_buf_size) {
        pInstance->read_ptr++;
        pData->frame_count = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }
    if (unread < frame_size) {
        /* Force compaction/refill on the next call without losing this partial frame. */
        memmove(pInstance->data_buf, pInstance->read_ptr, unread);
        size_t nRead = fread(pInstance->data_buf + unread, 1,
                             pInstance->data_buf_size - unread, fp);
        pInstance->bytes_in_buf = unread + nRead;
        pInstance->read_ptr = pInstance->data_buf;
        if (nRead == 0 || feof(fp)) pInstance->eof_reached = true;
        if (pInstance->bytes_in_buf < frame_size) {
            pData->frame_count = 0;
            return pInstance->eof_reached ? DECODE_STATUS_DONE : DECODE_STATUS_NO_DATA_CONTINUE;
        }
        unread = pInstance->bytes_in_buf;
    }

    /* A second valid sync prevents payload bytes from being mistaken for an ADTS header. */
    if (unread >= frame_size + 7 &&
        !valid_adts_header(pInstance->read_ptr + frame_size, unread - frame_size)) {
        pInstance->read_ptr++;
        pData->frame_count = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    esp_audio_dec_in_raw_t in_raw = {};
    in_raw.buffer = pInstance->read_ptr;
    in_raw.len    = (uint32_t)frame_size;

    esp_audio_dec_out_frame_t out_frame = {};
    out_frame.buffer = pData->samples;
    out_frame.len    = (uint32_t)pData->samples_capacity_max;

    esp_audio_err_t err = esp_audio_dec_process(pInstance->handle, &in_raw, &out_frame);

    /* Each call contains exactly one ADTS frame. Drop only that frame, even if corrupt. */
    pInstance->read_ptr += frame_size;

    if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_CONTINUE) {
        if (pInstance->eof_reached) {
            return DECODE_STATUS_DONE;
        }
        LOGI_1("AAC decode err %d, dropping ADTS frame (%u bytes)", (int)err, (unsigned)frame_size);
        pData->frame_count = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    if (out_frame.decoded_size > pData->samples_capacity_max) {
        ESP_LOGE(TAG, "AAC decoder output overflow prevented: %u > %u",
                 (unsigned)out_frame.decoded_size, (unsigned)pData->samples_capacity_max);
        pData->frame_count = 0;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    if (out_frame.decoded_size > 0) {
        esp_audio_dec_info_t info = {};
        esp_audio_dec_get_info(pInstance->handle, &info);

        pData->fmt.sample_rate    = info.sample_rate   ? info.sample_rate   : 44100;
        pData->fmt.bits_per_sample = info.bits_per_sample ? info.bits_per_sample : 16;
        pData->fmt.channels       = info.channel        ? info.channel        : 2;
        pData->frame_count = out_frame.decoded_size /
                             (pData->fmt.channels * (pData->fmt.bits_per_sample / 8));
        return DECODE_STATUS_CONTINUE;
    }

    pData->frame_count = 0;
    return DECODE_STATUS_NO_DATA_CONTINUE;
}
