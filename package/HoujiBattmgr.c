/* SPDX-License-Identifier: BSD-2-Clause-Patent */
// Read-only subset used by battery=4/5. No SET opcode is emitted.
#include "HoujiBattmgr.h"

static const struct {uint8_t opcode, property;} kFields[HoujiBmFields] = {
    [HoujiBmStatus] = {HOUJI_BATTMGR_OP_BATTERY_GET, 0},
    [HoujiBmPresent] = {HOUJI_BATTMGR_OP_BATTERY_GET, 2},
    [HoujiBmCapacity] = {HOUJI_BATTMGR_OP_BATTERY_GET, 4},
    [HoujiBmVoltage] = {HOUJI_BATTMGR_OP_BATTERY_GET, 7},
    [HoujiBmTemperature] = {HOUJI_BATTMGR_OP_BATTERY_GET, 13},
    [HoujiBmUsbOnline] = {HOUJI_BATTMGR_OP_USB_GET, 0},
    [HoujiBmUsbType] = {HOUJI_BATTMGR_OP_USB_GET, 6},
    [HoujiBmUsbVoltage] = {HOUJI_BATTMGR_OP_USB_GET, 1},
    [HoujiBmUsbCurrent] = {HOUJI_BATTMGR_OP_USB_GET, 3},
    [HoujiBmFull] = {HOUJI_BATTMGR_OP_BATTERY_GET, 18},
    [HoujiBmDesign] = {HOUJI_BATTMGR_OP_BATTERY_GET, 17},
    [HoujiBmCycles] = {HOUJI_BATTMGR_OP_BATTERY_GET, 16},
};
#define NO_CYCLE 0xffu
static void put32(uint8_t *p, uint32_t v) {p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void header(uint8_t *p, uint32_t op) {put32(p,HOUJI_BATTMGR_OWNER);put32(p+4,HOUJI_BATTMGR_TYPE_REQUEST);put32(p+8,op);}

void houji_battmgr_init(HoujiBattmgr *b) {for(unsigned i=0;i<sizeof *b;++i)((uint8_t*)b)[i]=0;b->cursor=NO_CYCLE;}
uint32_t houji_battmgr_enable_request(uint8_t out[HOUJI_BATTMGR_ENABLE_BYTES]) {
    header(out,HOUJI_BATTMGR_OP_REQUEST_NOTIFICATION);put32(out+12,0);put32(out+16,0);put32(out+20,1);put32(out+24,100);return HOUJI_BATTMGR_ENABLE_BYTES;
}
void houji_battmgr_refresh(HoujiBattmgr *b) {if(b->cursor!=NO_CYCLE){b->refresh_wanted=1;return;}b->cursor=0;b->in_flight=0;}
uint32_t houji_battmgr_next(HoujiBattmgr *b,uint8_t out[HOUJI_BATTMGR_REQUEST_BYTES]) {
    if(b->in_flight||b->cursor==NO_CYCLE||b->cursor>=HoujiBmFields)return 0;
    header(out,kFields[b->cursor].opcode);put32(out+12,0);put32(out+16,kFields[b->cursor].property);put32(out+20,0);b->in_flight=1;return HOUJI_BATTMGR_REQUEST_BYTES;
}
static int advance(HoujiBattmgr *b) {b->in_flight=0;if(++b->cursor<HoujiBmFields)return HoujiBmAnswer;b->cursor=NO_CYCLE;b->cycles_done++;if(b->refresh_wanted){b->refresh_wanted=0;houji_battmgr_refresh(b);}return HoujiBmCycleDone;}
void houji_battmgr_timeout(HoujiBattmgr *b) {if(b->in_flight){b->timeouts++;b->valid&=~(1u<<b->cursor);advance(b);}}
int houji_battmgr_receive(HoujiBattmgr *b,const uint8_t *data,uint32_t length) {
    if(length<12||get32(data)!=HOUJI_BATTMGR_OWNER){b->ignored++;return HoujiBmIgnored;}
    const uint32_t type=get32(data+4),op=get32(data+8);
    if(op==HOUJI_BATTMGR_OP_NOTIFICATION){b->notifications++;houji_battmgr_refresh(b);return HoujiBmNotified;}
    if(type!=HOUJI_BATTMGR_TYPE_REQUEST){b->ignored++;return HoujiBmIgnored;}
    if(op==HOUJI_BATTMGR_OP_REQUEST_NOTIFICATION){b->enabled=1;return HoujiBmEnabled;}
    if(b->cursor==NO_CYCLE||b->cursor>=HoujiBmFields||!b->in_flight||length<24||op!=kFields[b->cursor].opcode||get32(data+12)!=kFields[b->cursor].property){b->ignored++;return HoujiBmIgnored;}
    b->responses++;if(get32(data+20)){b->errors++;b->valid&=~(1u<<b->cursor);}else{b->value[b->cursor]=(int32_t)get32(data+16);b->valid|=1u<<b->cursor;}return advance(b);
}
static uint32_t family(int type){switch(type){case 1:return 0xe0004000u;case 2:return 0xe0004004u;case 3:return 0xe0004005u;case 5:return 0xe0004009u;case 6:case 7:case 8:return 0xe000400au;default:return 0xe0004007u;}}
void houji_battmgr_snapshot(const HoujiBattmgr *b,HoujiBatterySnapshot *s) {
#define HAS(f) ((b->valid>>(f))&1u)
#define V(f) (b->value[(f)])
    for(unsigned i=0;i<sizeof *s;++i)((uint8_t*)s)[i]=0;
    const int status=HAS(HoujiBmStatus)?V(HoujiBmStatus):0;
    s->present=HAS(HoujiBmPresent)?V(HoujiBmPresent)!=0:HAS(HoujiBmCapacity);s->external=HAS(HoujiBmUsbOnline)&&V(HoujiBmUsbOnline)!=0;
    s->percent=HAS(HoujiBmCapacity)?(V(HoujiBmCapacity)+50)/100:0;if(s->percent<0)s->percent=0;if(s->percent>100)s->percent=100;s->charge_known=HAS(HoujiBmCapacity);
    s->charging=s->external&&status==1;s->full=s->external&&(status==4||s->percent==100);s->critical=s->present&&!s->external&&s->percent<=2;
    s->voltage_mv=HAS(HoujiBmVoltage)?V(HoujiBmVoltage)/1000:0;s->temperature_valid=HAS(HoujiBmTemperature)&&V(HoujiBmTemperature)>=-4000&&V(HoujiBmTemperature)<=9000;s->temperature=s->temperature_valid?V(HoujiBmTemperature):0;
    s->full_mah=HAS(HoujiBmFull)?V(HoujiBmFull)/1000:0;s->design_mah=HAS(HoujiBmDesign)?V(HoujiBmDesign)/1000:0;s->cycles=HAS(HoujiBmCycles)?V(HoujiBmCycles):0;
    if(s->external){s->adapter_family=family(HAS(HoujiBmUsbType)?V(HoujiBmUsbType):0);s->adapter_mv=HAS(HoujiBmUsbVoltage)?V(HoujiBmUsbVoltage)/1000:0;s->adapter_ma=HAS(HoujiBmUsbCurrent)?V(HoujiBmUsbCurrent)/1000:0;if(s->adapter_mv<0)s->adapter_mv=0;if(s->adapter_ma<0)s->adapter_ma=-s->adapter_ma;s->adapter_watts=(int)(((int64_t)s->adapter_mv*s->adapter_ma+500000)/1000000);}
#undef HAS
#undef V
}
