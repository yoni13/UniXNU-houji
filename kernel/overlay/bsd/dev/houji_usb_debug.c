/* Experimental houji CDC ACM console. Opt-in: houji_usb=1.
 * Reuses ABL's PHY/clocks; never changes GCC, SMMU, PMIC or storage.
 * DWC3 register/endpoint protocol: Linux drivers/usb/dwc3, reference0073.
 * USB can read the RAM console ring and issue four bounded queries only.
 * All controller memory is permanent: a timeout never frees DMA memory.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "houji_usb_protocol.h"
#include "houji_ram_upload.h"
#include <machine/atomic.h>
#include <sys/reboot.h>
extern int reboot_kernel(int, char *);

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern unsigned houji_debug_copy(uint64_t *, char *, unsigned);
extern uint64_t houji_debug_total(void);
extern uint64_t houji_debug_dropped(void);
extern const char *houji_smmu_route_stream(uint16_t, uint64_t, uint64_t, uint32_t *);
void houji_usb_debug_start(void);
void houji_usb_report(void);
/* Early USB lines leave the rolling log long before a checkpoint; keep them. */
static char usb_notes[3072];
static unsigned usb_notes_n;
static void note_add(const char *line,int n)
{
    if (n<=0) return;
    if ((unsigned)n>=sizeof(usb_notes)-usb_notes_n) return;
    memcpy(usb_notes+usb_notes_n,line,(unsigned)n);usb_notes_n+=(unsigned)n;usb_notes[usb_notes_n]=0;
}
#define unote(...) do { char _l[200]; int _n=snprintf(_l,sizeof(_l),__VA_ARGS__); \
    printf("%s",_l); note_add(_l,_n<(int)sizeof(_l)?_n:(int)sizeof(_l)-1); } while (0)

#define GCTL 0xc110
#define GEVNTADRLO 0xc400
#define GEVNTADRHI 0xc404
#define GEVNTSIZ 0xc408
#define GEVNTCOUNT 0xc40c
#define DCFG 0xc700
#define DCTL 0xc704
#define DEVTEN 0xc708
#define DSTS 0xc70c
#define DALEPENA 0xc720
#define EPBASE(n) (0xc800+16*(n))
#define RUN (1u<<31)
#define RESET (1u<<30)
#define HALTED (1u<<22)
#define ACTIVE (1u<<10)
#define FORCE_REMOVE (1u<<11)
#define SET_CONFIG 1
#define SET_RESOURCE 2
#define STALL 4
#define CLEAR_STALL 5
#define START 6
#define END 8
#define START_CONFIG 9
#define T_NORMAL (1u<<4)
#define T_SETUP (2u<<4)
#define T_STATUS2 (3u<<4)
#define T_STATUS3 (4u<<4)
#define T_DATA (5u<<4)
#define HWO 1u
#define TRB_FLAGS (HWO|2u|(1u<<10)|(1u<<11))
#define EVENTS 0
#define EVENT_SIZE 4096
/* Separate cache lines: hardware never shares a dirty CPU-owned TRB line. */
#define SETUP 0x1200
#define DATA 0x1300
#define NOTIFY 0x1500
#define RX 0x4000
#define TX 0x2000
#define RX_SIZE 65536
#define TX_SIZE 2048
#define DMA_SIZE 0x20000
_Static_assert(RX+RX_SIZE<=DMA_SIZE,"USB RX exceeds DMA allocation");
_Static_assert(TX+TX_SIZE<=RX,"USB RX/TX overlap");
_Static_assert((RX&63)==0,"USB RX cache alignment");
static const unsigned trb_off[6]={0x1000,0x1040,0x1080,0x10c0,0x1100,0x1140};
static const char *const usb_strings[]={NULL,"Houji Lab","Xiaomi 14 XNU Debug","HOUJIDBG001"};
static struct {
    vm_offset_t regs, dma, smmu;
    uint64_t pa, cursor, tx_bytes, rx_bytes;
    uint32_t pos, events, setups, mps;
    unsigned resource[6], requested[6];
    bool busy[6], failed, configured, dtr, notify;
    enum { SETUP_PHASE, DATA_PHASE, WAIT2, WAIT3, STATUS_PHASE } phase;
    bool data_in, set_line;
    uint8_t coding[7];
    char command[64];
    unsigned command_n;
    bool command_overflow;
    int sleep_channel;
} u;


/* The root owner must bind before USB startup. No default RAM object exists. */
static struct hru_receiver upload;
static bool upload_mode,upload_binding_closed,upload_done_sent;
static uint8_t upload_ack[HRU_RESPONSE_MAX];
static unsigned upload_ack_length;
static uint64_t upload_deadline;
static int _Atomic upload_public_result;
static int upload_emit(void *context __unused,const uint8_t *frame,unsigned n)
{
    if (upload_ack_length || n>sizeof(upload_ack)) return -1;
    memcpy(upload_ack,frame,n);upload_ack_length=n;return 0;
}
static void upload_publish(void)
{
    int value=upload.phase==HRU_COMPLETE?1:upload.phase==HRU_FAILED?-1:0;
    os_atomic_store(&upload_public_result,value,release);
}
int houji_ram_upload_bind(const struct hru_target *target)
{
    if (upload_binding_closed || !target) return -1;
    struct hru_target owned=*target;
    owned.emit=upload_emit;
    return hru_bind(&upload,&owned);
}
int houji_ram_upload_result(void)
{
    return os_atomic_load(&upload_public_result,acquire);
}
static void upload_timeout_after(unsigned seconds)
{
    clock_interval_to_deadline(seconds,NSEC_PER_SEC,&upload_deadline);
}
static void upload_feed(const uint8_t *p,unsigned n)
{
    uint32_t before=upload.next_sequence;
    hru_feed(&upload,p,n);
    if (upload.next_sequence!=before) upload_timeout_after(upload.phase==HRU_VERIFYING?600:30);
    upload_publish();
}

