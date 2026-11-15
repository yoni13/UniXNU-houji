/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded decoder for an explicitly installed TouchCom contact report format.
 * Protocol constants: OnePlusOSS Synaptics S3910 TouchCom source, pinned in
 * hardware/sm8650-touch.json. This implementation performs no hardware I/O.
 * A transport must validate/reassemble its wire message and confirm the exact
 * report configuration before calling this decoder. HBP raw frames are refused.
 */
#ifndef GIULIA_TOUCH_REPORT_H
#define GIULIA_TOUCH_REPORT_H
#include <stddef.h>
#include <stdint.h>

#define GT_CONTACTS 10
#define GT_WIDTH 1200
#define GT_HEIGHT 2670
#define GT_MASK ((1U << GT_CONTACTS)-1)

/* Count8; for each active object: index8, classification8, X16, Y16. */
static const uint8_t gt_report_config[] = {
    0x18,8,0x01,0x06,8,0x07,8,0x08,16,0x09,16,0x03,0x00
};
struct gt_state {
    uint16_t active;
    uint16_t x[GT_CONTACTS], y[GT_CONTACTS];
};
struct gt_event { uint8_t id, down; uint16_t x, y; };
struct gt_frame { uint16_t active; unsigned count; struct gt_event events[GT_CONTACTS]; };

static inline int gt_config_matches(const uint8_t *config,size_t size)
{
    if (!config || size<sizeof(gt_report_config) || size>512) return 0;
    for(size_t i=0;i<sizeof(gt_report_config);i++) if(config[i]!=gt_report_config[i]) return 0;
    /* The application may return its complete fixed-size configuration buffer.
     * Only zero END padding is accepted after our exact descriptor. */
    for(size_t i=sizeof(gt_report_config);i<size;i++) if(config[i]) return 0;
    return 1;
}

/* Returns 0 on a complete frame, nonzero without changing state/output.
 * max_x/max_y must come from the device's application-info response, not a
 * guessed coordinate scale. Coordinates passed to HID are physical pixels;
 * SweetHID normalizes over the same full panel used by the display renderer.
 */
static inline int gt_decode(uint8_t report_code,const uint8_t *data,size_t size,
    uint16_t max_x,uint16_t max_y,int config_confirmed,
    struct gt_state *state,struct gt_frame *out)
{
    if(!state || !out || !data || !config_confirmed || report_code!=0x11 ||
       !max_x || !max_y || size<1 || (state->active&~GT_MASK)) return 1;
    unsigned count=data[0];
    if(count>GT_CONTACTS || size!=1+6*count) return 2;
    struct gt_state next={0};
    struct gt_frame frame={0};
    unsigned seen=0;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *p=data+1+6*i;
        unsigned id=p[0],kind=p[1];
        unsigned x=(unsigned)p[2]|(unsigned)p[3]<<8;
        unsigned y=(unsigned)p[4]|(unsigned)p[5]<<8;
        if(id>=GT_CONTACTS || (seen&(1U<<id))) return 3;
        seen|=1U<<id;
        if(kind>9 || kind==7 || x>max_x || y>max_y) return 4;
        /* Only fingers/gloves become touches. Lift, palm, hover and pen
         * objects do not become synthetic finger presses. */
        if(kind!=1 && kind!=2) continue;
        next.active|=(uint16_t)(1U<<id);
        next.x[id]=(uint16_t)((uint32_t)x*(GT_WIDTH-1)/max_x);
        next.y[id]=(uint16_t)((uint32_t)y*(GT_HEIGHT-1)/max_y);
    }
    frame.active=next.active;
    for(unsigned id=0;id<GT_CONTACTS;id++) {
        unsigned bit=1U<<id;
        if(!((state->active|next.active)&bit)) continue;
        struct gt_event *e=&frame.events[frame.count++];
        e->id=(uint8_t)id;e->down=!!(next.active&bit);
        e->x=e->down?next.x[id]:state->x[id];
        e->y=e->down?next.y[id]:state->y[id];
    }
    *state=next;*out=frame;
    return 0;
}
#endif
