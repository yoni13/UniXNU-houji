/* RAM-only upload candidate. Caller binds an owned object; no physical addresses
 * or allocation sizes are accepted from the host. Single worker owns this state. */
#ifndef HOUJI_RAM_UPLOAD_H
#define HOUJI_RAM_UPLOAD_H
#ifdef HOUJI_UPLOAD_HOST_TEST
#include <stdint.h>
#else
#include <sys/types.h>
#endif

#define HRU_HEADER 40u
#define HRU_CHUNK 65536u
#define HRU_RESPONSE_MAX (HRU_HEADER+32u)
#define HRU_MAX_OBJECT (16ULL<<30)
enum hru_kind { HRU_BEGIN=1, HRU_DATA=2, HRU_END=3,
    HRU_READY=128, HRU_ACK=129, HRU_DONE=130, HRU_ERROR=255 };
enum hru_phase { HRU_UNBOUND, HRU_BEGIN_WAIT, HRU_RECEIVING, HRU_VERIFYING,
    HRU_COMPLETE, HRU_FAILED };
enum hru_error { HRU_E_HEADER=1, HRU_E_ORDER, HRU_E_LENGTH, HRU_E_CRC,
    HRU_E_HASH, HRU_E_WRITE, HRU_E_READ, HRU_E_STATE, HRU_E_EMIT,
    HRU_E_TRANSPORT };
struct hru_sha256 { uint32_t h[8]; uint64_t bytes; unsigned used; uint8_t block[64]; };
struct hru_target {
    uint64_t bytes;
    uint8_t sha256[32];
    void *context;
    /* Return 0 only after the entire bounded range has been copied. */
    int (*write)(void *,uint64_t,const uint8_t *,unsigned);
    int (*read)(void *,uint64_t,uint8_t *,unsigned);
    /* Must copy synchronously; frame is valid only during this call. */
    int (*emit)(void *,const uint8_t *,unsigned);
};
struct hru_receiver {
    struct hru_target target;
    enum hru_phase phase;
    unsigned error,header_used,payload_used,payload_length,kind,ready_sent,padding;
    uint32_t next_sequence,frame_sequence,end_sequence;
    uint64_t received,verified;
    struct hru_sha256 stream_hash,readback_hash;
    uint8_t header[HRU_HEADER],payload[HRU_CHUNK];
};

uint32_t hru_crc32(const uint8_t *,unsigned);
void hru_sha_init(struct hru_sha256 *);
void hru_sha_update(struct hru_sha256 *,const uint8_t *,unsigned);
void hru_sha_final(struct hru_sha256 *,uint8_t [32]);
unsigned hru_frame(uint8_t *,unsigned,uint8_t,uint32_t,uint64_t,uint64_t,const uint8_t *,unsigned);
int hru_bind(struct hru_receiver *,const struct hru_target *);
int hru_ready(struct hru_receiver *);
void hru_feed(struct hru_receiver *,const uint8_t *,unsigned);
void hru_verify_step(struct hru_receiver *,uint8_t *,unsigned);
void hru_abort(struct hru_receiver *,unsigned);

/* Kernel adapter contract: bind once before starting CDC. No default target.
 * Result: 0 pending/unbound, 1 validated RAM object, -1 failed. Root must not be
 * published before 1. Transport loss after COMPLETE does not alter valid RAM. */
#ifndef HOUJI_UPLOAD_HOST_TEST
int houji_ram_upload_bind(const struct hru_target *);
int houji_ram_upload_result(void);
#endif
#endif
