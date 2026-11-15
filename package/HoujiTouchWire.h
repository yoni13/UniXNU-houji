/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded TouchCom v1 transport. Hardware adapters provide complete SPI
 * transactions. v2, CRC extensions and fragmented large reports are refused
 * until their wire format has been verified on this device.
 */
#ifndef GIULIA_TOUCH_WIRE_H
#define GIULIA_TOUCH_WIRE_H
#include <stddef.h>
#include <stdint.h>
#define GW_MAX 512
struct gw_bus {
    int (*read)(uint8_t *,unsigned);
    int (*write)(const uint8_t *,unsigned);
    void (*wait)(unsigned);
    unsigned max_write;
    int (*discard_report)(unsigned);
};
struct gw_message { uint8_t code; unsigned size; uint8_t data[GW_MAX]; };
static inline unsigned gw_le16(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
static inline int gw_read(struct gw_bus *bus,struct gw_message *out)
{
    uint8_t h[4],tail[GW_MAX+7];
    if(bus->read(h,4))return 1;
    if(h[0]!=0xa5)return 2;
    unsigned n=gw_le16(h+2);
    if(h[1]==0 || h[1]==2 || h[1]==0xff) {
        out->code=h[1];out->size=0;return 0;
    }
    if(h[1]==3)return 3;
    if(n>GW_MAX) {
        // Large unsolicited diagnostic/raw frames are never contact data.
        // Adapter validates their complete continuation and EOM in fixed memory.
        if(h[1]<0x12 || !bus->discard_report || bus->discard_report(n))return 3;
        out->code=h[1];out->size=0;return 0;
    }
    if(n) {
        /* The vendor detects optional CRC/RC by reading four extra bytes.
         * Legacy v1 fills them with EOM 0x5a. Never interpret extensions as
         * contact data or send a legacy command after detecting them. */
        if(bus->read(tail,n+7))return 4;
        if(tail[0]!=0xa5 || tail[1]!=3)return 5;
        for(unsigned i=n+2;i<n+7;i++)if(tail[i]!=0x5a)return 6;
    }
    out->code=h[1];out->size=n;
    for(unsigned i=0;i<n;i++)out->data[i]=tail[i+2];
    return 0;
}
static inline int gw_command(struct gw_bus *bus,uint8_t cmd,
    const uint8_t *payload,unsigned n,struct gw_message *reply)
{
    uint8_t packet[GW_MAX+3];
    if(n>GW_MAX || n+3>bus->max_write || (n && !payload))return 7;
    packet[0]=cmd;packet[1]=(uint8_t)n;packet[2]=(uint8_t)(n>>8);
    for(unsigned i=0;i<n;i++)packet[i+3]=payload[i];
    if(bus->write(packet,n+3))return 8;
    for(unsigned attempt=0;attempt<100;attempt++) {
        bus->wait(10);
        int e=gw_read(bus,reply);if(e)return e;
        if(reply->code==1 || (cmd==2 && reply->code==0x10))return 0;
        if(reply->code==0 || reply->code==2 || reply->code==0xff || reply->code>=0x10)continue;
        return 9;
    }
    return 10;
}
#endif
