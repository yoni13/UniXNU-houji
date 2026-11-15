#include "houji_ram_upload.h"
#ifdef HOUJI_UPLOAD_HOST_TEST
#include <string.h>
#else
#include <sys/systm.h>
#endif

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t get64(const uint8_t *p)
{ return (uint64_t)get32(p)|((uint64_t)get32(p+4)<<32); }
static void put32(uint8_t *p,uint32_t x)
{ for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i)); }
static void put64(uint8_t *p,uint64_t x)
{ put32(p,(uint32_t)x);put32(p+4,(uint32_t)(x>>32)); }
uint32_t hru_crc32(const uint8_t *p,unsigned n)
{
    uint32_t c=0xffffffffu;
    for(unsigned i=0;i<n;i++) {
        c^=p[i];
        for(unsigned j=0;j<8;j++) c=(c>>1)^(0xedb88320u&(0u-(c&1u)));
    }
    return ~c;
}
static uint32_t rr(uint32_t x,unsigned n) { return (x>>n)|(x<<(32-n)); }
static void sha_block(struct hru_sha256 *s,const uint8_t *p)
{
    static const uint32_t k[64]={
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for(unsigned i=0;i<16;i++) w[i]=((uint32_t)p[4*i]<<24)|((uint32_t)p[4*i+1]<<16)|((uint32_t)p[4*i+2]<<8)|p[4*i+3];
    for(unsigned i=16;i<64;i++) w[i]=w[i-16]+(rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10));
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for(unsigned i=0;i<64;i++) {
        uint32_t t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
void hru_sha_init(struct hru_sha256 *s)
{
    static const uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memset(s,0,sizeof(*s));memcpy(s->h,h,sizeof(h));
}
void hru_sha_update(struct hru_sha256 *s,const uint8_t *p,unsigned n)
{
    s->bytes+=n;
    while(n) {
        unsigned take=64-s->used;if(take>n) take=n;
        memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;
        if(s->used==64) { sha_block(s,s->block);s->used=0; }
    }
}
void hru_sha_final(struct hru_sha256 *s,uint8_t out[32])
{
    uint64_t bits=s->bytes*8;
    s->block[s->used++]=0x80;
    if(s->used>56) { memset(s->block+s->used,0,64-s->used);sha_block(s,s->block);s->used=0; }
    memset(s->block+s->used,0,56-s->used);
    for(unsigned i=0;i<8;i++) s->block[63-i]=(uint8_t)(bits>>(8*i));
    sha_block(s,s->block);
    for(unsigned i=0;i<8;i++) for(unsigned j=0;j<4;j++) out[4*i+j]=(uint8_t)(s->h[i]>>(24-8*j));
}
unsigned hru_frame(uint8_t *out,unsigned capacity,uint8_t kind,uint32_t seq,
    uint64_t offset,uint64_t object_bytes,const uint8_t *payload,unsigned n)
{
    unsigned padding=((HRU_HEADER+n)%64u)==0u;
    if(n>HRU_CHUNK || capacity<HRU_HEADER || n>capacity-HRU_HEADER ||
        padding>capacity-HRU_HEADER-n || (!payload && n)) return 0;
    memset(out,0,HRU_HEADER);memcpy(out,"HUR1",4);out[4]=1;out[5]=kind;
    put32(out+8,seq);put32(out+12,n);put64(out+16,offset);put64(out+24,object_bytes);
    put32(out+32,hru_crc32(payload,n));put32(out+36,hru_crc32(out,36));
    if(n) memcpy(out+HRU_HEADER,payload,n);
    if(padding) out[HRU_HEADER+n]=0;
    return HRU_HEADER+n+padding;
}
static int response(struct hru_receiver *s,uint8_t kind,uint32_t seq,const uint8_t *p,unsigned n)
{
    uint8_t frame[HRU_RESPONSE_MAX];
    unsigned bytes=hru_frame(frame,sizeof(frame),kind,seq,s->received,s->target.bytes,p,n);
    return bytes?s->target.emit(s->target.context,frame,bytes):-1;
}
void hru_abort(struct hru_receiver *s,unsigned code)
{
    if(s->phase==HRU_FAILED || s->phase==HRU_UNBOUND) return;
    s->phase=HRU_FAILED;s->error=code;
    uint8_t error[4];put32(error,code);
    (void)response(s,HRU_ERROR,s->next_sequence,error,4);
}
int hru_bind(struct hru_receiver *s,const struct hru_target *target)
{
    if(!s || !target || s->phase!=HRU_UNBOUND || !target->bytes ||
        target->bytes>HRU_MAX_OBJECT || !target->write || !target->read || !target->emit) return -1;
    /* The receiver must be zero-initialized and must never be rebound. */
    memset(s,0,sizeof(*s));s->target=*target;s->phase=HRU_BEGIN_WAIT;return 0;
}
int hru_ready(struct hru_receiver *s)
{
    if(s->phase!=HRU_BEGIN_WAIT || s->header_used || s->next_sequence || s->ready_sent) return -1;
    s->ready_sent=1;
    if(response(s,HRU_READY,0,s->target.sha256,32)) { hru_abort(s,HRU_E_EMIT);return -1; }
    return 0;
}
static int header(struct hru_receiver *s)
{
    const uint8_t *h=s->header;
    if(memcmp(h,"HUR1",4) || h[4]!=1 || h[6] || h[7] || get32(h+36)!=hru_crc32(h,36)) {
        hru_abort(s,HRU_E_HEADER);return 0;
    }
    s->kind=h[5];s->frame_sequence=get32(h+8);s->payload_length=get32(h+12);
    if(s->frame_sequence!=s->next_sequence || s->next_sequence==0xffffffffu ||
        get64(h+16)!=s->received || get64(h+24)!=s->target.bytes) {
        hru_abort(s,HRU_E_ORDER);return 0;
    }
    if(s->payload_length>HRU_CHUNK) { hru_abort(s,HRU_E_LENGTH);return 0; }
    s->padding=((HRU_HEADER+s->payload_length)%64u)==0u;
    if(s->phase==HRU_BEGIN_WAIT) {
        if(s->kind!=HRU_BEGIN || s->payload_length!=32 || s->received) { hru_abort(s,HRU_E_STATE);return 0; }
    } else if(s->phase==HRU_RECEIVING) {
        if(s->kind==HRU_DATA) {
            if(!s->payload_length || s->payload_length>s->target.bytes-s->received) { hru_abort(s,HRU_E_LENGTH);return 0; }
        } else if(s->kind==HRU_END) {
            if(s->payload_length || s->received!=s->target.bytes) { hru_abort(s,HRU_E_LENGTH);return 0; }
        } else { hru_abort(s,HRU_E_STATE);return 0; }
    } else { hru_abort(s,HRU_E_STATE);return 0; }
    return 1;
}
static void frame_complete(struct hru_receiver *s)
{
    if(get32(s->header+32)!=hru_crc32(s->payload,s->payload_length)) { hru_abort(s,HRU_E_CRC);return; }
    if(s->kind==HRU_BEGIN) {
        if(memcmp(s->payload,s->target.sha256,32)) { hru_abort(s,HRU_E_HASH);return; }
        hru_sha_init(&s->stream_hash);s->phase=HRU_RECEIVING;
    } else if(s->kind==HRU_DATA) {
        if(s->target.write(s->target.context,s->received,s->payload,s->payload_length)) { hru_abort(s,HRU_E_WRITE);return; }
        hru_sha_update(&s->stream_hash,s->payload,s->payload_length);s->received+=s->payload_length;
    } else {
        uint8_t digest[32];hru_sha_final(&s->stream_hash,digest);
        if(memcmp(digest,s->target.sha256,32)) { hru_abort(s,HRU_E_HASH);return; }
        s->end_sequence=s->frame_sequence;s->phase=HRU_VERIFYING;hru_sha_init(&s->readback_hash);
    }
    s->next_sequence++;s->header_used=s->payload_used=0;
    if(s->phase!=HRU_VERIFYING && response(s,HRU_ACK,s->frame_sequence,NULL,0)) hru_abort(s,HRU_E_EMIT);
}
void hru_feed(struct hru_receiver *s,const uint8_t *p,unsigned n)
{
    if(!n) return;
    if(!s->ready_sent || (s->phase!=HRU_BEGIN_WAIT && s->phase!=HRU_RECEIVING)) { hru_abort(s,HRU_E_STATE);return; }
    while(n && s->phase!=HRU_FAILED) {
        if(s->phase!=HRU_BEGIN_WAIT && s->phase!=HRU_RECEIVING) { hru_abort(s,HRU_E_STATE);return; }
        if(s->header_used<HRU_HEADER) {
            unsigned take=HRU_HEADER-s->header_used;if(take>n) take=n;
            memcpy(s->header+s->header_used,p,take);s->header_used+=take;p+=take;n-=take;
            if(s->header_used<HRU_HEADER) continue;
            if(!header(s)) return;
        }
        unsigned take=s->payload_length-s->payload_used;if(take>n) take=n;
        if(take) memcpy(s->payload+s->payload_used,p,take);
        s->payload_used+=take;p+=take;n-=take;
        if(s->payload_used==s->payload_length) {
            if(s->padding) {
                if(!n) continue;
                if(*p) { hru_abort(s,HRU_E_HEADER);return; }
                p++;n--;s->padding=0;
            }
            frame_complete(s);
        }
    }
}
void hru_verify_step(struct hru_receiver *s,uint8_t *scratch,unsigned capacity)
{
    if(s->phase!=HRU_VERIFYING) return;
    if(!scratch || !capacity || capacity>HRU_CHUNK) { hru_abort(s,HRU_E_READ);return; }
    uint64_t remaining=s->target.bytes-s->verified;
    unsigned n=remaining<capacity?(unsigned)remaining:capacity;
    if(s->target.read(s->target.context,s->verified,scratch,n)) { hru_abort(s,HRU_E_READ);return; }
    hru_sha_update(&s->readback_hash,scratch,n);s->verified+=n;
    if(s->verified==s->target.bytes) {
        uint8_t digest[32];hru_sha_final(&s->readback_hash,digest);
        if(memcmp(digest,s->target.sha256,32)) { hru_abort(s,HRU_E_HASH);return; }
        s->phase=HRU_COMPLETE;
        if(response(s,HRU_DONE,s->end_sequence,digest,32)) hru_abort(s,HRU_E_EMIT);
    }
}