static uint32_t read_reg(vm_offset_t base,unsigned off)
{
    uint32_t v=*(volatile uint32_t *)(base+off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static uint32_t rd(unsigned off) { return read_reg(u.regs,off); }
static void wr(unsigned off,uint32_t v)
{
    *(volatile uint32_t *)(u.regs+off)=v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void sync_dma(unsigned off,unsigned size)
{
    flush_dcache64((addr64_t)(u.pa+off),size,TRUE);
    __asm__ volatile("dsb sy" ::: "memory");
}
static void fail(const char *why)
{
    if (!u.failed) unote("HOUJI USB: stopped: %s; permanent DMA retained\n",why);
    u.failed=true;
    if (upload_mode && upload.phase!=HRU_COMPLETE) {
        hru_abort(&upload,HRU_E_TRANSPORT);upload_publish();
    }
}
static int epcmd(unsigned ep,unsigned cmd,uint32_t p0,uint32_t p1,uint32_t p2)
{
    if (u.failed || ep>=6) return -1;
    wr(EPBASE(ep)+8,p0); wr(EPBASE(ep)+4,p1); wr(EPBASE(ep),p2);
    wr(EPBASE(ep)+12,cmd|ACTIVE);
    for (unsigned n=0;n<1000;n++) {
        uint32_t v=rd(EPBASE(ep)+12);
        if (!(v&ACTIVE)) {
            if ((v>>12)&15) { fail("endpoint command status"); return -1; }
            if ((cmd&15)==START) u.resource[ep]=(v>>16)&0x7f;
            return 0;
        }
        delay(1);
    }
    fail("endpoint command timeout"); return -1;
}
static void transfer(unsigned ep,unsigned buf,unsigned length,unsigned kind)
{
    if (u.failed) return;
    if (ep>=6 || u.busy[ep] || buf>DMA_SIZE || length>DMA_SIZE-buf) {
        fail("transfer ownership/bounds"); return;
    }
    unsigned off=trb_off[ep];
    uint64_t p=u.pa+buf,t=u.pa+off;
    volatile uint32_t *trb=(volatile uint32_t *)(u.dma+off);
    trb[0]=(uint32_t)p; trb[1]=(uint32_t)(p>>32);
    trb[2]=length; trb[3]=kind|TRB_FLAGS;
    u.requested[ep]=length;
    if (length) sync_dma(buf,length);
    sync_dma(off,16);
    u.busy[ep]=true;
    (void)epcmd(ep,START,(uint32_t)(t>>32),(uint32_t)t,0);
}
static int complete(unsigned ep)
{
    if (ep>=6 || !u.busy[ep]) { fail("unexpected completion"); return -1; }
    sync_dma(trb_off[ep],16);
    volatile uint32_t *t=(volatile uint32_t *)(u.dma+trb_off[ep]);
    unsigned left=t[2]&0xffffff;
    if ((t[3]&HWO) || (t[2]>>28) || left>u.requested[ep]) {
        fail("invalid DMA completion"); return -1;
    }
    u.busy[ep]=false;
    return (int)(u.requested[ep]-left);
}
static void setup_arm(void)
{
    u.phase=SETUP_PHASE;
    transfer(0,SETUP,8,T_SETUP);
}
static void stall(void)
{
    (void)epcmd(0,STALL,0,0,0);
    setup_arm();
}
static void receive_arm(void) { transfer(2,RX,RX_SIZE,T_NORMAL); }
static void stop_data(void)
{
    for (unsigned ep=2;ep<6;ep++) {
        if (u.busy[ep]) {
            if (!u.resource[ep]) { fail("active endpoint without transfer resource"); return; }
            if (epcmd(ep,END|FORCE_REMOVE|(u.resource[ep]<<16),0,0,0)) return;
            u.busy[ep]=false;
        }
    }
    wr(DALEPENA,3);
    u.configured=false; u.dtr=false; u.notify=false;
}
static void configure(void)
{
    if (u.configured || u.failed) return;
    if (epcmd(0,START_CONFIG|(2u<<16),0,0,0) ||
        epcmd(2,SET_CONFIG,(2u<<1)|(u.mps<<3),(1u<<8)|(2u<<25),0) ||
        epcmd(2,SET_RESOURCE,1,0,0) ||
        epcmd(3,SET_CONFIG,(2u<<1)|(u.mps<<3)|(1u<<17),(1u<<8)|(3u<<25),0) ||
        epcmd(3,SET_RESOURCE,1,0,0) ||
        epcmd(5,SET_CONFIG,(3u<<1)|(16u<<3)|(2u<<17),(1u<<8)|(8u<<16)|(5u<<25),0) ||
        epcmd(5,SET_RESOURCE,1,0,0)) return;
    wr(DALEPENA,3|(1u<<2)|(1u<<3)|(1u<<5));
    u.configured=true; receive_arm();
    unote("HOUJI USB: CDC ACM configured, bulk MPS %u\n",u.mps);
}
static unsigned string_desc(unsigned index,uint8_t *data)
{
    if (!index) { data[0]=4;data[1]=3;data[2]=9;data[3]=4; return 4; }
    if (index>=sizeof(usb_strings)/sizeof(usb_strings[0])) return 0;
    unsigned n=2;
    for (const char *s=usb_strings[index];*s && n<126;s++) { data[n++]=(uint8_t)*s;data[n++]=0; }
    data[0]=(uint8_t)n;data[1]=3;return n;
}
static void handle_setup(void)
{
    sync_dma(SETUP,8);
    const uint8_t *s=(const uint8_t *)(u.dma+SETUP);
    uint8_t *data=(uint8_t *)(u.dma+DATA);
    uint16_t value=(uint16_t)(s[2]|(s[3]<<8)),index=(uint16_t)(s[4]|(s[5]<<8)),length=(uint16_t)(s[6]|(s[7]<<8));
    unsigned n=0;
    int kind=vusb_request(s[0],s[1],value,index,length);
    u.data_in=(s[0]&0x80)!=0;u.set_line=false;
    if (u.setups++<40) printf("HOUJI USB: setup %02x %02x %04x %04x len%u\n",s[0],s[1],value,index,length);
    switch (kind) {
    case VUSB_DESC:
        switch (value>>8) {
        case 1: if (value&255) break;memcpy(data,vusb_device,sizeof(vusb_device));n=sizeof(vusb_device);break;
        case 2: case 7:
            if (value&255) break;
            memcpy(data,vusb_config,sizeof(vusb_config));n=sizeof(vusb_config);
            data[1]=(uint8_t)(value>>8);
            /* Configuration and other-speed configuration have opposite MPS. */
            for (unsigned i=0;i<n;i+=data[i]) if (data[i+1]==5 && (data[i+3]&3)==2) {
                unsigned m=(value>>8)==7?(u.mps==512?64:512):u.mps;
                data[i+4]=(uint8_t)m;data[i+5]=(uint8_t)(m>>8);
            }
            break;
        case 3:n=string_desc(value&255,data);break;
        case 6:if (!(value&255)) { memcpy(data,vusb_qualifier,sizeof(vusb_qualifier));n=sizeof(vusb_qualifier); }break;
        default:break;
        }
        if (!n) { stall();return; }break;
    case VUSB_ADDRESS:wr(DCFG,(rd(DCFG)&~(127u<<3))|((uint32_t)value<<3));break;
    case VUSB_CONFIG:
        /* Reconfiguration while transfers are live is stopped, never recycled
         * before a controller reset drains stale completion events. */
        if (!value && u.configured) { stop_data();if (u.failed) return;unote("HOUJI USB: host deconfigured\n");break; }
        if (value) configure();break;
    case VUSB_GET_CONFIG:data[0]=u.configured?1:0;n=1;break;
    case VUSB_STATUS:data[0]=data[1]=0;n=2;break;
    case VUSB_INTERFACE:break;
    case VUSB_GET_INTERFACE:data[0]=0;n=1;break;
    case VUSB_SET_LINE:u.set_line=true;break;
    case VUSB_GET_LINE:memcpy(data,u.coding,7);n=7;break;
    case VUSB_CONTROL:
        if (u.configured) {
            bool old=u.dtr;u.dtr=(value&1)!=0;u.notify=true;
            if (!u.dtr && upload_mode && upload.phase!=HRU_COMPLETE) {
                hru_abort(&upload,HRU_E_TRANSPORT);upload_publish();
            }
            if (u.dtr && !old) unote("HOUJI USB: debug connected; help/status/ping/replay/reboot\n");
        }break;
    case VUSB_BREAK:break;
    case VUSB_CLEAR_HALT:
        /* Active transfer replay cannot be made safe by CLEARSTALL alone. */
        if (u.busy[(index&15)*2+((index>>7)&1)]) { fail("clear-halt on active endpoint");return; }
        (void)epcmd((index&15)*2+((index>>7)&1),CLEAR_STALL,0,0,0);break;
    default:stall();return;
    }
    if (u.failed) return;
    if (!length) { u.phase=WAIT2;return; }
    u.phase=DATA_PHASE;
    if (u.data_in) transfer(1,DATA,n<length?n:length,T_DATA);
    else if (u.set_line) transfer(0,DATA,64,T_DATA);
    else stall();
}
/* XNU's own reboot: sync and unmount, then halt_all_cpus -> PSCI reset
 * (houji_panic_recovery=1). The host armed the no-wipe recovery BCB. */
static void reboot_thread(void *arg __unused,wait_result_t result __unused)
{
    printf("HOUJI DEBUG: reboot requested over USB; syncing filesystems\n");
    reboot_kernel(RB_AUTOBOOT,(char *)"houji usb debug reboot");
    printf("HOUJI DEBUG: reboot_kernel returned\n");
    thread_terminate(current_thread());
}
/* Synthetic input for unattended UI tests over this console: the same HID entry points the touch and button drivers use.
 * tap X Y | swipe X1 Y1 X2 Y2 MS | home. Panel pixels (0..1199, 0..2669). One gesture at a time, on its own thread. */
extern void sweet_hid_touch(int, int, int, int);
extern void sweet_hid_key(int, int);
static struct { int kind, x1, y1, x2, y2, ms; } gesture;
static int _Atomic gesture_busy;
static void gesture_sleep_ms(unsigned ms)
{
    static int channel;uint64_t deadline;
    clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
    (void)assert_wait_deadline(&channel,THREAD_UNINT,deadline);
    thread_block(THREAD_CONTINUE_NULL);
}
static void gesture_thread(void *arg __unused,wait_result_t result __unused)
{
    if (gesture.kind==3) {
        sweet_hid_key(3,1);gesture_sleep_ms(80);sweet_hid_key(3,0);
    } else {
        int steps=gesture.kind==1?1:(gesture.ms/8>2?gesture.ms/8:2);
        sweet_hid_touch(0,1,gesture.x1,gesture.y1);
        for (int i=1;i<=steps;i++) {
            gesture_sleep_ms(gesture.kind==1?60:8);
            int x=gesture.x1+(gesture.x2-gesture.x1)*i/steps,y=gesture.y1+(gesture.y2-gesture.y1)*i/steps;
            sweet_hid_touch(0,1,x,y);
        }
        sweet_hid_touch(0,0,gesture.x2,gesture.y2);
    }
    os_atomic_store(&gesture_busy,0,release);
    thread_terminate(current_thread());
}
static int gesture_numbers(const char *s,int *out,int max)
{
    int n=0;
    while (*s && n<max) {
        while (*s==' ') s++;
        if (*s<'0' || *s>'9') break;
        int v=0;while (*s>='0' && *s<='9') { v=v*10+(*s-'0');if (v>100000) return -1;s++; }
        out[n++]=v;
    }
    while (*s==' ') s++;
    return *s?-1:n;
}
static void gesture_command(const char *cmd)
{
    int v[5],n;
    if (!strcmp(cmd,"home")) { gesture.kind=3; }
    else if (!strncmp(cmd,"tap ",4) && (n=gesture_numbers(cmd+4,v,5))==2) {
        gesture.kind=1;gesture.x1=gesture.x2=v[0];gesture.y1=gesture.y2=v[1];gesture.ms=0;
    } else if (!strncmp(cmd,"swipe ",6) && (n=gesture_numbers(cmd+6,v,5))==5) {
        gesture.kind=2;gesture.x1=v[0];gesture.y1=v[1];gesture.x2=v[2];gesture.y2=v[3];gesture.ms=v[4]>2000?2000:v[4];
    } else { printf("HOUJI DEBUG: gesture syntax: tap X Y | swipe X1 Y1 X2 Y2 MS | home\n");return; }
    if (gesture.kind!=3 && (gesture.x1>1199 || gesture.x2>1199 || gesture.y1>2669 || gesture.y2>2669)) {
        printf("HOUJI DEBUG: gesture outside 1200x2670\n");return;
    }
    if (!os_atomic_cmpxchg(&gesture_busy,0,1,acquire)) { printf("HOUJI DEBUG: gesture busy\n");return; }
    thread_t t;
    if (kernel_thread_start(gesture_thread,NULL,&t)!=KERN_SUCCESS) { os_atomic_store(&gesture_busy,0,release);return; }
    thread_deallocate(t);
    printf("HOUJI DEBUG: gesture %s\n",cmd);
}
/* "type TEXT": a hardware keyboard types TEXT (a password, too: it is never logged here). */
static char type_text[64];
static volatile uint32_t type_busy;
static void type_thread(void *arg __unused,wait_result_t result __unused)
{
    extern int sweet_hid_type(const char *);
    const int n=sweet_hid_type(type_text);
    bzero(type_text,sizeof(type_text));
    printf("HOUJI DEBUG: typed %d characters\n",n);
    os_atomic_store(&type_busy,0,release);
    thread_terminate(current_thread());
}
static void type_command(const char *text)
{
    if (!os_atomic_cmpxchg(&type_busy,0,1,acquire)) { printf("HOUJI DEBUG: typing busy\n");return; }
    /* "\n" in the text is the Return key */
    size_t o=0;
    for (const char *c=text;*c && o<sizeof(type_text)-1;c++) {
        if (c[0]=='\\' && c[1]=='n') { type_text[o++]='\n'; c++; }
        else type_text[o++]=*c;
    }
    type_text[o]=0;
    thread_t t;
    if (kernel_thread_start(type_thread,NULL,&t)!=KERN_SUCCESS) { bzero(type_text,sizeof(type_text));os_atomic_store(&type_busy,0,release);return; }
    thread_deallocate(t);
}
static void command_line(void)
{
    u.command[u.command_n]=0;
    if (u.command_overflow) printf("HOUJI DEBUG: command too long\n");
    else if (!strcmp(u.command,"upload")) {
        if (upload.phase!=HRU_BEGIN_WAIT || upload.ready_sent) {
            printf("HOUJI UPLOAD: unavailable; no fresh RAM target bound\n");
        } else {
            upload_mode=true;upload_timeout_after(30);
            (void)hru_ready(&upload);upload_publish();
        }
    }
    else if (!strcmp(u.command,"ping")) printf("HOUJI DEBUG: pong HOUJIDBG001\n");
    else if (!strcmp(u.command,"shot")) { extern void houji_framebuffer_shot(unsigned,unsigned); houji_framebuffer_shot(150,334); }
    else if (!strncmp(u.command,"shotr ",6)) {
        extern void houji_framebuffer_shot_region(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
        int r[4];
        if (gesture_numbers(u.command+6,r,4)==4) {
            unsigned cols=r[2]>=r[3]?300:(unsigned)(300ull*r[2]/r[3]),rows=r[2]>=r[3]?(unsigned)(300ull*r[3]/r[2]):300;
            houji_framebuffer_shot_region(r[0],r[1],r[2],r[3],cols?cols:1,rows?rows:1);
        } else printf("HOUJI DEBUG: shotr X Y W H\n");
    }
    else if (!strcmp(u.command,"home") || !strncmp(u.command,"tap ",4) || !strncmp(u.command,"swipe ",6)) gesture_command(u.command);
    else if (!strcmp(u.command,"key power")) { sweet_hid_key(0,1); delay(120000); sweet_hid_key(0,0); }
    else if (!strncmp(u.command,"camera ",7)) { extern void houji_camera_command(const char *); houji_camera_command(u.command+7); }
    else if (!strncmp(u.command,"screen ",7)) { extern void houji_screen_command(const char *); houji_screen_command(u.command+7); }
    else if (!strncmp(u.command,"type ",5)) { type_command(u.command+5); bzero(u.command,sizeof(u.command)); }
    else if (!strncmp(u.command,"wifi ",5)) { extern void houji_wifi_command(const char *); houji_wifi_command(u.command+5); bzero(u.command,sizeof(u.command)); }
    else if (!strcmp(u.command,"reboot")) {
        static bool requested;thread_t t;
        if (requested) printf("HOUJI DEBUG: reboot already in progress\n");
        else if (kernel_thread_start(reboot_thread,NULL,&t)==KERN_SUCCESS) { requested=true;thread_deallocate(t); }
        else printf("HOUJI DEBUG: reboot thread failed\n");
    }
    else if (!strcmp(u.command,"status"))
        printf("HOUJI DEBUG: HOUJIDBG001 candidate002 build %s %s events=%u tx=%llu rx=%llu log=%llu dropped=%llu\n",
            __DATE__,__TIME__,u.events,u.tx_bytes,u.rx_bytes,houji_debug_total(),houji_debug_dropped());
    else if (!strcmp(u.command,"replay")) {
        u.cursor=0;printf("HOUJI DEBUG: replay retained RAM log\n");
    } else if (u.command_n) printf("HOUJI DEBUG: commands help status ping replay reboot tap swipe home shot shotr wifi\n");
    u.command_n=0;u.command_overflow=false;
}
static void receive_done(unsigned length)
{
    sync_dma(RX,RX_SIZE);u.rx_bytes+=length;
    const uint8_t *p=(const uint8_t *)(u.dma+RX);
    for (unsigned i=0;i<length;i++) {
        if (upload_mode) { upload_feed(p+i,length-i);break; }
        if (p[i]=='\r' || p[i]=='\n') command_line();
        else if (p[i]>=32 && p[i]<=126) {
            if (u.command_n<sizeof(u.command)-1) u.command[u.command_n++]=(char)p[i];
            else u.command_overflow=true;
        }
    }
    if (!upload_mode || upload.phase==HRU_BEGIN_WAIT || upload.phase==HRU_RECEIVING) receive_arm();
}
static void transmit(void)
{
    if (!u.configured || !u.dtr || u.failed) return;
    if (u.notify && !u.busy[5]) {
        const uint8_t msg[10]={0xa1,0x20,0,0,0,0,2,0,3,0};
        memcpy((void *)(u.dma+NOTIFY),msg,sizeof(msg));u.notify=false;
        transfer(5,NOTIFY,sizeof(msg),T_NORMAL);
    }
    if (u.busy[3]) return;
    if (upload_mode) {
        if (upload_ack_length) {
            unsigned n=upload_ack_length;
            upload_done_sent=upload_ack[5]==HRU_DONE;
            memcpy((void *)(u.dma+TX),upload_ack,n);upload_ack_length=0;
            transfer(3,TX,n,T_NORMAL);u.tx_bytes+=n;
        }
        return; /* Text never interleaves with binary protocol responses. */
    }
    unsigned n=houji_debug_copy(&u.cursor,(char *)(u.dma+TX),TX_SIZE);
    /* Serial stream must have a short packet, with no padding/invented bytes. */
    if (n && !(n%u.mps)) { n--;u.cursor--; }
    if (n) { transfer(3,TX,n,T_NORMAL);u.tx_bytes+=n; }
}
static void event(uint32_t e)
{
    if (e&1) {
        if ((e&0xfe)!=0) { fail("unexpected device event format");return; }
        unsigned type=(e>>8)&15;
        if (type==1) {
            /* Host re-enumeration: end data transfers, then serve a fresh setup. */
            if (u.configured) { stop_data();if (u.failed) return;unote("HOUJI USB: bus reset while configured; data endpoints ended\n"); }
            wr(DCFG,rd(DCFG)&~(127u<<3));unote("HOUJI USB: bus reset\n");
        } else if (type==2) {
            unsigned speed=rd(DSTS)&7;
            if (speed!=0 && speed!=1 && speed!=3) { fail("unexpected non-USB2 speed");return; }
            u.mps=speed==0?512:64;
            (void)epcmd(0,SET_CONFIG,(64u<<3)|(2u<<30),(1u<<8)|(1u<<10),0);
            (void)epcmd(1,SET_CONFIG,(64u<<3)|(2u<<30),(1u<<8)|(1u<<10)|(1u<<25),0);
            unote("HOUJI USB: connected speed=%u\n",speed);
        } else if (!type) {
            if (u.configured) { stop_data();if (u.failed) return; }
            unote("HOUJI USB: cable disconnect; waiting for host\n");
        }
        else if (type==9 || type==11) fail("controller/event overflow");
        return;
    }
    unsigned ep=(e>>1)&31,type=(e>>6)&15,status=(e>>12)&15;
    if (type==1) {
        int n=complete(ep);if (n<0) return;
        if (status&1) { fail("USB transfer bus error");return; }
        if (ep==2) { receive_done((unsigned)n);return; }
        if (ep==3 && upload_mode && upload.phase==HRU_COMPLETE && upload_done_sent && !upload_ack_length) {
            upload_mode=false;receive_arm();
            printf("HOUJI UPLOAD: RAM readback verified; text console resumed\n");
        }
        if (ep>1) return;
        switch (u.phase) {
        case SETUP_PHASE:if (ep!=0 || n!=8) fail("setup length");else handle_setup();break;
        case DATA_PHASE:
            if (u.set_line) {
                if (ep!=0 || n!=7) { fail("line coding length");break; }
                sync_dma(DATA,64);memcpy(u.coding,(void *)(u.dma+DATA),7);
            }
            u.phase=WAIT3;break;
        case STATUS_PHASE:setup_arm();break;
        default:fail("control completion order");break;
        }
    } else if (ep<=1 && type==3 && status==2 && (u.phase==WAIT2 || u.phase==WAIT3)) {
        unsigned direction=u.phase==WAIT2?1:(u.data_in?0:1);
        if (ep!=direction) { fail("control status direction");return; }
        unsigned kind=u.phase==WAIT3?T_STATUS3:T_STATUS2;
        u.phase=STATUS_PHASE;transfer(ep,DATA,0,kind);
    }
}
static void usb_thread(void *arg __unused,wait_result_t result __unused)
{
    while (!u.failed) {
        unsigned count=rd(GEVNTCOUNT)&0xffff;
        if (count>EVENT_SIZE || (count&3)) { fail("event ring size/alignment");break; }
        if (count) {
            sync_dma(EVENTS,EVENT_SIZE);
            for (unsigned i=0;i<count && !u.failed;i+=4) {
                uint32_t e=*(volatile uint32_t *)(u.dma+u.pos);
                u.pos=(u.pos+4)%EVENT_SIZE;u.events++;
                event(e);
            }
            wr(GEVNTCOUNT,count);
        }
        if (upload_mode && upload.phase!=HRU_COMPLETE && upload.phase!=HRU_FAILED) {
            if (mach_absolute_time()>upload_deadline) {
                hru_abort(&upload,HRU_E_TRANSPORT);
            } else if (upload.phase==HRU_VERIFYING) {
                /* END never re-arms OUT; this DMA window is now CPU-owned. */
                if (u.busy[2]) hru_abort(&upload,HRU_E_STATE);
                else hru_verify_step(&upload,(uint8_t *)(u.dma+RX),RX_SIZE);
            }
            upload_publish();
        }
        transmit();
        uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
        (void)assert_wait_deadline(&u.sleep_channel,THREAD_UNINT,deadline);
        thread_block(THREAD_CONTINUE_NULL);
    }
    wr(DEVTEN,0);wr(DCTL,rd(DCTL)&~RUN);
    thread_terminate(current_thread());
}
/* Require the exact known nonsecure SMMU layout and a unique identity route.
 * Never create mappings or weaken isolation just to make debug USB work. */
/* 0 refuse, 1 routed already, 2 unmatched and faulting: needs our own route. */
static int identity(void)
{
    vm_offset_t s=u.smmu;
    unote("HOUJI USB: SMMU id0=%08x id1=%08x id2=%08x scr0=%08x\n",
        read_reg(s,0x20),read_reg(s,0x24),read_reg(s,0x28),read_reg(s,0));
    if (!huc_layout(read_reg(s,0x20),read_reg(s,0x24),read_reg(s,0x28))) return 0;
    unsigned matches=0;uint32_t route=0;
    for (unsigned i=0;i<107;i++) if (vusb_stream_match(read_reg(s,0x800+4*i))) {
        matches++;route=read_reg(s,0xc00+4*i);
    }
    if (!matches) {
        /* ABL's fastboot DMA used this same unmatched stream: sCR0.USFCFG=0
         * bypasses it to the VM's stage 2. With USFCFG=1 it would fault. */
        uint32_t scr0=read_reg(s,0);
        unote("HOUJI USB: SID40 unmatched; sCR0 USFCFG=%u CLIENTPD=%u\n",(scr0>>10)&1,scr0&1);
        return ((scr0>>10)&1)?2:1;
    }
    if (matches!=1) { unote("HOUJI USB: SID40 matches %u\n",matches);return 0; }
    unsigned cb=route&255;uint32_t cbar=0,sctlr=1;
    if (((route>>16)&3)==0) {
        if (cb>=71) return 0;
        cbar=read_reg(s,0x1000+4*cb);sctlr=read_reg(s,0x80000+0x1000*cb);
    }
    unote("HOUJI USB: SID40 route=%08x cbar=%08x sctlr=%08x\n",route,cbar,sctlr);
    return huc_route(route,cbar,sctlr,((route>>16)&3)==0?read_reg(s,0x1800+4*cb):0)?1:0;
}
void houji_usb_debug_start(void)
{
    static bool attempted;
    { extern void houji_power_start(void); static bool power; if (!power) { power=true; houji_power_start(); } }
    int enabled=0;PE_parse_boot_argn("houji_usb",&enabled,sizeof(enabled));
    if ((enabled!=1 && enabled!=2) || attempted) return;
    attempted=true;u.mps=512;u.coding[0]=0;u.coding[1]=0xc2;u.coding[2]=1;u.coding[6]=8;
    unote("HOUJI USB: CDC ACM candidate002 build %s %s, RAM log and read-only commands\n",__DATE__,__TIME__);
    DTEntry root;
    void const *compatible=NULL,*target=NULL;
    unsigned compatible_n=0,target_n=0;
    if (SecureDTLookupEntry(NULL,"/",&root)!=kSuccess ||
        SecureDTGetProperty(root,"compatible",&compatible,&compatible_n)!=kSuccess ||
        SecureDTGetProperty(root,"target-type",&target,&target_n)!=kSuccess ||
        !huc_board(compatible,compatible_n,target,target_n)) {
        char shown[96];unsigned k=0;
        for (unsigned i=0;compatible && i<compatible_n && k+1<sizeof(shown);i++) {
            char ch=((const char *)compatible)[i];shown[k++]=ch?(ch>=32&&ch<127?ch:'?'):'|';
        }
        shown[k]=0;
        unote("HOUJI USB: houji/SM8650 ADT guard refused; compatible[%u]=%s target[%u]=%.16s; no MMIO mapped\n",
            compatible_n,shown,target_n,target?(const char *)target:"(none)");return;
    }
    int test_target=0;
    PE_parse_boot_argn("houji_upload_test",&test_target,sizeof(test_target));
    if (enabled==1 && test_target==1) {
        extern void houji_ram_upload_bind_test_target(void);
        houji_ram_upload_bind_test_target();
    }
    upload_binding_closed=true;
    vm_offset_t gcc=ml_io_map(0x139000,0x1000);
    if (!gcc) return;
    uint32_t power=read_reg(gcc,4),master=read_reg(gcc,0x18),cfg=read_reg(gcc,0x88),aggre=read_reg(gcc,0x8c);
    unote("HOUJI USB: GCC power=%08x master=%08x cfg=%08x aggre=%08x\n",power,master,cfg,aggre);
    if (!(power&(1u<<31)) || ((master|cfg|aggre)&(1u<<31))) {
        unote("HOUJI USB: clocks/power unavailable; controller untouched\n");return;
    }
    u.regs=ml_io_map(0xa600000,0xe000);u.smmu=ml_io_map(0x15000000,0x100000);
    if (!u.regs || !u.smmu) return;
    uint32_t id=rd(0xc120),hw=rd(0xc140);unsigned width=hw>>24;
    unote("HOUJI USB: id=%08x hwparams0=%08x awidth=%u gctl=%08x dctl=%08x dsts=%08x\n",
        id,hw,width,rd(GCTL),rd(DCTL),rd(DSTS));
    if ((id>>16)!=0x5533 && (id>>16)!=0x3331) return;
    int route=((rd(GCTL)>>12)&3)==2?identity():0;
    if (!route) {
        unote("HOUJI USB: device mode/identity guard refused; controller untouched\n");return;
    }
    if (width<36 || width>64) { unote("HOUJI USB: DMA address width guard refused\n");return; }
    if (enabled==2) {
        unote("HOUJI USB: read-only preflight passed; no DMA allocation or controller writes\n");return;
    }
    if (kmem_alloc_contig(kernel_map,&u.dma,DMA_SIZE,0x3fff,0,0,
        KMA_DATA|KMA_ZERO|KMA_PERMANENT,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS) return;
    ppnum_t pn=pmap_find_phys(kernel_pmap,(addr64_t)u.dma);
    if (!pn) return;
    u.pa=ptoa_64(pn);
    if (!vusb_dma_allowed(u.pa,DMA_SIZE,width)) { unote("HOUJI USB: owned DMA width/range guard refused\n");return; }
    for (unsigned off=0;off<DMA_SIZE;off+=PAGE_SIZE) {
        ppnum_t page=pmap_find_phys(kernel_pmap,(addr64_t)(u.dma+off));
        if (!page || ptoa_64(page)!=u.pa+off) return;
    }
    if (route==2) {
        if (!(rd(DSTS)&HALTED) || (rd(DCTL)&RUN)) { unote("HOUJI USB: controller not halted; SMMU route refused\n");return; }
        uint32_t r[3];
        const char *why=houji_smmu_route_stream(0x40,u.pa,DMA_SIZE,r);
        unote("HOUJI USB: SID40 bypass route %s slot=%u bank=%u cbar=%08x sctlr=%08x\n",
            why?why:"ok",r[0]&0xffff,r[0]>>16,r[1],r[2]);
        if (why) return;
    }
    vm_offset_t qs=ml_io_map(0xa6f8800,0x100);
    if (!qs) return;
    /* First stop any old device DMA; soft reset must finish before replacing
     * descriptors. PHY configuration and global bus settings remain from ABL. */
    wr(DEVTEN,0);wr(DCTL,rd(DCTL)&~RUN);
    unsigned i;
    for (i=0;i<500 && !(rd(DSTS)&HALTED);i++) delay(1000);
    if (!(rd(DSTS)&HALTED)) { fail("controller halt timeout");return; }
    wr(DCTL,RESET);
    for (i=0;i<500 && (rd(DCTL)&RESET);i++) delay(1000);
    if (rd(DCTL)&RESET) { fail("controller reset timeout");return; }
    sync_dma(0,DMA_SIZE);
    wr(GEVNTADRLO,(uint32_t)u.pa);wr(GEVNTADRHI,(uint32_t)(u.pa>>32));
    if (rd(GEVNTADRHI)!=(uint32_t)(u.pa>>32)) { fail("event address readback");return; }
    wr(GEVNTSIZ,(1u<<31)|EVENT_SIZE);wr(GEVNTCOUNT,rd(GEVNTCOUNT)&0xffff);
    wr(DCFG,rd(DCFG)&~(7u|(127u<<3))); /* high speed only */
    if (epcmd(0,START_CONFIG,0,0,0) ||
        epcmd(0,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10),0) || epcmd(0,SET_RESOURCE,1,0,0) ||
        epcmd(1,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10)|(1u<<25),0) || epcmd(1,SET_RESOURCE,1,0,0)) return;
    wr(DALEPENA,3);setup_arm();if (u.failed) return;
    uint32_t hs=read_reg(qs,0x10),ss=read_reg(qs,0x30);
    *(volatile uint32_t *)(qs+0x10)=hs|(1u<<20)|(1u<<28);
    *(volatile uint32_t *)(qs+0x30)=ss|(1u<<24);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(DEVTEN,7|(1u<<9)|(1u<<11));wr(DCTL,rd(DCTL)|RUN);
    for (i=0;i<500 && (rd(DSTS)&HALTED);i++) delay(1000);
    if (rd(DSTS)&HALTED) { fail("controller run timeout");return; }
    thread_t thread;
    if (kernel_thread_start(usb_thread,NULL,&thread)!=KERN_SUCCESS) { fail("poll thread creation");wr(DCTL,rd(DCTL)&~RUN);return; }
    thread_deallocate(thread);
    unote("HOUJI USB: owned RAM base=%lx bytes=%lx; mode=%d\n",gPhysBase,gPhysSize,enabled);
    unote("HOUJI USB: running; DMA=%llx; CDC serial HOUJIDBG001; awaiting enumeration\n",u.pa);
    int hold=0;
    PE_parse_boot_argn("houji_usb_hold",&hold,sizeof(hold));
    if (hold==1) {
        static int hold_channel;
        unote("HOUJI USB: diagnostic hold before root selection; reset to leave\n");
        for (;;) {
            uint64_t deadline;
            clock_interval_to_deadline(1000,NSEC_PER_MSEC,&deadline);
            (void)assert_wait_deadline(&hold_channel,THREAD_UNINT,deadline);
            thread_block(THREAD_CONTINUE_NULL);
        }
    }
}
void houji_usb_report(void)
{
    if (!usb_notes_n) { printf("HOUJI USB REPORT: driver never started\n");return; }
    printf("HOUJI USB REPORT BEGIN\n%sHOUJI USB REPORT END\n",usb_notes);
    if (u.regs) printf("HOUJI USB STATE: events=%u setups=%u configured=%u dtr=%u failed=%u tx=%llu rx=%llu dsts=%08x dctl=%08x gevntcount=%08x\n",
        u.events,u.setups,u.configured,u.dtr,u.failed,u.tx_bytes,u.rx_bytes,rd(DSTS),rd(DCTL),rd(GEVNTCOUNT));
}
#endif
