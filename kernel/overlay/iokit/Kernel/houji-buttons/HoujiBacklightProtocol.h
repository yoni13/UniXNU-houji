/* SPDX-License-Identifier: APSL-2.0 (from lonzzi/UniXNU-houji 99beb10, kernel/overlay/iokit/Kernel/arm)
 * DSI FIFO approach adapted from data-a-live patches 0139/0140/0263.
 * See kernel/APPLE_LICENSE. Registers follow Qualcomm dsi_ctrl_hw_cmn;
 * houji n3 panel uses high-byte-first DCS 0x51, normal maximum 0xb32.
 */
#ifndef HOUJI_BACKLIGHT_PROTOCOL_H
#define HOUJI_BACKLIGHT_PROTOCOL_H
#include <stdint.h>
enum { HBL_MIN=25, HBL_MAX=2866, HBL_SCALE=65536 };
// DSI_INT_CTRL interleaves W1C status bits with interrupt enable bits.
// Match MiCode's shennong-u-oss controller mask; writing back a sampled
// stream/refresh status bit would acknowledge another display owner's event.
static const uint32_t HBL_DSI_STATUS_BITS=0x55115501u;
static inline bool houjiBacklightReadback(uint32_t control,uint32_t packet,unsigned *level) {
    unsigned bytes=control>>16;
    if((bytes!=4 && bytes!=8) || (packet>>24)!=0x22)return false;
    unsigned value=(packet>>8)&0xffff;
    if(value>4095)return false;
    *level=value;return true;
}
static inline unsigned houjiBacklightLevel(unsigned fraction) {
    if(!fraction)return 0;
    if(fraction>HBL_SCALE)fraction=HBL_SCALE;
    unsigned level=(uint64_t)fraction*fraction*HBL_MAX/((uint64_t)HBL_SCALE*HBL_SCALE);
    return level<HBL_MIN?HBL_MIN:level;
}
static inline unsigned houjiBacklightFraction(unsigned level) {
    if(!level)return 0;
    if(level>=HBL_MAX)return HBL_SCALE;
    unsigned lo=1,hi=HBL_SCALE;
    while(lo<hi){unsigned m=(lo+hi)/2;if(houjiBacklightLevel(m)<level)lo=m+1;else hi=m;}
    return lo;
}
static inline unsigned houjiBacklightFade(unsigned from,unsigned to,uint64_t elapsed,uint64_t duration) {
    if(!duration || elapsed>=duration)return to;
    int64_t step=((int64_t)to-from)*(int64_t)elapsed/(int64_t)duration;
    return (unsigned)((int64_t)from+step);
}
struct HoujiDsiIo {
    void *cookie;
    uint32_t (*read)(void *,unsigned);
    void (*write)(void *,unsigned,uint32_t);
};
struct HoujiDsiCommand {
    HoujiDsiIo io; bool pending,failed; uint64_t deadline;
    uint32_t dma,schedule,length,tpg;
    uint32_t read(unsigned r){return io.read(io.cookie,r);}
    void write(unsigned r,uint32_t v){io.write(io.cookie,r,v);}
    bool begin(const uint32_t *words,unsigned count,uint64_t now,bool receive=false) {
        if(pending || failed || !words || !count || count>2)return false;
        // Enabled command mode; no other command-DMA/FIFO owner.
        if((read(4)&5)!=5 || (read(8)&2) || read(0x15c) || (read(0x110)&(1u<<24)))return false;
        dma=read(0x3c);schedule=read(0x390);length=read(0x4c);tpg=read(0x15c);
        write(0x110,(read(0x110)&~HBL_DSI_STATUS_BITS)|1u);
        // Clear both the previous bytes and count before requesting a reply.
        // A DMA-done interrupt alone does not certify fresh panel data.
        if(receive){write(0x1d4,1);write(0x1d4,0);}
        write(0x15c,0x30006);
        for(unsigned i=0;i<count;++i)write(0x17c,words[i]);
        // The FIFO consumes pairs; the programmed byte count excludes padding.
        if(count&1)write(0x17c,0);
        write(0x3c,(dma&0x2bffffffu)|0x10000000u);
        write(0x390,schedule&~2u);write(0x4c,count*4);
        pending=true;deadline=now+50000;write(0x90,1);return true;
    }
    // 0 pending, 1 success, -1 terminal error. No spin loop or sleep here.
    int poll(uint64_t now) {
        if(failed)return -1;
        if(!pending)return 1;
        uint32_t status=read(0x110);
        if((status&(1u<<24)) || (!(status&1u) && now>=deadline)) {
            failed=true;pending=false;return -1;
        }
        if(!(status&1u))return 0;
        write(0x110,(status&~HBL_DSI_STATUS_BITS)|1u);
        write(0x15c,0);write(0x1ec,1);write(0x1ec,0);
        write(0x3c,dma);write(0x390,schedule);write(0x4c,length);write(0x15c,tpg);
        pending=false;return 1;
    }
    bool brightness(unsigned level,uint64_t now) {
        if(level>HBL_MAX)return false;
        const uint32_t words[]={0xc0390003u,0xff000051u|((level&255)<<16)|((level>>8)<<8)};
        return begin(words,2,now);
    }
    bool readBrightness(uint64_t now) {
        const uint32_t word=0xa0060052u;
        return begin(&word,1,now,true);
    }
};
#endif
