/* houji CDC ACM debug console (from the iOS 16 port of this phone, bsd/dev/houji_usb_debug.c). Opt-in: houji_usb=1.
 * Reuses ABL's PHY/clocks; never changes GCC, PMIC or storage. An owned
 * USB SMMU route may be installed only after halt and completed reset.
 * DWC3 register/endpoint protocol: Linux drivers/usb/dwc3, reference0073.
 * USB can read the RAM console ring and issue four bounded queries only.
 * All controller memory is permanent: a timeout never frees DMA memory.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc_internal.h>
#include <sys/bsdtask_info.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/recount.h>
#include <mach/thread_act.h>
#include <mach/host_info.h>
#include <mach/vm_statistics.h>
#include <mach/task_info.h>
extern kern_return_t task_info(task_t,task_flavor_t,task_info_t,mach_msg_type_number_t *);
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "houji_usb_protocol.h"
#include "houji_ram_upload.h"
#include <machine/atomic.h>
#include <sys/reboot.h>
#include <kern/cpu_number.h>
extern int reboot_kernel(int, char *);
/* Restart. reboot_kernel() shuts the BSD processes down first and panics without an init process (proc_shutdown -> proc_rele of
 * a null proc, seen 2026-11-15 on a kernel that had no root volume): before launchd exists the platform restart is called directly. */
#include <pexpert/pexpert.h>
static int houji_restart(char *message)
{
    if (initproc) return reboot_kernel(RB_AUTOBOOT,message);
    printf("HOUJI RESTART: %s (no init process: platform restart)\n",message);
    extern void native_houji_reset_hook(void);
    native_houji_reset_hook();
    extern int PEHaltRestart(unsigned int type);   /* pexpert.h declares it for the kernel proper only */
    (void)PEHaltRestart(1 /* kPERestartCPU */);
    return 0;
}

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
extern unsigned houji_debug_copy(uint64_t *, char *, unsigned);
extern uint64_t houji_debug_total(void);
extern uint64_t houji_debug_dropped(void);
extern const char *houji_smmu_route_stream(uint16_t, uint64_t, uint64_t, uint32_t *);
void houji_usb_debug_start(void);
void houji_usb_report(void);
/* Early USB lines leave the rolling log long before a checkpoint; keep them. */
static char usb_notes[8192];
static unsigned usb_notes_n;
static void note_add(const char *line,int n)
{
    if (n<=0) return;
    if ((unsigned)n>=sizeof(usb_notes)-usb_notes_n) return;
    memcpy(usb_notes+usb_notes_n,line,(unsigned)n);usb_notes_n+=(unsigned)n;usb_notes[usb_notes_n]=0;
}
#define unote(...) do { char _l[200]; int _n=snprintf(_l,sizeof(_l),__VA_ARGS__); \
    printf("%s",_l); note_add(_l,_n<(int)sizeof(_l)?_n:(int)sizeof(_l)-1); } while (0)
/* iokit/Kernel/arm/HoujiUsbPhy.inc: the USB 2 PHY bring-up a power-on boot needs, its register report, and the cold-state test. */
extern int houji_usb_phy_start(void);
extern void houji_usb_phy_state(const char *);
extern void houji_usb_phy_cold(void);
/* For houji_usb_require=1 (HoujiKernelHooks.c): did the controller start, and what the driver noted on the way. */
static bool usb_started;
int houji_usb_started(void);
const char *houji_usb_notes_tail(unsigned bytes);
int houji_usb_started(void) { return usb_started; }
const char *houji_usb_notes_tail(unsigned bytes) { return usb_notes+(usb_notes_n>bytes?usb_notes_n-bytes:0); }
void houji_usb_note(const char *line);
void houji_usb_note(const char *line) { unote("%s",line); }

#define GCTL 0xc110
#define GCTL_DSBLCLKGTNG 1u
#define GUSB2PHYCFG0 0xc200
#define GUSB3PIPECTL0 0xc2c0
#define USB2_SUSPHY (1u<<6)
#define USB2_ENBLSLPM (1u<<8)
#define LINK_REQUEST_MASK (0xfu<<5)
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
    char command[160];
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
/* Linux v6.12 dwc3_gadget_dctl_write_safe: do not replay link requests. */
static void dctl_write(uint32_t v) { wr(DCTL,v&~LINK_REQUEST_MASK); }
static void reset_state(const char *phase)
{
    unote("HOUJI USB: %s gctl=%08x dctl=%08x dsts=%08x phy2=%08x pipe3=%08x\n",
        phase,rd(GCTL),rd(DCTL),rd(DSTS),rd(GUSB2PHYCFG0),rd(GUSB3PIPECTL0));
}
static void sync_dma(unsigned off,unsigned size)
{
    flush_dcache64((addr64_t)(u.pa+off),size,TRUE);
    __asm__ volatile("dsb sy" ::: "memory");
}
static int fail_panic; /* houji_usb_fail_panic=1: a stop after the host configured the console panics, so the panic log keeps why */
static void fail(const char *why)
{
    if (!u.failed) unote("HOUJI USB: stopped: %s; permanent DMA retained\n",why);
    if (fail_panic && u.configured && !u.failed)
        panic("HOUJI USB: stopped: %s; dsts=%08x dctl=%08x gevntcount=%08x events=%u",why,rd(DSTS),rd(DCTL),rd(GEVNTCOUNT),u.events);
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
    /* Setup pending (TRB status 2): the host began a new control request before this one ended, e.g. after timing out one sent
     * while the phone slept. As Linux dwc3_ep0_complete_data: drop this control transfer; the caller arms for the new setup. */
    if (ep<=1 && !(t[3]&HWO) && (t[2]>>28)==2) {
        u.busy[ep]=false;
        unote("HOUJI USB: control transfer ended by a new setup (phase %u)\n",(unsigned)u.phase);
        return -2;
    }
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
    if (rd(DALEPENA)!=(3|(1u<<2)|(1u<<3)|(1u<<5))) { fail("endpoint enable readback");return; }
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
/* houji_heartbeat_ms=N (10..1000): one line every N ms with a sequence number and the time since boot, so the log shows whether
 * the system ran up to a reset that leaves no panic (a sudden fault below the kernel) or stopped first (a hang, then a watchdog). */
static void heartbeat_thread(void *arg,wait_result_t result __unused)
{
    const unsigned ms=(unsigned)(uintptr_t)arg;
    for (uint64_t n=0;;n++) {
        uint64_t now=mach_absolute_time(),ns=0;absolutetime_to_nanoseconds(now,&ns);
        printf("HOUJI HB %llu %llu.%03llu\n",n,ns/1000000000ull,(ns/1000000ull)%1000ull);
        static int channel;uint64_t deadline;
        clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
        (void)assert_wait_deadline(&channel,THREAD_UNINT,deadline);
        thread_block(THREAD_CONTINUE_NULL);
    }
}
/* Console "readfile PATH": read a whole file through the vnode layer in 1 MiB pieces, printing progress every 4 MiB, to test whether
 * the storage path can read a given file (for example the DriverKit shared cache, first read when a dext starts). Read only. */
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/bsdtask_info.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/vnode_if.h>
#include <sys/dirent.h>
extern task_t proc_task(proc_t);
static char readfile_path[256];
static int _Atomic readfile_busy;
static uint64_t hex_start,hex_length;   /* hex PATH [START [LENGTH]]: hex digits; default 0 and 2 MiB */
static int readfile_mode;   /* 0 readfile (throughput), 1 cat (text lines, at most 48 KiB), 2 ls (directory entries), 3 hex (at most 2 MiB) */
/* ls PATH: every entry name with its type (d directory, - file, l link, ? other). */
static void list_directory(vnode_t vp,vfs_context_t ctx)
{
    char *buffer=(char *)kalloc_data(16384,Z_WAITOK);
    if (!buffer) { printf("HOUJI LS: no buffer\n"); return; }
    unsigned entries=0;int eof=0;off_t offset=0;
    while (!eof && entries<2000) {
        uio_t uio=uio_create(1,offset,UIO_SYSSPACE,UIO_READ);
        if (!uio) break;
        uio_addiov(uio,CAST_USER_ADDR_T(buffer),16384);
        int count=0;
        const int error=VNOP_READDIR(vp,uio,0,&eof,&count,ctx);
        const size_t got=16384-(size_t)uio_resid(uio);
        offset=uio_offset(uio);
        uio_free(uio);
        if (error) { printf("HOUJI LS: readdir error %d\n",error); break; }
        if (!got) break;
        for (size_t at=0;at+8<=got;) {
            const struct dirent *d=(const struct dirent *)(buffer+at);
            if (!d->d_reclen) break;
            if (d->d_ino) {
                const char type=d->d_type==DT_DIR?'d':d->d_type==DT_REG?'-':d->d_type==DT_LNK?'l':'?';
                printf("HOUJI LS| %c %.*s\n",type,(int)d->d_namlen,d->d_name);
                if (!(++entries&31)) delay(20000);
            }
            at+=d->d_reclen;
        }
    }
    printf("HOUJI LS: %s %u entries\n",readfile_path,entries);
    kfree_data(buffer,16384);
}
/* sshot NAME: a kcdata stackshot of the first process with that name (thread states, wait info, dispatch queues, load info), printed as
   "HOUJI HEX" lines like hex PATH; decode with unhex.py and xnu's tools/lldbmacros/kcdata.py. */
extern kern_return_t stack_snapshot_from_kernel(int pid,void *buf,uint32_t size,uint64_t flags,uint64_t delta_since_timestamp,uint32_t pagetable_mask,unsigned *bytes_traced);
static void stackshot_process(const char *name)
{
    int pid=-1;
    for (int candidate=1;candidate<8000 && pid<0;++candidate) {
        proc_t p=proc_find(candidate);
        if (!p) continue;
        char found[64]={0};
        proc_name(candidate,found,sizeof(found));
        if (!strcmp(found,name)) pid=candidate;
        proc_rele(p);
    }
    if (pid<0) { printf("HOUJI SSHOT: no process %s\n",name); return; }
    const uint32_t size=2u<<20;
    char *buffer=(char *)kalloc_data(size,Z_WAITOK|Z_ZERO);
    if (!buffer) { printf("HOUJI SSHOT: no buffer\n"); return; }
    unsigned bytes=0;
    /* KCDATA_FORMAT | GET_DQ | SAVE_LOADINFO | NO_IO_STATS | THREAD_WAITINFO */
    const uint64_t flags=0x10000ull|0x01ull|0x02ull|0x800000ull|0x1000000ull;
    const kern_return_t kr=stack_snapshot_from_kernel(pid,buffer,size,flags,0,0,&bytes);
    printf("HOUJI SSHOT: %s pid %d kr %d bytes %u\n",name,pid,kr,bytes);
    unsigned lines=0;
    for (uint32_t i=0;kr==KERN_SUCCESS && i<bytes;i+=64) {
        char line[140];unsigned n=0;
        for (uint32_t j=i;j<i+64 && j<bytes;++j) n+=(unsigned)snprintf(line+n,sizeof(line)-n,"%02x",(unsigned char)buffer[j]);
        printf("HOUJI HEX %x %s\n",i,line);
        if (!(++lines&15)) delay(15000);
    }
    printf("HOUJI HEX: sshot %s %u bytes\n",name,bytes);
    kfree_data(buffer,size);
}
extern void sweet_hid_key(int, int);
extern void IOSleep(unsigned int milliseconds);
static void key_thread(void *arg,wait_result_t result __unused)
{
    const int key=*(const int *)arg;
    sweet_hid_key(key,1);
    IOSleep(500);   /* 150 ms woke the display once in eight presses (fps85, fps85c); a person holds for 200-500 ms */
    sweet_hid_key(key,0);
    printf("HOUJI KEY: %s pressed 500 ms\n",key==0?"power":key==1?"volume-up":"volume-down");
    thread_terminate(current_thread());
}
static void readfile_thread(void *arg __unused,wait_result_t result __unused)
{
    vfs_context_t ctx=vfs_context_kernel();
    vnode_t vp=NULLVP;
    if (readfile_mode==4) { stackshot_process(readfile_path); goto out; }
    int error=vnode_open(readfile_path,FREAD,0,0,&vp,ctx);
    if (error) { printf("HOUJI READFILE: open %s error %d\n",readfile_path,error); goto out; }
    if (readfile_mode==2) { list_directory(vp,ctx); vnode_close(vp,FREAD,ctx); goto out; }
    if (readfile_mode==3) {
        /* hex PATH: the file as "HOUJI HEX <offset> <hex>" lines of 64 bytes, for scripts/unhex on the PC. */
        char *data=(char *)kalloc_data(65536,Z_WAITOK);
        uint64_t at=hex_start;unsigned lines=0;
        while (data && at<hex_start+hex_length) {
            int resid=0;
            const uint64_t left=hex_start+hex_length-at;
            if (vn_rdwr(UIO_READ,vp,data,left<65536?(int)left:65536,(off_t)at,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx))) break;
            const size_t got=(left<65536?(size_t)left:65536)-(size_t)resid;
            if (!got) break;
            for (size_t i=0;i<got;i+=64) {
                char line[140];unsigned n=0;
                for (size_t j=i;j<i+64 && j<got;++j) n+=(unsigned)snprintf(line+n,sizeof(line)-n,"%02x",(unsigned char)data[j]);
                printf("HOUJI HEX %llx %s\n",at+i,line);
                if (!(++lines&15)) delay(15000);
            }
            at+=got;
        }
        printf("HOUJI HEX: %s %llu bytes from %llx\n",readfile_path,at-hex_start,hex_start);
        if (data) kfree_data(data,65536);
        vnode_close(vp,FREAD,ctx);
        goto out;
    }
    if (readfile_mode==1) {
        char *text=(char *)kalloc_data(49152,Z_WAITOK);
        int resid=0;
        if (text && !vn_rdwr(UIO_READ,vp,text,49152,0,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx))) {
            const size_t got=49152-(size_t)resid;char line[200];unsigned n=0,lines=0;
            for (size_t i=0;i<got;++i) {
                const char c=text[i];
                if (c>=32 && c<127 && n<sizeof(line)-1) line[n++]=c;
                if (c=='\n' || n==sizeof(line)-1 || i+1==got) {
                    line[n]=0;printf("HOUJI CAT| %s\n",line);n=0;
                    if (!(++lines&31)) delay(20000);
                }
            }
            printf("HOUJI CAT: %s %zu bytes shown\n",readfile_path,got);
        } else printf("HOUJI CAT: read failed\n");
        if (text) kfree_data(text,49152);
        vnode_close(vp,FREAD,ctx);
        goto out;
    }
    const size_t chunk=1u<<20;
    char *buffer=(char *)kalloc_data(chunk,Z_WAITOK);
    if (!buffer) { printf("HOUJI READFILE: no buffer\n"); vnode_close(vp,FREAD,ctx); goto out; }
    uint64_t total=0,start=mach_absolute_time();
    for (;;) {
        int resid=0;
        error=vn_rdwr(UIO_READ,vp,buffer,(int)chunk,(off_t)total,UIO_SYSSPACE,0,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
        const uint64_t got=chunk-(uint64_t)resid;
        if (error || !got) break;
        total+=got;
        if (!(total&((4u<<20)-1))) printf("HOUJI READFILE: %llu MiB\n",total>>20);
    }
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-start,&ns);
    printf("HOUJI READFILE: %s %llu bytes, error %d, %llu ms\n",readfile_path,total,error,ns/1000000ull);
    kfree_data(buffer,chunk);
    vnode_close(vp,FREAD,ctx);
out:
    os_atomic_store(&readfile_busy,0,release);
    thread_terminate(current_thread());
}
static void reboot_thread(void *arg,wait_result_t result __unused)
{
    extern int houji_ufs_arm_recovery_bcb(void);
    if (arg) {
        /* To the bootloader: the platform restart at once. reboot_kernel()'s process shutdown and unmounts once stopped the whole
         * system on iOS 27 (root17, 2026-11-16: console dead, no reset); nothing here needs them, the root is read only and Data
         * is a RAM disk. The reason cell is already set and the console thread still runs (200 ms sends this line), so this calls
         * PSCI SYSTEM_RESET itself rather than the reset hook, whose log drain drives the USB controller and is for stopped CPUs. */
        printf("HOUJI DEBUG: reboot to the bootloader requested over USB; platform restart\n");
        delay(200000);
        extern void houji_psci_reset(void);
        houji_psci_reset();
    }
    printf("HOUJI DEBUG: reboot requested over USB; recovery request %d; syncing filesystems\n",houji_ufs_arm_recovery_bcb());
    houji_restart((char *)"houji usb debug reboot");
    printf("HOUJI DEBUG: reboot_kernel returned\n");
    thread_terminate(current_thread());
}
/* Synthetic input for unattended UI tests over this console: the same HID entry points the touch and button drivers use.
 * tap X Y | swipe X1 Y1 X2 Y2 MS | home. Panel pixels (0..1199, 0..2669). One gesture at a time, on its own thread. */
extern void sweet_hid_touch(int, int, int, int);
extern void houji_touch_report(void);
extern void houji_touch_driver_report(void);
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

/* top [MS]: two bounded snapshots of recount's lifetime CPU accounting.  Task
 * totals include threads already terminated; references retain the starting
 * threads so their individual lifetime counters remain readable at the end. */
#define TOP_MAX_PROCS 2048
#define TOP_MAX_THREADS 16384
struct top_proc_sample {
    task_t task;
    uint64_t started,elapsed,cpu_start,cpu_delta,permille;
    uint32_t fault_start,fault_delta;   /* page faults (TASK_EVENTS_INFO) at the start and in the window: "HOUJI TOPF" */
    unsigned thread_begin,thread_initial,threads;
    int pid;
    char name[64];
};
struct top_thread_sample {
    uint64_t tid,cpu_start,cpu_delta,permille;
    unsigned proc_index;
    char name[MAXTHREADNAMESIZE];
};
struct top_context {
    struct top_proc_sample *procs;
    struct top_thread_sample *threads;
    uint64_t *tids;
    unsigned proc_count,thread_count;
    bool proc_truncated,thread_truncated;
};
static int _Atomic top_busy;

static uint32_t top_task_faults(task_t task)
{
    task_events_info_data_t e;mach_msg_type_number_t n=TASK_EVENTS_INFO_COUNT;
    bzero(&e,sizeof e);
    return task_info(task,TASK_EVENTS_INFO,(task_info_t)&e,&n)==KERN_SUCCESS?(uint32_t)e.faults:0;
}
static uint64_t top_task_cpu(task_t task)
{
    struct recount_usage usage={0};
    recount_task_usage(task,&usage);
    return recount_usage_time_mach(&usage);
}
/* Threads through the accessors proc_pidinfo uses (osfmk/kern/bsd_kern.c): they take the task lock themselves, so this file needs
 * neither struct task nor struct thread. Thread times are in nanoseconds; the layout below is bsd/sys/bsdtask_info.h's. */
extern int get_task_numacts(task_t);
extern int fill_taskthreadlist(task_t,void *,int,bool);
static bool top_thread_ns(task_t task,uint64_t tid,uint64_t *ns,char *name,size_t size)
{
    struct proc_threadinfo_internal info={0};
    if (fill_taskthreadinfo(task,tid,true,&info,NULL,NULL)) return false;
    *ns=info.pth_user_time+info.pth_system_time;
    if (name) { strlcpy(name,info.pth_name,size); }
    return true;
}
static uint64_t top_permille(uint64_t cpu,uint64_t elapsed)
{
    if (!elapsed) return 0;
    return cpu/elapsed*1000+(cpu%elapsed)*1000/elapsed;
}
static void top_clean_name(char *name,size_t size,const char *fallback)
{
    if (!name[0]) strlcpy(name,fallback,size);
    for (size_t i=0;name[i];++i)
        if ((unsigned char)name[i]<32 || (unsigned char)name[i]>126) name[i]='?';
}
/* "qos PROC PREFIX [QOS]": every thread of the processes named PROC whose name starts with PREFIX gets the requested QoS class QOS (default
 * 6, user-interactive), as pthread's own class change sets it (proc_set_thread_policy_ext, TASK_POLICY_ATTRIBUTE / QOS_AND_RELPRIO).
 * For the render server's metal2vulkan threads, which start at the default class (priority 37) under the render thread's 55. */
extern thread_t task_findtid(task_t task,uint64_t tid);
extern void proc_set_thread_policy_ext(thread_t thread,int category,int flavor,int value,int value2);
struct qos_request { char proc[32]; char prefix[32]; int qos; unsigned matched, set; };
static int qos_proc(proc_t p,void *arg)
{
    struct qos_request *r=arg;
    if (strcmp(proc_best_name(p),r->proc)) return PROC_RETURNED;
    task_t task=proc_task(p);
    if (task==TASK_NULL) return PROC_RETURNED;
    static uint64_t tids[256];
    int n=get_task_numacts(task);
    if (n<=0) return PROC_RETURNED;
    if (n>256) n=256;
    const int got=fill_taskthreadlist(task,tids,n,true)/(int)sizeof(uint64_t);
    for (int i=0;i<got;++i) {
        struct proc_threadinfo_internal info={0};
        if (fill_taskthreadinfo(task,tids[i],true,&info,NULL,NULL)) continue;
        if (strncmp(info.pth_name,r->prefix,strlen(r->prefix))) continue;
        r->matched++;
        thread_t th=task_findtid(task,tids[i]);
        if (th==THREAD_NULL) continue;
        proc_set_thread_policy_ext(th,0x2 /* TASK_POLICY_ATTRIBUTE */,0x38 /* TASK_POLICY_QOS_AND_RELPRIO */,r->qos,0);
        thread_deallocate(th);
        r->set++;
        printf("HOUJI QOS: %s[%d] thread %llu '%s' -> qos %d\n",r->proc,proc_pid(p),(unsigned long long)tids[i],info.pth_name,r->qos);
    }
    return PROC_RETURNED;
}
/* "ustack PID TID [MS]": a user-stack profile of one thread (default 1500 ms, a sample every 2 ms). Each sample takes the thread's
 * saved user registers (its state at its last kernel entry: exact while it waits in a trap, as of the last interrupt while it runs)
 * and walks up to 7 frame-pointer frames from its process's map. The 24 most frequent stacks are printed with their counts and
 * whether the thread was running, addresses in the dyld shared cache unslid (cache start taken as 0x180000000), so they can be
 * named from the cache on the host. For the frames SpringBoard's main thread spends while a swipe draws nothing. */
extern struct arm_saved_state *get_user_regs(thread_t);
struct vm_shared_region;
extern struct vm_shared_region *vm_shared_region_get(struct task *);
extern void vm_shared_region_deallocate(struct vm_shared_region *);
extern kern_return_t vm_shared_region_start_address(struct vm_shared_region *,mach_vm_offset_t *);
extern vm_map_t get_task_map(task_t);
extern kern_return_t copyinmap(vm_map_t,vm_map_offset_t,void *,vm_size_t);
#define USTACK_DEPTH 20   /* 8 frames did not reach the caller of a UIKit loop (root214) */
#define USTACK_SLOTS 512
struct ustack_slot { uint64_t pc[USTACK_DEPTH]; uint32_t count, running; };
/* The slots are allocated per run: a static table this size does not fit the kernel's __DATA beside the retained segments. */
static struct { int pid; uint64_t tid; unsigned ms; char name[32]; struct ustack_slot *slots; } ustack;
static int _Atomic ustack_busy;
static uint64_t ustack_strip(uint64_t a) { return a & 0xffffffffffull; }
static void ustack_thread(void *arg __unused,wait_result_t result __unused)
{
    proc_t p=proc_find(ustack.pid);
    task_t task=p?proc_task(p):TASK_NULL;
    if (task!=TASK_NULL) task_reference(task);
    if (p) proc_rele(p);
    if (task!=TASK_NULL && !ustack.tid) {   /* TID 0: the main thread, the process's lowest thread id */
        uint64_t *tids=kalloc_data(sizeof(uint64_t)*512,Z_WAITOK|Z_ZERO);
        if (tids) {
            int n=get_task_numacts(task);if (n>512) n=512;
            const int got=n>0?fill_taskthreadlist(task,tids,n,true)/(int)sizeof(uint64_t):0;
            for (int i=0;i<got;i++) {
                if (!tids[i]) continue;
                if (ustack.name[0]) {   /* a thread name prefix: its first thread of that name */
                    struct proc_threadinfo_internal info={0};
                    if (fill_taskthreadinfo(task,tids[i],true,&info,NULL,NULL) || strncmp(info.pth_name,ustack.name,strlen(ustack.name))) continue;
                    if (!ustack.tid || tids[i]<ustack.tid) ustack.tid=tids[i];
                } else if (!ustack.tid || tids[i]<ustack.tid) ustack.tid=tids[i];
            }
            kfree_data(tids,sizeof(uint64_t)*512);
        }
    }
    thread_t th=task!=TASK_NULL&&ustack.tid?task_findtid(task,ustack.tid):THREAD_NULL;
    if (th==THREAD_NULL) {
        printf("HOUJI USTACK: no thread %llu in pid %d\n",(unsigned long long)ustack.tid,ustack.pid);
        if (task!=TASK_NULL) task_deallocate(task);
        os_atomic_store(&ustack_busy,0,release);thread_terminate(current_thread());return;
    }
    mach_vm_offset_t start=0;
    struct vm_shared_region *sr=vm_shared_region_get(task);
    if (sr) { (void)vm_shared_region_start_address(sr,&start);vm_shared_region_deallocate(sr); }
    const uint64_t slide=start>=0x180000000ull?start-0x180000000ull:0;
    vm_map_t map=get_task_map(task);
    ustack.slots=kalloc_data(sizeof(struct ustack_slot)*USTACK_SLOTS,Z_WAITOK|Z_ZERO);
    if (!ustack.slots) { thread_deallocate(th);task_deallocate(task);os_atomic_store(&ustack_busy,0,release);thread_terminate(current_thread());return; }
    unsigned samples=0,dropped=0,running=0;
    for (unsigned elapsed=0;elapsed<ustack.ms;elapsed+=2) {
        gesture_sleep_ms(2);
        struct proc_threadinfo_internal info={0};
        if (fill_taskthreadinfo(task,ustack.tid,true,&info,NULL,NULL)) break;
        const int run=info.pth_run_state==1;
        struct arm_saved_state *ss=get_user_regs(th);
        if (!ss) break;
        uint64_t pc[USTACK_DEPTH]={0};
        pc[0]=ustack_strip(get_saved_state_pc(ss));
        pc[1]=ustack_strip(get_saved_state_lr(ss));
        uint64_t fp=get_saved_state_fp(ss);
        for (int d=2;d<USTACK_DEPTH && fp && !(fp&7);d++) {
            uint64_t frame[2];
            if (copyinmap(map,(vm_map_offset_t)fp,frame,sizeof frame)!=KERN_SUCCESS) break;
            pc[d]=ustack_strip(frame[1]);
            if (frame[0]<=fp) break;
            fp=frame[0];
        }
        for (int d=0;d<USTACK_DEPTH;d++) if (pc[d]>=start && start && pc[d]-slide<0x300000000ull) pc[d]-=slide;
        samples++;running+=run;
        unsigned h=0;for (int d=0;d<USTACK_DEPTH;d++) h=h*31u+(unsigned)(pc[d]^(pc[d]>>32));
        unsigned i=h%USTACK_SLOTS,k=0;
        for (;k<USTACK_SLOTS;k++,i=(i+1)%USTACK_SLOTS) {
            struct ustack_slot *s=&ustack.slots[i];
            if (!s->count) { memcpy(s->pc,pc,sizeof pc); }
            if (!memcmp(s->pc,pc,sizeof pc)) { s->count++;s->running+=run;break; }
        }
        if (k==USTACK_SLOTS) dropped++;
    }
    thread_deallocate(th);task_deallocate(task);
    printf("HOUJI USTACK: pid %d tid %llu samples %u running %u dropped %u cache-start 0x%llx slide 0x%llx\n",ustack.pid,
           (unsigned long long)ustack.tid,samples,running,dropped,(unsigned long long)start,(unsigned long long)slide);
    for (int r=0;r<24;r++) {
        struct ustack_slot *best=NULL;
        for (int i=0;i<USTACK_SLOTS;i++) if (ustack.slots[i].count && (!best || ustack.slots[i].count>best->count)) best=&ustack.slots[i];
        if (!best) break;
        char line[24*USTACK_DEPTH];unsigned at=0;line[0]=0;
        for (int d=0;d<USTACK_DEPTH && at+20<sizeof line;d++) at+=(unsigned)snprintf(line+at,sizeof line-at," %llx",best->pc[d]);
        printf("HOUJI USTACK %u run %u:%s\n",best->count,best->running,line);
        delay(2000);
        best->count=0;
    }
    kfree_data(ustack.slots,sizeof(struct ustack_slot)*USTACK_SLOTS);ustack.slots=NULL;
    os_atomic_store(&ustack_busy,0,release);
    thread_terminate(current_thread());
}
/* "threads PID": every thread of PID with its id, CPU time (user + system, ms), run state, priority and name. Two calls apart give
 * each thread's CPU over the interval: which thread of a busy daemon to sample with "ustack PID TID". */
static void threads_command(const char *args)
{
    const int pid=(int)strtoq(args,NULL,10);
    proc_t p=pid>0?proc_find(pid):PROC_NULL;
    task_t task=p?proc_task(p):TASK_NULL;
    if (task!=TASK_NULL) task_reference(task);
    if (p) proc_rele(p);
    if (task==TASK_NULL) { printf("HOUJI THREADS: no pid %d\n",pid);return; }
    uint64_t *tids=kalloc_data(sizeof(uint64_t)*512,Z_WAITOK|Z_ZERO);
    int n=get_task_numacts(task);if (n>512) n=512;
    const int got=tids && n>0?fill_taskthreadlist(task,tids,n,true)/(int)sizeof(uint64_t):0;
    for (int i=0;i<got;i++) {
        struct proc_threadinfo_internal info={0};
        if (!tids[i] || fill_taskthreadinfo(task,tids[i],true,&info,NULL,NULL)) continue;
        printf("HOUJI THREADS %d %llu cpu %llu ms state %d pri %d sys %llu %.32s\n",pid,(unsigned long long)tids[i],
            (unsigned long long)((info.pth_user_time+info.pth_system_time)/1000000ull),info.pth_run_state,info.pth_curpri,
            (unsigned long long)(info.pth_system_time/1000000ull),info.pth_name);
        delay(2000);
    }
    printf("HOUJI THREADS: pid %d, %d threads\n",pid,got);
    if (tids) kfree_data(tids,sizeof(uint64_t)*512);
    task_deallocate(task);
}
static void ustack_command(const char *args)
{
    long long v[3]={0,0,1500};int n=0;const char *s=args;
    char name[32]={0};
    while (*s && n<3) {
        while (*s==' ') s++; if (!*s) break;
        char *e=NULL;v[n]=strtoq(s,&e,10);
        if (e==s && n==1) {   /* TID given as a thread name prefix */
            size_t i=0;while (*s && *s!=' ' && i<sizeof(name)-1) name[i++]=*s++;
            v[1]=0;n++;continue;
        }
        if (e==s) break;n++;s=e;
    }
    if (n<2 || v[0]<=0 || v[1]<0 || v[2]<2 || v[2]>10000) { printf("HOUJI USTACK: syntax ustack PID TID [MS 2..10000] (TID 0: the main thread; or a thread name prefix)\n");return; }
    if (!os_atomic_cmpxchg(&ustack_busy,0,1,acquire)) { printf("HOUJI USTACK: busy\n");return; }
    ustack.pid=(int)v[0];ustack.tid=(uint64_t)v[1];ustack.ms=(unsigned)v[2];strlcpy(ustack.name,name,sizeof ustack.name);
    thread_t t;
    if (kernel_thread_start(ustack_thread,NULL,&t)!=KERN_SUCCESS) { os_atomic_store(&ustack_busy,0,release);return; }
    thread_deallocate(t);
    printf("HOUJI USTACK: sampling pid %d tid %llu for %u ms\n",ustack.pid,(unsigned long long)ustack.tid,ustack.ms);
}
/* "vm": the host's VM statistics (host_statistics64 HOST_VM_INFO64, as vm_stat reads them), in pages: the page-ins and
 * decompressions a stall in a page fault shows as rising counts between two calls. */
extern kern_return_t host_statistics64(host_t,host_flavor_t,host_info64_t,mach_msg_type_number_t *);
extern host_t host_self(void);
extern volatile uint64_t native_ram_block_resident_bytes,native_ram_block_bytes,native_ram_block_writes;
extern volatile uint64_t native_ram_block_unmapped_pages,native_ram_block_unmaps;
/* "io": the processes that wrote the most to disk (rusage ri_diskio_byteswritten, MiB, lifetime) and those with the largest
 * physical footprint (MiB): on this phone Data is a RAM disk, so a file written is memory taken. */
#include <sys/resource.h>
extern void gather_rusage_info(proc_t,rusage_info_current *,int);
struct io_entry { int pid; uint32_t written, footprint, pageins; char name[24]; };
/* Allocated per call: tables this size do not fit the kernel's __DATA beside the retained segments. */
struct io_table { struct io_entry e[512]; struct io_entry copy[512]; unsigned n; rusage_info_current ru; };
static struct io_table *io_t;
static int io_proc(proc_t p,void *arg __unused)
{
    if (io_t->n>=512 || !p->p_stats) return PROC_RETURNED;
    gather_rusage_info(p,&io_t->ru,RUSAGE_INFO_V4);
    struct io_entry *e=&io_t->e[io_t->n++];
    e->pid=proc_pid(p);e->written=(uint32_t)(io_t->ru.ri_diskio_byteswritten>>20);e->footprint=(uint32_t)(io_t->ru.ri_phys_footprint>>20);
    e->pageins=(uint32_t)io_t->ru.ri_pageins;
    strlcpy(e->name,proc_best_name(p),sizeof e->name);
    return PROC_RETURNED;
}
/* by: 0 bytes written, 1 footprint, 2 page-ins (ri_pageins: faults that read a page from its file, lifetime) */
static uint32_t io_key(const struct io_entry *e,int by) { return by==1?e->footprint:by==2?e->pageins:e->written; }
static void io_print(int by)
{
    static const char *const what[3]={"disk","mem","pagein"};
    for (int r=0;r<14;r++) {
        int best=-1;
        for (unsigned i=0;i<io_t->n;i++) {
            const uint32_t v=io_key(&io_t->e[i],by);
            if (v && (best<0 || v>io_key(&io_t->e[best],by))) best=(int)i;
        }
        if (best<0) break;
        struct io_entry *e=&io_t->e[best];
        printf("HOUJI IO: %s %d %s written %u MiB footprint %u MiB pageins %u\n",what[by],e->pid,e->name,e->written,e->footprint,e->pageins);
        if (by==1) e->footprint=0; else if (by==2) e->pageins=0; else e->written=0;
    }
}
static void io_command(void)
{
    static int _Atomic busy;
    if (!os_atomic_cmpxchg(&busy,0,1,acquire)) return;
    io_t=kalloc_data(sizeof *io_t,Z_WAITOK|Z_ZERO);
    if (!io_t) { os_atomic_store(&busy,0,release);return; }
    proc_iterate(PROC_ALLPROCLIST,io_proc,NULL,NULL,NULL);
    memcpy(io_t->copy,io_t->e,sizeof io_t->copy);
    io_print(0);
    memcpy(io_t->e,io_t->copy,sizeof io_t->copy);
    io_print(1);
    memcpy(io_t->e,io_t->copy,sizeof io_t->copy);
    io_print(2);
    printf("HOUJI IO: %u processes\n",io_t->n);
    kfree_data(io_t,sizeof *io_t);io_t=NULL;
    os_atomic_store(&busy,0,release);
}
extern volatile uint32_t native_adreno_client_bytes[5][2],native_adreno_client_stats[5][6];
extern volatile uint32_t native_adreno_import_memory[4];
extern int nextpidversion;
/* iOS memory kills (memorystatus) so far, summed over priority bands, by cause (sys/kern_memorystatus.h JETSAM_REASON_*). */
extern uint32_t _Atomic memorystatus_kill_counts[210 + 1][17 - 1];
extern uint32_t _Atomic memorystatus_idle_exit_kill_count;
static void kills_print(void)
{
    static const char *const names[17]={"","generic","highwater","vnode","page-shortage","proc-thrash","fc-thrash","per-process",
        "disk-space","idle-exit","zone-map","compressor-thrash","compressor-space","lowswap","sustained","pageout-starved","conclave"};
    char line[400];int n=snprintf(line,sizeof line,"HOUJI VM: kills idle-exit(idle band) %u",memorystatus_idle_exit_kill_count);
    for (int cause=1;cause<=17;cause++) {
        if (cause==9) continue;
        const int column=cause<9?cause-1:cause-2;
        if (column>=16) break;
        uint32_t sum=0;
        for (int pri=0;pri<=210;pri++) sum+=memorystatus_kill_counts[pri][column];
        if (sum && n<(int)sizeof line-40) n+=snprintf(line+n,sizeof line-n," %s %u",cause<17?names[cause]:"longidle",sum);
    }
    printf("%s\n",line);
}
/* The GPU driver's per-process tables (NativeAdrenoMemory.cpp): pid, KiB, objects; imports (process memory the GPU maps) and
 * the driver's own buffer objects. */
extern volatile uint32_t native_adreno_import_owners[24][4],native_adreno_bo_owners[24][4];
static void owners_print(const char *what,volatile uint32_t (*table)[4])
{
    for (int i=0;i<24;i++) {
        if (!table[i][2] || table[i][1]<4096) continue;
        char name[32]="?";
        proc_t p=proc_find((int)table[i][0]);
        if (p) { strlcpy(name,proc_best_name(p),sizeof name);proc_rele(p); } else strlcpy(name,"(exited)",sizeof name);
        printf("HOUJI VM: %s owner %u %s %u MiB in %u\n",what,table[i][0],name,table[i][1]>>10,table[i][2]);
    }
}
extern volatile uint64_t houji_adreno_submit_errors[5];
extern volatile uint64_t houji_adreno_missing[6];
extern volatile uint32_t houji_adreno_free_count;
extern volatile uint64_t houji_adreno_cpumap_refusals[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_view_diag[6] __attribute__((weak_import)),native_adreno_view_kept __attribute__((weak_import));
/* "gm": the GPU driver's per-method counters (NativeAdrenoUserClient's Tally: calls, failures, last result, total microseconds) and
 * the BO view counters (clientMemoryForType). Two calls around an action give its allocations, CPU maps and frees and their cost. */
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak_import));
static void gm_command(void)
{
    static const char *const names[20]={"?","query","alloc","map","free","submit","wait","time","status","setmeta","getmeta","mkqueue",
        "closeq","cpumap","cpuunmap","devinfo","import","prio","note","?"};
    if (&native_adreno_method_stats)
        for (int m=1;m<20;m++) if (native_adreno_method_stats[m][0])
            printf("HOUJI GM %s calls %u fails %u last 0x%x us %u\n",names[m],native_adreno_method_stats[m][0],native_adreno_method_stats[m][1],
                   native_adreno_method_stats[m][2],native_adreno_method_stats[m][3]);
    if (&native_adreno_view_diag)
        printf("HOUJI GM views calls %u new %u cached %u refused %u\n",native_adreno_view_diag[0],native_adreno_view_diag[1],
               native_adreno_view_diag[2],native_adreno_view_diag[3]);
}
static void vm_command(void)
{
    if (houji_adreno_missing[3]) {
        uint64_t ago=0,freed=0;
        absolutetime_to_nanoseconds(mach_absolute_time()-houji_adreno_missing[3],&ago);
        absolutetime_to_nanoseconds(houji_adreno_missing[4],&freed);
        printf("HOUJI VM: gpu missing handle %llu pid %llu %s %llu ms ago; freed %llu us before (%llu KiB; 0 = not in the last 256 of %u frees)\n",
               houji_adreno_missing[0],houji_adreno_missing[1],houji_adreno_missing[2]?"entry unmapped":"no entry",ago/1000000,freed/1000,
               houji_adreno_missing[5],houji_adreno_free_count);
    }
    {
        uint64_t ago_ms=0;
        if (houji_adreno_submit_errors[4]) { absolutetime_to_nanoseconds(mach_absolute_time()-houji_adreno_submit_errors[4],&ago_ms);ago_ms/=1000000; }
        printf("HOUJI VM: gpu submit errors %llu last 0x%llx pid %llu at %llu (1 fence wait, 2 buffer, 3 ring) %llu ms ago\n",
               houji_adreno_submit_errors[0],houji_adreno_submit_errors[1],houji_adreno_submit_errors[2],houji_adreno_submit_errors[3],ago_ms);
    }
    if (&houji_adreno_cpumap_refusals) {
        /* Turnip's VK_ERROR_MEMORY_MAP_FAILED has two sources: BoCpuMap refusals and refused views (clientMemoryForType). */
        uint64_t ago_ms=0;
        if (houji_adreno_cpumap_refusals[7]) { absolutetime_to_nanoseconds(mach_absolute_time()-houji_adreno_cpumap_refusals[7],&ago_ms);ago_ms/=1000000; }
        printf("HOUJI VM: gpu cpumap refusals not-open %llu no-handle %llu unreleasable %llu imported %llu leases %llu; last handle %llu pid %llu %llu ms ago\n",
               houji_adreno_cpumap_refusals[0],houji_adreno_cpumap_refusals[1],houji_adreno_cpumap_refusals[2],houji_adreno_cpumap_refusals[3],
               houji_adreno_cpumap_refusals[4],houji_adreno_cpumap_refusals[5],houji_adreno_cpumap_refusals[6],ago_ms);
    }
    if (&native_adreno_view_diag)
        printf("HOUJI VM: gpu views calls %u new %u cached %u refused %u last 0x%x inside %u kept %u\n",native_adreno_view_diag[0],
               native_adreno_view_diag[1],native_adreno_view_diag[2],native_adreno_view_diag[3],native_adreno_view_diag[4],native_adreno_view_diag[5],
               &native_adreno_view_kept?native_adreno_view_kept:0);
    kills_print();
    owners_print("import",native_adreno_import_owners);
    owners_print("gpu-bo",native_adreno_bo_owners);
    /* Processes forked so far (each fork takes a pid version) and alive now; GPU imports of process memory: live, KiB live and
     * peak, made so far. Two calls apart give spawns and imports per second. */
    printf("HOUJI VM: forks %d procs %d | gpu imports live %u %u KiB peak %u KiB made %u\n",nextpidversion,nprocs,
           native_adreno_import_memory[0],native_adreno_import_memory[1],native_adreno_import_memory[2],native_adreno_import_memory[3]);
    vm_statistics64_data_t s;
    mach_msg_type_number_t n=HOST_VM_INFO64_COUNT;
    bzero(&s,sizeof s);
    const kern_return_t kr=host_statistics64(host_self(),HOST_VM_INFO64,(host_info64_t)&s,&n);
    printf("HOUJI VM: kr %d free %u active %u inactive %u spec %u wire %u ext %u int %u compressor %u (holds %llu) | faults %llu pageins %llu "
           "pageouts %llu decompressions %llu compressions %llu reactivations %llu swapins %llu\n",kr,s.free_count,s.active_count,
           s.inactive_count,s.speculative_count,s.wire_count,s.external_page_count,s.internal_page_count,s.compressor_page_count,
           s.total_uncompressed_pages_in_compressor,s.faults,s.pageins,s.pageouts,s.decompressions,s.compressions,s.reactivations,s.swapins);
    printf("HOUJI VM: Data RAM disk resident %llu MiB of %llu MiB, writes %llu, unmaps %llu freeing %llu pages\n",
           native_ram_block_resident_bytes>>20,native_ram_block_bytes>>20,native_ram_block_writes,native_ram_block_unmaps,
           native_ram_block_unmapped_pages);
    printf("HOUJI VM: GPU BOs MiB live/peak per client slot %u/%u %u/%u %u/%u %u/%u kernel %u/%u; refused %u %u %u %u\n",
           native_adreno_client_bytes[0][0],native_adreno_client_bytes[0][1],native_adreno_client_bytes[1][0],native_adreno_client_bytes[1][1],
           native_adreno_client_bytes[2][0],native_adreno_client_bytes[2][1],native_adreno_client_bytes[3][0],native_adreno_client_bytes[3][1],
           native_adreno_client_bytes[4][0],native_adreno_client_bytes[4][1],native_adreno_client_stats[0][2],native_adreno_client_stats[1][2],
           native_adreno_client_stats[2][2],native_adreno_client_stats[3][2]);
}
/* "tl MS SPEC...": a timeline of up to six threads, for lining their work up with individual frames (console "swaps"). SPEC is PID:0 (the
 * process's main thread) or PID:NAME (its first thread whose name starts with NAME). Every millisecond each thread's run state and its
 * top TL_DEPTH user frames (unslid like ustack's) are taken; consecutive identical samples of a thread make one run, kept in a buffer and
 * printed at the end: "HOUJI TL R2 thread start-us end-us state cpu pc..." (state 1 on a CPU, 2 runnable, 0 waiting), times from "HOUJI TL BASE" (mach time in ns, the clock of
 * "GESTURE AT" and the swaps). ustack adds a thread's samples up over seconds; this keeps when each one happened. */
#define TL_THREADS 6
#define TL_DEPTH 10
#define TL_RUNS 12000
struct tl_run { uint64_t t0,t1,pc[TL_DEPTH]; uint8_t thr,run,cpu; };
int houji_thread_cpu_state(thread_t thread,int *cpu);
int houji_sample_running(thread_t thread,int cpu,uint64_t out[3]);
struct tl_thread { int pid; char name[24]; uint64_t tid; task_t task; thread_t th; vm_map_t map; uint64_t start,slide; int last; };
static struct { unsigned ms,n; struct tl_thread t[TL_THREADS]; } tl;
static int _Atomic tl_busy;
static uint64_t tl_find_tid(task_t task,const char *name)
{
    uint64_t found=0;
    uint64_t *tids=kalloc_data(sizeof(uint64_t)*512,Z_WAITOK|Z_ZERO);
    if (!tids) return 0;
    int n=get_task_numacts(task);if (n>512) n=512;
    const int got=n>0?fill_taskthreadlist(task,tids,n,true)/(int)sizeof(uint64_t):0;
    for (int i=0;i<got;i++) {
        if (!tids[i]) continue;
        if (name[0] && strcmp(name,"0")) {
            struct proc_threadinfo_internal info={0};
            if (fill_taskthreadinfo(task,tids[i],true,&info,NULL,NULL) || strncmp(info.pth_name,name,strlen(name))) continue;
        }
        if (!found || tids[i]<found) found=tids[i];
    }
    kfree_data(tids,sizeof(uint64_t)*512);
    return found;
}
static void tl_thread_main(void *arg __unused,wait_result_t result __unused)
{
    struct tl_run *runs=kalloc_data(sizeof(struct tl_run)*TL_RUNS,Z_WAITOK|Z_ZERO);
    unsigned nruns=0,full=0,samples=0,samples_fresh=0;
    for (unsigned i=0;i<tl.n;i++) {
        struct tl_thread *t=&tl.t[i];
        t->task=TASK_NULL;t->th=THREAD_NULL;t->last=-1;
        proc_t p=proc_find(t->pid);
        if (p) { t->task=proc_task(p);if (t->task!=TASK_NULL) task_reference(t->task);proc_rele(p); }
        if (t->task==TASK_NULL) continue;
        t->tid=tl_find_tid(t->task,t->name);
        t->th=t->tid?task_findtid(t->task,t->tid):THREAD_NULL;
        struct vm_shared_region *sr=vm_shared_region_get(t->task);
        t->start=0;if (sr) { (void)vm_shared_region_start_address(sr,&t->start);vm_shared_region_deallocate(sr); }
        t->slide=t->start>=0x180000000ull?t->start-0x180000000ull:0;
        t->map=get_task_map(t->task);
        printf("HOUJI TL THREAD %u pid %d tid %llu %s\n",i,t->pid,(unsigned long long)t->tid,t->name);
    }
    uint64_t base=mach_absolute_time(),base_ns=0;absolutetime_to_nanoseconds(base,&base_ns);
    printf("HOUJI TL BASE %llu\n",(unsigned long long)base_ns);
    for (unsigned elapsed=0;runs && elapsed<tl.ms;elapsed++) {
        gesture_sleep_ms(1);
        uint64_t now_ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&now_ns);
        const uint64_t us=(now_ns-base_ns)/1000;
        for (unsigned i=0;i<tl.n;i++) {
            struct tl_thread *t=&tl.t[i];
            if (t->th==THREAD_NULL) continue;
            struct proc_threadinfo_internal info={0};
            if (fill_taskthreadinfo(t->task,t->tid,true,&info,NULL,NULL)) { t->th=THREAD_NULL;continue; }
            /* 1 on a CPU, 2 runnable and waiting for one (pth_run_state alone counts both as running), 0 waiting. */
            int cpu=-1;
            const int where=houji_thread_cpu_state(t->th,&cpu);
            const uint8_t run=info.pth_run_state==1?(where==2?2:1):0,on=(uint8_t)(cpu<0?255:cpu);
            struct arm_saved_state *ss=get_user_regs(t->th);
            uint64_t pc[TL_DEPTH]={0};
            /* A thread on a CPU: its registers now, by a cross-call to that CPU (houji_sample_running); its saved state is only
             * as new as its last kernel entry. Else, or when that fails, the saved state. */
            uint64_t now_regs[3]={0};
            const int fresh=where==1 && houji_sample_running(t->th,cpu,now_regs)==0;
            if (fresh) samples_fresh++;
            if (ss || fresh) {
                pc[0]=ustack_strip(fresh?now_regs[0]:get_saved_state_pc(ss));pc[1]=ustack_strip(fresh?now_regs[1]:get_saved_state_lr(ss));
                uint64_t fp=fresh?now_regs[2]:get_saved_state_fp(ss);
                for (int d=2;d<TL_DEPTH && fp && !(fp&7);d++) {
                    uint64_t frame[2];
                    if (copyinmap(t->map,(vm_map_offset_t)fp,frame,sizeof frame)!=KERN_SUCCESS) break;
                    pc[d]=ustack_strip(frame[1]);
                    if (frame[0]<=fp) break;
                    fp=frame[0];
                }
                for (int d=0;d<TL_DEPTH;d++) if (pc[d]>=t->start && t->start && pc[d]-t->slide<0x300000000ull) pc[d]-=t->slide;
            }
            samples++;
            struct tl_run *last=t->last>=0?&runs[t->last]:NULL;
            if (last && last->run==run && last->cpu==on && !memcmp(last->pc,pc,sizeof pc)) { last->t1=us;continue; }
            if (nruns>=TL_RUNS) { full++;continue; }
            struct tl_run *r=&runs[nruns];r->t0=r->t1=us;r->thr=(uint8_t)i;r->run=run;r->cpu=on;memcpy(r->pc,pc,sizeof pc);
            t->last=(int)nruns++;
        }
    }
    printf("HOUJI TL: %u ms, %u samples (%u running ones read by cross-call), %u runs, %u not kept (buffer full)\n",tl.ms,samples,samples_fresh,nruns,full);
    for (unsigned k=0;runs && k<nruns;k++) {
        const struct tl_run *r=&runs[k];
        char line[20*TL_DEPTH];unsigned at=0;line[0]=0;
        for (int d=0;d<TL_DEPTH && at+20<sizeof line;d++) at+=(unsigned)snprintf(line+at,sizeof line-at," %llx",r->pc[d]);
        printf("HOUJI TL R2 %u %llu %llu %u %u%s\n",r->thr,(unsigned long long)r->t0,(unsigned long long)r->t1,r->run,r->cpu,line);
        delay(1500);
    }
    printf("HOUJI TL: end\n");
    if (runs) kfree_data(runs,sizeof(struct tl_run)*TL_RUNS);
    for (unsigned i=0;i<tl.n;i++) {
        if (tl.t[i].th!=THREAD_NULL) thread_deallocate(tl.t[i].th);
        if (tl.t[i].task!=TASK_NULL) task_deallocate(tl.t[i].task);
    }
    os_atomic_store(&tl_busy,0,release);
    thread_terminate(current_thread());
}
static void tl_command(const char *args)
{
    if (!os_atomic_cmpxchg(&tl_busy,0,1,acquire)) { printf("HOUJI TL: busy\n");return; }
    tl.ms=(unsigned)strtoq(args,NULL,10);if (tl.ms<10 || tl.ms>10000) tl.ms=3000;
    tl.n=0;
    const char *c=strchr(args,' ');
    while (c && tl.n<TL_THREADS) {
        while (*c==' ') c++;
        if (!*c) break;
        struct tl_thread *t=&tl.t[tl.n];bzero(t,sizeof *t);
        t->pid=(int)strtoq(c,NULL,10);
        const char *colon=strchr(c,':'),*end=strchr(c,' ');
        if (colon && (!end || colon<end)) {
            size_t len=(end?(size_t)(end-colon-1):strlen(colon+1));if (len>=sizeof t->name) len=sizeof t->name-1;
            memcpy(t->name,colon+1,len);t->name[len]=0;
        } else strlcpy(t->name,"0",sizeof t->name);
        if (t->pid>0) tl.n++;
        c=end;
    }
    if (!tl.n) { printf("HOUJI TL: tl MS PID:0|PID:NAME ...\n");os_atomic_store(&tl_busy,0,release);return; }
    thread_t th;
    if (kernel_thread_start(tl_thread_main,NULL,&th)!=KERN_SUCCESS) { os_atomic_store(&tl_busy,0,release);return; }
    thread_deallocate(th);
}
/* "zbench": what zeroing a page costs here. A zero-fill page fault zeroes the new page through the kernel's physical aperture
 * (pmap_zero_page); root247: about 7500 faults in backboardd per Control Center open at an estimated ~50 us each. Times 64 rounds of
 * pmap_zero_page on the pages of a kernel buffer against bzero of the same buffer through its ordinary cached mapping. */
extern void pmap_zero_page(ppnum_t);
extern ppnum_t pmap_find_phys(pmap_t,addr64_t);
static void zbench_command(void)
{
    const vm_size_t size=4*PAGE_SIZE;
    char *buf=kalloc_data(size,Z_WAITOK|Z_ZERO);
    if (!buf) { printf("HOUJI ZBENCH: allocation failed\n");return; }
    ppnum_t pages[4];
    for (int i=0;i<4;i++) pages[i]=pmap_find_phys(kernel_pmap,(addr64_t)(uintptr_t)(buf+i*PAGE_SIZE));
    uint64_t t0=mach_absolute_time();
    for (int r=0;r<64;r++) bzero(buf,size);
    uint64_t t1=mach_absolute_time();
    for (int r=0;r<64;r++) for (int i=0;i<4;i++) if (pages[i]) pmap_zero_page(pages[i]);
    uint64_t t2=mach_absolute_time();
    uint64_t a=0,b=0;absolutetime_to_nanoseconds(t1-t0,&a);absolutetime_to_nanoseconds(t2-t1,&b);
    printf("HOUJI ZBENCH: page %u bytes; bzero cached %llu ns/page; pmap_zero_page %llu ns/page (pages %x %x %x %x)\n",(unsigned)PAGE_SIZE,
           a/256,b/256,pages[0],pages[1],pages[2],pages[3]);
    kfree_data(buf,size);
}
/* houji_no_posters=1 (opt-in): the wallpaper gallery's poster extensions are killed (SIGKILL) whenever found, every 3 s.
 * Data is a fresh RAM seed on every boot, so PosterBoard builds its gallery again each time: seven such extensions held about
 * 1.3 GB (houji root151) on a 4 GiB phone whose wired memory leaves ~0.5 GB pageable, and every swipe stalled in page faults.
 * (A MAC exec check would refuse them at spawn, but this kernel's policy list is read-only once the console starts: root153
 * panicked registering one.) The current wallpaper's provider goes too, so its poster may stay empty. Counted in
 * houji_posters_killed. */
#include <sys/signalvar.h>
volatile uint32_t houji_posters_killed;
static const char *const poster_names[]={"AegirPoster","RhizomePoster","MercuryPosterExtension","KaleidoscopePoster",
    "PhotosPosterProvider","GradientPosterExtension","CollectionsPoster"};
static int posters_proc(proc_t p,void *arg __unused)
{
    const char *name=proc_best_name(p);
    for (unsigned i=0;i<sizeof poster_names/sizeof poster_names[0];i++) {
        if (strcmp(name,poster_names[i])) continue;
        psignal(p,SIGKILL);
        houji_posters_killed=houji_posters_killed+1;
        printf("HOUJI POSTERS: killed %s[%d] (%u so far)\n",name,proc_pid(p),houji_posters_killed);
        break;
    }
    return PROC_RETURNED;
}
static void posters_thread(void *arg __unused,wait_result_t result __unused)
{
    for (;;) {
        gesture_sleep_ms(3000);
        proc_iterate(PROC_ALLPROCLIST,posters_proc,NULL,NULL,NULL);
    }
}
static void posters_arm(void)
{
    unsigned v=0;thread_t t;
    if (!PE_parse_boot_argn("houji_no_posters",&v,sizeof v) || v!=1) return;
    if (kernel_thread_start(posters_thread,NULL,&t)==KERN_SUCCESS) thread_deallocate(t);
}
/* "ps PID": the processes with a pid of at least PID (name, pid, parent): the newest ones, which a spawn storm keeps making. */
static int ps_proc(proc_t p,void *arg)
{
    const int from=*(const int *)arg;
    if (proc_pid(p)>=from) printf("HOUJI PS %d %s ppid %d\n",proc_pid(p),proc_best_name(p),proc_ppid(p));
    return PROC_RETURNED;
}
static void ps_command(const char *args)
{
    int from=(int)strtoq(args,NULL,10);
    proc_iterate(PROC_ALLPROCLIST,ps_proc,&from,NULL,NULL);
    printf("HOUJI PS: done, %d processes\n",nprocs);
}
static void qos_command(const char *args)
{
    static struct qos_request r;
    bzero(&r,sizeof r);r.qos=6;
    const char *s=args;size_t i=0;
    while (*s && *s!=' ' && i<sizeof(r.proc)-1) r.proc[i++]=*s++;
    while (*s==' ') s++;
    i=0;
    while (*s && *s!=' ' && i<sizeof(r.prefix)-1) r.prefix[i++]=*s++;
    while (*s==' ') s++;
    if (*s) r.qos=(int)strtoq(s,NULL,10);
    if (!r.proc[0] || !r.prefix[0] || r.qos<1 || r.qos>6) { printf("HOUJI QOS: syntax qos PROC PREFIX [QOS 1..6]\n");return; }
    proc_iterate(PROC_ALLPROCLIST,qos_proc,&r,NULL,NULL);
    printf("HOUJI QOS: %s threads '%s*': %u matched, %u set to qos %d\n",r.proc,r.prefix,r.matched,r.set,r.qos);
}
static int top_collect_proc(proc_t p,void *arg)
{
    struct top_context *c=arg;
    if (c->proc_count==TOP_MAX_PROCS) { c->proc_truncated=true;return PROC_RETURNED_DONE; }
    task_t task=proc_task(p);
    if (task==TASK_NULL) return PROC_RETURNED;
    const unsigned pi=c->proc_count++;
    struct top_proc_sample *s=&c->procs[pi];
    s->pid=proc_pid(p);s->task=task;task_reference(task);
    strlcpy(s->name,proc_best_name(p),sizeof(s->name));
    top_clean_name(s->name,sizeof(s->name),s->pid==0?"kernel_task":"?");
    s->cpu_start=top_task_cpu(task);s->fault_start=top_task_faults(task);s->started=mach_absolute_time();
    s->thread_begin=c->thread_count;
    int n=get_task_numacts(task);
    if (n<0) n=0;
    if (n>(int)(TOP_MAX_THREADS-c->thread_count)) { n=(int)(TOP_MAX_THREADS-c->thread_count); c->thread_truncated=true; }
    s->threads=(unsigned)n;
    if (n>0) {
        const int got=fill_taskthreadlist(task,c->tids,n,true)/(int)sizeof(uint64_t);
        for (int i=0;i<got;++i) {
            struct top_thread_sample *ts=&c->threads[c->thread_count];
            if (!top_thread_ns(task,c->tids[i],&ts->cpu_start,ts->name,sizeof(ts->name))) continue;
            ts->tid=c->tids[i];ts->proc_index=pi;c->thread_count++;
            top_clean_name(ts->name,sizeof(ts->name),"-");
        }
    }
    s->thread_initial=c->thread_count-s->thread_begin;
    return PROC_RETURNED;
}
static void top_finish_proc(struct top_context *c,unsigned pi)
{
    struct top_proc_sample *s=&c->procs[pi];
    const uint64_t end_cpu=top_task_cpu(s->task);
    const uint64_t ended=mach_absolute_time();
    s->elapsed=ended-s->started;
    s->cpu_delta=end_cpu>=s->cpu_start?end_cpu-s->cpu_start:0;
    s->permille=top_permille(s->cpu_delta,s->elapsed);
    s->fault_delta=top_task_faults(s->task)-s->fault_start;
    uint64_t elapsed_ns=0;absolutetime_to_nanoseconds(s->elapsed,&elapsed_ns);
    for (unsigned i=s->thread_begin;i<s->thread_begin+s->thread_initial;++i) {
        struct top_thread_sample *ts=&c->threads[i];
        uint64_t end=0;
        if (!top_thread_ns(s->task,ts->tid,&end,NULL,0)) end=ts->cpu_start;   /* ended in the window: its time is in the task total */
        ts->cpu_delta=end>=ts->cpu_start?end-ts->cpu_start:0;
        ts->permille=top_permille(ts->cpu_delta,elapsed_ns);
    }
}
static void top_thread(void *arg,wait_result_t result __unused)
{
    const unsigned ms=(unsigned)(uintptr_t)arg;
    struct top_context c={0};
    c.procs=kalloc_data(sizeof(*c.procs)*TOP_MAX_PROCS,Z_WAITOK|Z_ZERO);
    c.threads=kalloc_data(sizeof(*c.threads)*TOP_MAX_THREADS,Z_WAITOK|Z_ZERO);
    c.tids=kalloc_data(sizeof(uint64_t)*TOP_MAX_THREADS,Z_WAITOK|Z_ZERO);
    if (!c.procs || !c.threads || !c.tids) { printf("HOUJI TOP: allocation failed\n");goto out; }
    proc_iterate(PROC_ALLPROCLIST,top_collect_proc,&c,NULL,NULL);
    gesture_sleep_ms(ms);
    for (unsigned i=0;i<c.proc_count;++i) top_finish_proc(&c,i);

    unsigned best[15],best_n=0;
    uint64_t total=0,kernel=0;
    for (unsigned i=0;i<c.proc_count;++i) {
        total+=c.procs[i].permille;
        if (c.procs[i].pid==0 || !strcmp(c.procs[i].name,"kernel_task")) kernel=c.procs[i].permille;
        unsigned at=0;while (at<best_n && c.procs[best[at]].cpu_delta>=c.procs[i].cpu_delta) at++;
        if (at<15) {
            if (best_n<15) best_n++;
            for (unsigned j=best_n-1;j>at;--j) best[j]=best[j-1];
            best[at]=i;
        }
    }
    {   /* the eight tasks with the most page faults in the window */
        unsigned fb[8],fn=0;
        for (unsigned i=0;i<c.proc_count;++i) {
            unsigned at=0;while (at<fn && c.procs[fb[at]].fault_delta>=c.procs[i].fault_delta) at++;
            if (at<8) { if (fn<8) fn++; for (unsigned j=fn-1;j>at;--j) fb[j]=fb[j-1]; fb[at]=i; }
        }
        for (unsigned r=0;r<fn;++r) if (c.procs[fb[r]].fault_delta)
            printf("HOUJI TOPF: %d %.63s faults %u\n",c.procs[fb[r]].pid,c.procs[fb[r]].name,c.procs[fb[r]].fault_delta);
    }
    for (unsigned rank=0;rank<best_n;++rank) {
        const struct top_proc_sample *s=&c.procs[best[rank]];
        printf("HOUJI TOP: %d %.63s %llu %u\n",s->pid,s->name,s->permille,s->threads);
        if (rank<3) {
            unsigned tb[3],tn=0;
            for (unsigned i=0;i<c.thread_count;++i) if (c.threads[i].proc_index==best[rank]) {
                unsigned at=0;while (at<tn && c.threads[tb[at]].cpu_delta>=c.threads[i].cpu_delta) at++;
                if (at<3) {
                    if (tn<3) tn++;
                    for (unsigned j=tn-1;j>at;--j) tb[j]=tb[j-1];
                    tb[at]=i;
                }
            }
            for (unsigned i=0;i<tn;++i) {
                const struct top_thread_sample *ts=&c.threads[tb[i]];
                printf("HOUJI TOP THREAD: %d %llu %.63s %llu\n",s->pid,ts->tid,ts->name,ts->permille);
            }
        }
    }
    printf("HOUJI TOP: total %llu permille over %u processes, kernel_task %llu\n",total,c.proc_count,kernel);
    if (c.proc_truncated || c.thread_truncated)
        printf("HOUJI TOP: sample truncated at %u processes or %u threads\n",TOP_MAX_PROCS,TOP_MAX_THREADS);
out:
    if (c.tids) kfree_data(c.tids,sizeof(uint64_t)*TOP_MAX_THREADS);
    for (unsigned i=0;i<c.proc_count;++i) if (c.procs[i].task) task_deallocate(c.procs[i].task);
    if (c.threads) kfree_data(c.threads,sizeof(*c.threads)*TOP_MAX_THREADS);
    if (c.procs) kfree_data(c.procs,sizeof(*c.procs)*TOP_MAX_PROCS);
    os_atomic_store(&top_busy,0,release);
    thread_terminate(current_thread());
}
static void top_command(const char *cmd)
{
    unsigned ms=2000;
    if (cmd[3]) {
        char *end=NULL;long long parsed=strtoq(cmd+4,&end,10);
        if (cmd[3]!=' ' || end==cmd+4 || *end) { printf("HOUJI TOP: syntax top [MS]\n");return; }
        ms=(unsigned)(parsed<200?200:parsed>10000?10000:parsed);
    }
    if (!os_atomic_cmpxchg(&top_busy,0,1,acq_rel)) { printf("HOUJI TOP: busy\n");return; }
    thread_t th;
    if (kernel_thread_start(top_thread,(void *)(uintptr_t)ms,&th)==KERN_SUCCESS) {
        thread_deallocate(th);printf("HOUJI TOP: sampling %u ms\n",ms);
    } else { os_atomic_store(&top_busy,0,release);printf("HOUJI TOP: thread start failed\n"); }
}
extern unsigned native_legacy_swap_timeline(char *out,unsigned cap) __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_offers __attribute__((weak_import)),native_gop_gpu_completions __attribute__((weak_import)),
    native_gop_gpu_failures __attribute__((weak_import)),native_legacy_early_retires __attribute__((weak_import)),
    native_legacy_early_failures __attribute__((weak_import)),native_legacy_gop_status_overrides __attribute__((weak_import));
extern volatile uint32_t native_legacy_gop_status_last __attribute__((weak_import));
extern unsigned native_legacy_prepare_report(char *out,unsigned cap) __attribute__((weak_import));
extern volatile uint64_t houji_frame_intervals[5] __attribute__((weak_import));
extern volatile uint64_t houji_frame_last __attribute__((weak_import));
extern volatile uint64_t houji_frame_fast[2] __attribute__((weak_import));
static void disp_command(const char *cmd)
{
    if (&houji_frame_intervals) {
        if (!strcmp(cmd,"disp reset")) {
            for (unsigned i=0;i<5;i++) houji_frame_intervals[i]=0;
            if (&houji_frame_fast) { houji_frame_fast[0]=0; houji_frame_fast[1]=0; }
            houji_frame_last=0;
        }
        printf("HOUJI DISP INTERVALS: le20 %llu le34 %llu le100 %llu le500 %llu gt500 %llu\n",houji_frame_intervals[0],houji_frame_intervals[1],
            houji_frame_intervals[2],houji_frame_intervals[3],houji_frame_intervals[4]);
        if (&houji_frame_fast) printf("HOUJI DISP FAST: le9 %llu le12 %llu\n",houji_frame_fast[0],houji_frame_fast[1]);
    }
    /* The display driver's own counters (fmt/gpu/cpu are every frame it took), with the uptime: two calls give frames per second. */
    if (!native_legacy_prepare_report) { printf("HOUJI DISP: unavailable\n");return; }
    char line[512];native_legacy_prepare_report(line,sizeof line);
    uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    printf("HOUJI DISP: t=%llu.%03llus %s\n",(unsigned long long)(ns/1000000000ull),(unsigned long long)((ns/1000000ull)%1000),line);
}
extern volatile int native_legacy_frameinfo_override __attribute__((weak_import));
extern volatile uint64_t native_legacy_frameinfo[7] __attribute__((weak_import));
static void fi_command(const char *cmd)
{
    /* fi: the frame-info level and counters; fi N (0..2): override fb-frameinfo at runtime (-1 = the boot argument again). */
    if (!&native_legacy_frameinfo_override) { printf("HOUJI FI: unavailable\n");return; }
    if (cmd[2]) {
        char *end=NULL;long long parsed=strtoq(cmd+3,&end,10);
        if (cmd[2]!=' ' || end==cmd+3 || *end || parsed<-1 || parsed>2) { printf("HOUJI FI: syntax fi [-1|0|1|2]\n");return; }
        native_legacy_frameinfo_override=(int)parsed;
    }
    printf("HOUJI FI: override=%d sel74=%llu completions=%llu notifies=%llu no-client=%llu deferred=%llu early-sent=%llu hooks=%llu\n",
        native_legacy_frameinfo_override,native_legacy_frameinfo[0],native_legacy_frameinfo[1],native_legacy_frameinfo[2],
        native_legacy_frameinfo[3],native_legacy_frameinfo[4],native_legacy_frameinfo[5],native_legacy_frameinfo[6]);
}
extern int houji_rtc_read(uint32_t *seconds, uint8_t *control);
static void rtc_command(void)
{
    /* The PMIC RTC's raw count next to the kernel calendar: their difference is the offset a clock driver would apply. */
    uint32_t secs=0;uint8_t control=0;int rc=houji_rtc_read(&secs,&control);
    clock_sec_t now=0;clock_usec_t us=0;clock_get_calendar_microtime(&now,&us);
    printf("HOUJI RTC: rc=%d raw=%u control=%02x enabled=%u calendar=%lu calendar-minus-raw=%lld\n",rc,secs,control,(control>>7)&1,
        (unsigned long)now,(long long)now-(long long)secs);
    extern int houji_rtc_offset_read(uint32_t *offset);
    uint32_t offset=0;int orc=houji_rtc_offset_read(&offset);
    printf("HOUJI RTC: sdam 0x71bc offset rc=%d value=%u raw+offset=%llu\n",orc,offset,(unsigned long long)secs+offset);
}
extern volatile uint64_t native_adreno_fence_wake[8] __attribute__((weak_import));
extern volatile uint64_t native_adreno_wait_outer[5] __attribute__((weak_import));
static void fence_command(const char *cmd)
{
    /* "fence [reset]": the GPU fence waits as the render server sees them (NativeAdrenoPublish.cpp): wake = interrupts, fence-bit
     * interrupts, completion passes, waiters a pass woke, waits that slept, sleeps that timed out, waiters the periodic pass woke, ring switches; wait = waits, us waited in total, pauses, longest us, lock-free polls. */
    if (!&native_adreno_fence_wake || !&native_adreno_wait_outer) { printf("HOUJI FENCE: unavailable\n");return; }
    if (!strcmp(cmd,"fence reset")) { for (unsigned i=0;i<8;i++) native_adreno_fence_wake[i]=0; for (unsigned i=0;i<5;i++) native_adreno_wait_outer[i]=0; }
    const uint64_t waits=native_adreno_wait_outer[0];
    printf("HOUJI FENCE: irq %llu fence-irq %llu passes %llu pass-woke %llu slept %llu timed-out %llu periodic-woke %llu switches %llu wait n=%llu total-us=%llu pauses=%llu longest-us=%llu nolock=%llu avg-us=%llu\n",
        native_adreno_fence_wake[0],native_adreno_fence_wake[1],native_adreno_fence_wake[2],native_adreno_fence_wake[3],
        native_adreno_fence_wake[4],native_adreno_fence_wake[5],native_adreno_fence_wake[6],native_adreno_fence_wake[7],waits,native_adreno_wait_outer[1],native_adreno_wait_outer[2],
        native_adreno_wait_outer[3],native_adreno_wait_outer[4],waits?native_adreno_wait_outer[1]/waits:0);
}
/* "cores HEXMASK": the CPUs the scheduler may run ordinary threads on (bit n = cpu n), through the perf-control recommendation CLPC
 * uses (sched_perfcontrol_update_recommended_cores). This kernel has no __AMP__, so it schedules with Clutch alone and treats the
 * two in-order Cortex-A520s (cpu0, cpu1) like the A720/X4 cores: "cores fc" keeps the render server's threads off them. "cores ff"
 * recommends every core again. */
extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);
static void cores_command(const char *arg)
{
    char *end=NULL;
    const long long mask=strtoq(arg,&end,16);
    if (end==arg || *end || mask<1 || mask>0xff) { printf("HOUJI CORES: syntax cores HEXMASK (1..ff, bit n = cpu n)\n");return; }
    sched_perfcontrol_update_recommended_cores((uint32_t)mask);
    printf("HOUJI CORES: recommended mask %02llx\n",mask);
}
static void gop_command(void)
{
#define GOPV(x) (&(x)?(unsigned long long)(x):~0ull)
    printf("HOUJI GOP: offers %llu completions %llu failures %llu early-retires %llu early-failures %llu status-overrides %llu last %#x\n",
        GOPV(native_gop_gpu_offers),GOPV(native_gop_gpu_completions),GOPV(native_gop_gpu_failures),GOPV(native_legacy_early_retires),
        GOPV(native_legacy_early_failures),GOPV(native_legacy_gop_status_overrides),&native_legacy_gop_status_last?native_legacy_gop_status_last:~0u);
#undef GOPV
}
static void swaps_command(void)
{
    /* The display driver's last 128 swaps (submitted, offered to the copy, done), same clock as HOUJI GESTURE AT. */
    if (!native_legacy_swap_timeline) { printf("HOUJI SWAPS: unavailable\n");return; }
    char *buffer=kalloc_data(12288,Z_WAITOK|Z_ZERO);
    if (!buffer) return;
    native_legacy_swap_timeline(buffer,12288);
    for (char *line=buffer,*next;line && *line;line=next) {
        next=strchr(line,'\n');
        if (next) *next++=0;
        printf("HOUJI SWAPS: %s\n",line);
        delay(2000);
    }
    kfree_data(buffer,12288);
}
/* One line per tap or swipe: when each contact change was published and when NativeHidInput handed each touch report to the HID
 * event driver (native_touch_recent), in ms from the touch-down: span, largest step, and the delay of the first and last report. */
extern volatile uint64_t native_touch_recent[256];
extern volatile uint32_t native_touch_recent_count;
static uint64_t gesture_us(uint64_t a,uint64_t b) { uint64_t ns;absolutetime_to_nanoseconds(b>a?b-a:0,&ns);return ns/1000; }
static void gesture_timing(const uint64_t *pub,int np,uint32_t r0)
{
    const uint32_t r1=native_touch_recent_count;
    uint32_t nr=r1-r0;if (nr>256) nr=256;
    uint64_t pmax=0,rmax=0;
    for (int i=1;i<np;i++) { uint64_t g=gesture_us(pub[i-1],pub[i]);if (g>pmax) pmax=g; }
    for (uint32_t i=1;i<nr;i++) { uint64_t g=gesture_us(native_touch_recent[(r1-nr+i-1)&255],native_touch_recent[(r1-nr+i)&255]);if (g>rmax) rmax=g; }
    const uint64_t first=nr?native_touch_recent[(r1-nr)&255]:0,last=nr?native_touch_recent[(r1-1)&255]:0;
    printf("HOUJI GESTURE TIMING: published %d span %llu us max-step %llu us; reports %u first +%llu us last +%llu us (lift +%llu us) max-step %llu us\n",
           np,gesture_us(pub[0],pub[np-1]),pmax,r1-r0,nr?gesture_us(pub[0],first):0ull,nr?gesture_us(pub[0],last):0ull,
           nr?(last>=pub[np-1]?gesture_us(pub[np-1],last):0ull):0ull,rmax);
}
static void gesture_thread(void *arg __unused,wait_result_t result __unused)
{
    {uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
     printf("HOUJI GESTURE AT t=%llu.%03llus\n",(unsigned long long)(ns/1000000000ull),(unsigned long long)((ns/1000000ull)%1000));}
    if (gesture.kind==3) {
        sweet_hid_key(3,1);gesture_sleep_ms(80);sweet_hid_key(3,0);
    } else {
        int steps=gesture.kind==1?1:(gesture.ms/8>2?gesture.ms/8:2);
        const uint32_t r0=native_touch_recent_count;
        uint64_t pub[258];int np=0;
        pub[np++]=mach_absolute_time();
        sweet_hid_touch(0,1,gesture.x1,gesture.y1);
        for (int i=1;i<=steps;i++) {
            gesture_sleep_ms(gesture.kind==1?60:8);
            int x=gesture.x1+(gesture.x2-gesture.x1)*i/steps,y=gesture.y1+(gesture.y2-gesture.y1)*i/steps;
            if (np<257) pub[np++]=mach_absolute_time();
            sweet_hid_touch(0,1,x,y);
        }
        if (np<258) pub[np++]=mach_absolute_time();
        sweet_hid_touch(0,0,gesture.x2,gesture.y2);
        gesture_sleep_ms(150);
        gesture_timing(pub,np,r0);
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
    /* "\n" in the text is the Return key, "\b" Backspace */
    size_t o=0;
    for (const char *c=text;*c && o<sizeof(type_text)-1;c++) {
        if (c[0]=='\\' && c[1]=='n') { type_text[o++]='\n'; c++; }
        else if (c[0]=='\\' && c[1]=='b') { type_text[o++]='\b'; c++; }
        else type_text[o++]=*c;
    }
    type_text[o]=0;
    thread_t t;
    if (kernel_thread_start(type_thread,NULL,&t)!=KERN_SUCCESS) { bzero(type_text,sizeof(type_text));os_atomic_store(&type_busy,0,release);return; }
    thread_deallocate(t);
}
/*
 * Pointer authentication check for an arm64e system (console "pac" and "pac keys"). "pac" reads the two feature registers on
 * every running CPU. "pac keys" also writes the IA and GA keys at EL1 and signs and authenticates one pointer with EnIA set for
 * the length of the test; a hypervisor that traps key access or the instructions turns this into a panic, which is the answer.
 */
/*
 * Physical memory outside what this kernel owns (console "mem w ADDR", "mem r ADDR", "mem t ADDR LEN", hexadecimal): w fills
 * 1 MiB with a pattern derived from each word's address, r counts the words that still hold it and shows the first four, t
 * prints the printable text found there. For checking whether RAM keeps its contents across a restart, and for reading what
 * another kernel left there.
 */
static uint64_t mem_hex(const char **s)
{
    uint64_t v=0;
    while (**s==' ') ++*s;
    for (;;++*s) {
        const char c=**s;
        if (c>='0' && c<='9') v=v<<4|(uint64_t)(c-'0');
        else if (c>='a' && c<='f') v=v<<4|(uint64_t)(c-'a'+10);
        else break;
    }
    return v;
}
static void mem_thread(void *arg,wait_result_t result __unused)
{
    const char *s=arg;const char op=*s++;
    const uint64_t pa=mem_hex(&s)&~0xfffull;uint64_t length=op=='t'?mem_hex(&s):0x100000;
    if (!pa || !length || length>0x1000000 || (op!='w' && op!='r' && op!='t')) printf("HOUJI MEM: mem w|r ADDR, mem t ADDR LEN\n");
    else {
        length=(length+0xfff)&~0xfffull;
        const vm_offset_t va=ml_io_map(pa,length);
        if (!va) printf("HOUJI MEM: cannot map %llx\n",pa);
        else if (op=='w') {
            for (uint64_t i=0;i<length;i+=8) *(volatile uint64_t *)(va+i)=(pa+i)*0x9e3779b97f4a7c15ull;
            printf("HOUJI MEM: wrote pattern %llx +%llx\n",pa,length);
        } else if (op=='r') {
            uint64_t same=0;
            for (uint64_t i=0;i<length;i+=8) same+=*(volatile uint64_t *)(va+i)==(pa+i)*0x9e3779b97f4a7c15ull;
            printf("HOUJI MEM: %llx pattern words %llu of %llu; first %016llx %016llx %016llx %016llx\n",pa,same,length/8,
                *(volatile uint64_t *)va,*(volatile uint64_t *)(va+8),*(volatile uint64_t *)(va+16),*(volatile uint64_t *)(va+24));
        } else {
            char line[100];unsigned n=0;uint64_t lines=0;
            for (uint64_t i=0;i<length;++i) {
                const char c=*(volatile char *)(va+i);
                if (c>=32 && c<127 && n<sizeof(line)-1) line[n++]=c;
                if (c=='\n' || n==sizeof(line)-1) {
                    if (n) { line[n]=0;printf("HOUJI MEM| %s\n",line);if (!(++lines&31)) delay(20000); }
                    n=0;
                }
            }
            printf("HOUJI MEM: text end, %llu lines\n",lines);
        }
    }
    thread_terminate(current_thread());
}

static struct { uint64_t isar1,isar2,signed_ptr,auth_ptr,ga; uint32_t seen; } pac_result[8];
static void pac_xcall(void *arg)
{
    const int cpu=cpu_number();
    if (cpu<0 || cpu>=8) return;
    pac_result[cpu].isar1=__builtin_arm_rsr64("ID_AA64ISAR1_EL1");
    pac_result[cpu].isar2=__builtin_arm_rsr64("S3_0_C0_C6_2");
    pac_result[cpu].seen=1;
    if (!arg) return;
    const uint64_t lo=__builtin_arm_rsr64("S3_0_C2_C1_0"),hi=__builtin_arm_rsr64("S3_0_C2_C1_1");
    const uint64_t glo=__builtin_arm_rsr64("S3_0_C2_C3_0"),ghi=__builtin_arm_rsr64("S3_0_C2_C3_1");
    const uint64_t sctlr=__builtin_arm_rsr64("SCTLR_EL1");
    __builtin_arm_wsr64("S3_0_C2_C1_0",0x0123456789abcdefull);__builtin_arm_wsr64("S3_0_C2_C1_1",0xfedcba9876543210ull);
    __builtin_arm_wsr64("S3_0_C2_C3_0",0x1111222233334444ull);__builtin_arm_wsr64("S3_0_C2_C3_1",0x5555666677778888ull);
    __builtin_arm_wsr64("SCTLR_EL1",sctlr|(1ull<<31));__builtin_arm_isb(15);
    register uint64_t x0 __asm__("x0")=0xfffffff007004000ull;
    register uint64_t x1 __asm__("x1")=0x1234;
    register uint64_t x2 __asm__("x2")=0x5678;
    __asm__ volatile(".inst 0xdac10020":"+r"(x0):"r"(x1));          /* pacia x0, x1 */
    pac_result[cpu].signed_ptr=x0;
    __asm__ volatile(".inst 0xdac11020":"+r"(x0):"r"(x1));          /* autia x0, x1 */
    pac_result[cpu].auth_ptr=x0;
    __asm__ volatile(".inst 0x9ac23020":"+r"(x0):"r"(x1),"r"(x2));  /* pacga x0, x1, x2 */
    pac_result[cpu].ga=x0;
    __builtin_arm_wsr64("SCTLR_EL1",sctlr);__builtin_arm_isb(15);
    __builtin_arm_wsr64("S3_0_C2_C1_0",lo);__builtin_arm_wsr64("S3_0_C2_C1_1",hi);
    __builtin_arm_wsr64("S3_0_C2_C3_0",glo);__builtin_arm_wsr64("S3_0_C2_C3_1",ghi);
    pac_result[cpu].seen=2;
}
static void pac_thread(void *arg,wait_result_t result __unused)
{
    extern unsigned int cpu_broadcast_xcall_simple(boolean_t,void (*)(void *),void *);
    bzero(pac_result,sizeof(pac_result));
    printf("HOUJI PAC: %s, CurrentEL %llu\n",arg?"keys and instructions":"feature registers",(__builtin_arm_rsr64("CurrentEL")>>2)&3);
    (void)cpu_broadcast_xcall_simple(TRUE,pac_xcall,arg);
    for (int i=0;i<8;++i) {
        if (!pac_result[i].seen) { printf("HOUJI PAC: cpu%d did not answer\n",i); continue; }
        printf("HOUJI PAC: cpu%d isar1 %016llx isar2 %016llx\n",i,pac_result[i].isar1,pac_result[i].isar2);
        if (pac_result[i].seen==2)
            printf("HOUJI PAC: cpu%d signed %016llx auth %016llx ga %016llx %s\n",i,pac_result[i].signed_ptr,pac_result[i].auth_ptr,
                pac_result[i].ga,pac_result[i].signed_ptr!=0xfffffff007004000ull && pac_result[i].auth_ptr==0xfffffff007004000ull &&
                (pac_result[i].ga>>32)?"ok":"FAILED");
    }
    thread_terminate(current_thread());
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
    else if (!strcmp(u.command,"top") || !strncmp(u.command,"top ",4)) top_command(u.command);
    else if (!strcmp(u.command,"shot")) { extern void houji_framebuffer_shot(unsigned,unsigned); houji_framebuffer_shot(150,334); }
    else if (!strncmp(u.command,"shotr ",6)) {
        extern void houji_framebuffer_shot_region(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
        int r[4];
        if (gesture_numbers(u.command+6,r,4)==4) {
            unsigned cols=r[2]>=r[3]?300:(unsigned)(300ull*r[2]/r[3]),rows=r[2]>=r[3]?(unsigned)(300ull*r[3]/r[2]):300;
            houji_framebuffer_shot_region(r[0],r[1],r[2],r[3],cols?cols:1,rows?rows:1);
        } else printf("HOUJI DEBUG: shotr X Y W H\n");
    }
    else if (!strcmp(u.command,"swaps")) swaps_command();
    else if (!strcmp(u.command,"gop")) gop_command();
    else if (!strcmp(u.command,"disp") || !strcmp(u.command,"disp reset")) disp_command(u.command);
    else if (!strcmp(u.command,"rtc")) rtc_command();
    else if (!strcmp(u.command,"fence") || !strcmp(u.command,"fence reset")) fence_command(u.command);
    else if (!strncmp(u.command,"qos ",4)) qos_command(u.command+4);
    else if (!strncmp(u.command,"cores ",6)) cores_command(u.command+6);
    else if (!strncmp(u.command,"ustack ",7)) ustack_command(u.command+7);
    else if (!strncmp(u.command,"threads ",8)) threads_command(u.command+8);
    else if (!strcmp(u.command,"vm")) vm_command();
    else if (!strcmp(u.command,"zbench")) zbench_command();
    else if (!strncmp(u.command,"tl ",3)) tl_command(u.command+3);
    else if (!strcmp(u.command,"gm")) gm_command();
    else if (!strcmp(u.command,"aks")) { extern void native_aks_report(void) __attribute__((weak_import)); if (native_aks_report) native_aks_report(); else printf("HOUJI AKS: unavailable\n"); }
    else if (!strcmp(u.command,"io")) io_command();
    else if (!strncmp(u.command,"ps ",3)) ps_command(u.command+3);
    else if (!strcmp(u.command,"mt")) {
        /* The render server's Metal bridge telemetry table (diag=1; NativeAdrenoPublish.cpp): its memory pools, purge and cache rows. */
        extern char native_adreno_metal_rows[48][96];
        for (int r=0;r<48;r++) if (native_adreno_metal_rows[r][0]) printf("HOUJI MT %02d %.95s\n",r,native_adreno_metal_rows[r]);
    }
    else if (!strcmp(u.command,"fi") || !strncmp(u.command,"fi ",3)) fi_command(u.command);
    else if (!strcmp(u.command,"home") || !strncmp(u.command,"tap ",4) || !strncmp(u.command,"swipe ",6)) gesture_command(u.command);
    else if (!strncmp(u.command,"readfile ",9) || !strncmp(u.command,"cat ",4) || !strncmp(u.command,"ls ",3) || !strncmp(u.command,"hex ",4) || !strncmp(u.command,"sshot ",6)) {
        int expected=0;thread_t th;
        if (!os_atomic_cmpxchg(&readfile_busy,expected,1,acq_rel)) printf("HOUJI READFILE: busy\n");
        else {
            readfile_mode=u.command[0]=='c'?1:u.command[0]=='l'?2:u.command[0]=='h'?3:u.command[0]=='s'?4:0;
            strlcpy(readfile_path,u.command+(readfile_mode==1||readfile_mode==3?4:readfile_mode==2?3:readfile_mode==4?6:9),sizeof(readfile_path));
            hex_start=0;hex_length=2u<<20;
            if (readfile_mode==3) {
                /* Optional trailing hex numbers after the path (paths here have no spaces): START, then LENGTH. */
                char *space=strchr(readfile_path,' ');
                if (space) {
                    *space++=0;
                    char *end=space;
                    hex_start=strtoq(space,&end,16);
                    if (*end==' ') hex_length=strtoq(end+1,NULL,16);
                    if (hex_length>(8u<<20)) hex_length=8u<<20;
                }
            }
            if (kernel_thread_start(readfile_thread,NULL,&th)==KERN_SUCCESS) thread_deallocate(th);
            else os_atomic_store(&readfile_busy,0,release);
        }
    }
    else if (!strcmp(u.command,"commpage")) {
        /* The user commpage as processes see it (kernel alias commPagePtr): what libsystem/dyld read to choose timers, TPRO and APT. */
        extern vm_address_t commPagePtr;
        const uint8_t *cp=(const uint8_t *)commPagePtr;
        if (!cp) printf("HOUJI COMMPAGE: not set up\n");
        else for (unsigned at=0;at<0x400;at+=32) {
            char line[120];unsigned used=0;
            for (unsigned j=at;j<at+32;j++) used+=(unsigned)snprintf(line+used,sizeof(line)-used,"%02x%s",cp[j],(j&3)==3?" ":"");
            printf("HOUJI COMMPAGE +%03x: %s\n",at,line);
        }
    }
    else if (!strcmp(u.command,"dvfs")) { extern void houji_cpu_dvfs_report(void); houji_cpu_dvfs_report(); }
    else if (!strncmp(u.command,"notes ",6)) {
        /* "notes N": the newest N rows of the GPU driver's pass log (NativeAdrenoPublish.cpp, NativeAdrenoMethodNote kind 1) whose first
         * word is the Metal bridge's counter mark 0x4d32434e (NativeMetalCounts.rs, m2v-count=1): pid, then its words. */
        extern volatile uint32_t *native_adreno_pass_log;extern volatile uint32_t native_adreno_pass_count;
        unsigned want=0;const char *p=u.command+6;while (*p>='0' && *p<='9') want=want*10+(unsigned)(*p++-'0');
        volatile uint32_t *rows=native_adreno_pass_log;const uint32_t n=native_adreno_pass_count;unsigned shown=0;
        for (uint32_t k=0;rows && k<n && k<512 && shown<want;k++) {
            volatile uint32_t *r=rows+((n-1-k)%512)*32;
            if (r[2]==0x53555246u) {
                /* the frontend's IOSurface texture notes (adrenometal-zzz-surface-notes.patch) */
                printf("HOUJI SURF %u: desc %ux%u fmt %u | surface %ux%u rows %u fmt %08x planes %u alloc %u KiB | plane %u rows %u %s\n",
                       r[1],r[3],r[4],r[5],r[6],r[7],r[8],r[9],r[10],r[11],r[12],r[13]&0x7fffffffu,(r[13]>>31)?"made":"refused");shown++;
                continue;
            }
            if (r[2]!=0x4d32434eu) continue;
            printf("HOUJI NOTE %u: %u %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u %u %u\n",r[1],r[3],r[4],r[5],r[6],r[7],r[8],r[9],r[10],r[11],
                   r[12],r[13],r[14],r[15],r[16],r[17],r[18],r[19],r[20],r[21]);shown++;
        }
        printf("HOUJI NOTES: %u shown of %u\n",shown,n);
    }
    else if (!strncmp(u.command,"ddr ",4)) {
        /* HoujiCpuDvfs.cpp houji_ddr_vote: "ddr Y", Y decimal (16383 top, 0 withdraws) */
        extern void houji_ddr_vote(unsigned y);
        unsigned v=0;const char *p=u.command+4;while (*p>='0' && *p<='9') v=v*10+(unsigned)(*p++-'0');
        houji_ddr_vote(v);
    }
    else if (!strcmp(u.command,"l3") || !strncmp(u.command,"l3 vote ",8) || !strncmp(u.command,"l3 perf ",8) || !strncmp(u.command,"l3 mode ",8)) {
        /* HoujiCpuDvfs.cpp houji_cpu_l3: the L3 clock's table and state; "l3 vote N" / "l3 perf N" write row N */
        extern void houji_cpu_l3(int op,unsigned value);
        unsigned v=0;const char *p=u.command[2]?u.command+8:"";while (*p>='0' && *p<='9') v=v*10+(unsigned)(*p++-'0');
        houji_cpu_l3(!u.command[2]?0:u.command[3]=='v'?1:u.command[3]=='m'?3:2,v);
    }
    else if (!strncmp(u.command,"dvfs set ",9)) {
        /* "dvfs set NAME VALUE" (HoujiCpuDvfs.cpp houji_cpu_dvfs_set) */
        extern void houji_cpu_dvfs_set(const char *name,int value);
        char name[16]={0};const char *p=u.command+9;unsigned k=0;
        while (*p && *p!=' ' && k+1<sizeof name) name[k++]=*p++;
        while (*p==' ') p++;
        int neg=0;if (*p=='-') { neg=1;p++; }
        int v=0;while (*p>='0' && *p<='9') v=v*10+(*p++-'0');
        houji_cpu_dvfs_set(name,neg?-v:v);
    }
    else if (!strcmp(u.command,"temp")) { extern void houji_thermal_report(void); houji_thermal_report(); }
    else if (!strcmp(u.command,"hid")) {
        /* NativeHidInput's report counters (HidInputProbe.cpp): 0 init 1 attach 2 start 3 timer 4 key-down/buttons 5 key-up 6 touch 7 fail. */
        extern volatile uint32_t native_hid_input_events[8];
        extern volatile int native_hid_input_results[8];
        extern volatile uint32_t native_touch_hid_reports;
        printf("HOUJI HID: events %u %u %u %u %u %u %u %u results %x %x %x %x %x %x %x %x touch-reports %u\n",
            native_hid_input_events[0],native_hid_input_events[1],native_hid_input_events[2],native_hid_input_events[3],
            native_hid_input_events[4],native_hid_input_events[5],native_hid_input_events[6],native_hid_input_events[7],
            native_hid_input_results[0],native_hid_input_results[1],native_hid_input_results[2],native_hid_input_results[3],
            native_hid_input_results[4],native_hid_input_results[5],native_hid_input_results[6],native_hid_input_results[7],
            native_touch_hid_reports);
    }
    else if (!strcmp(u.command,"buttons")) { extern void houji_buttons_report(void); houji_buttons_report(); }
    else if (!strcmp(u.command,"pon")) { extern void houji_pon_report(void); houji_pon_report(); }
    else if (!strcmp(u.command,"touch")) { houji_touch_report(); houji_touch_driver_report(); }
    else if (!strcmp(u.command,"key power") || !strcmp(u.command,"key up") || !strcmp(u.command,"key down")) {
        /* In a thread that sleeps while the key is held: the buttons sink only records a mask that the HID side reads later, so a press
           busy-waited inside the console (the old delay(120000)) was released before anyone saw it (fps84b: three presses, no wake). */
        static int key_index;thread_t t;
        key_index=u.command[4]=='p'?0:u.command[4]=='u'?1:2;
        if (kernel_thread_start(key_thread,&key_index,&t)==KERN_SUCCESS) thread_deallocate(t);
    }
    else if (!strncmp(u.command,"prof ",5)) { extern void houji_prof_command(const char *); houji_prof_command(u.command+5); }
    else if (!strncmp(u.command,"camera ",7)) { extern void houji_camera_command(const char *); houji_camera_command(u.command+7); }
    else if (!strncmp(u.command,"screen ",7)) { extern void houji_screen_command(const char *); houji_screen_command(u.command+7); }
    else if (!strncmp(u.command,"type ",5)) { type_command(u.command+5); bzero(u.command,sizeof(u.command)); }
    else if (!strcmp(u.command,"sleep")) { extern void houji_sleep_now(void); houji_sleep_now(); }
    else if (!strcmp(u.command,"panic")) panic("HOUJI DEBUG: panic requested over USB");
    else if (!strncmp(u.command,"pw ",3)) { extern void houji_power_command(const char *); houji_power_command(u.command+3); }
    else if (!strcmp(u.command,"usb phy")) houji_usb_phy_state("now");
    else if (!strncmp(u.command,"mem ",4) && u.command[4] && u.command[5]==' ') {
        static char mem_request[64];thread_t t;
        strlcpy(mem_request,u.command+4,sizeof(mem_request));
        if (kernel_thread_start(mem_thread,mem_request,&t)==KERN_SUCCESS) thread_deallocate(t);
    }
    else if (!strcmp(u.command,"pac") || !strcmp(u.command,"pac keys")) {
        thread_t t;
        if (kernel_thread_start(pac_thread,(void *)(uintptr_t)(u.command[3]!=0),&t)==KERN_SUCCESS) thread_deallocate(t);
    }
    else if (!strncmp(u.command,"sysctl ",7)) {
        /* sysctl NAME prints an integer sysctl; sysctl NAME=VALUE sets it (32-bit when the old value is 4 bytes, else 64-bit). */
        extern int kernel_sysctlbyname(const char *,void *,size_t *,void *,size_t);
        char name[56];strlcpy(name,u.command+7,sizeof(name));
        char *eq=strchr(name,'=');long long set=0;
        if (eq) {*eq=0;for (const char *d=eq+1;*d>='0' && *d<='9';++d) set=set*10+(*d-'0');}
        uint64_t value=0;size_t size=sizeof(value);
        int error=kernel_sysctlbyname(name,&value,&size,NULL,0);
        printf("HOUJI SYSCTL: %s = %llu (size %zu, error %d)\n",name,size==4?(unsigned long long)(uint32_t)value:(unsigned long long)value,size,error);
        if (eq && !error) {
            uint32_t v32=(uint32_t)set;uint64_t v64=(uint64_t)set;
            error=size==4?kernel_sysctlbyname(name,NULL,NULL,&v32,4):kernel_sysctlbyname(name,NULL,NULL,&v64,8);
            value=0;size=sizeof(value);kernel_sysctlbyname(name,&value,&size,NULL,0);
            printf("HOUJI SYSCTL: set %s error %d, now %llu\n",name,error,size==4?(unsigned long long)(uint32_t)value:(unsigned long long)value);
        }
    }
    else if (!strcmp(u.command,"pm")) { extern void houji_pm_report(void); houji_pm_report(); }
    else if (!strcmp(u.command,"batt") || !strncmp(u.command,"batt ",5)) { extern void houji_batt_command(const char *); houji_batt_command(u.command[4]?u.command+5:""); }
    else if (!strcmp(u.command,"gpu clients")) { extern void native_adreno_client_report(void); native_adreno_client_report(); }
    else if (!strncmp(u.command,"cpu off ",8) || !strncmp(u.command,"cpu on ",7)) {
        extern void houji_cpu_hotplug(unsigned, int);
        const int on=u.command[5]=='n';
        houji_cpu_hotplug((unsigned)(u.command[on?7:8]-'0'),on);
    }
    else if (!strncmp(u.command,"bt ",3)) { extern void houji_bt_command(const char *); houji_bt_command(u.command+3); }
    else if (!strncmp(u.command,"wifi ",5)) { extern void houji_wifi_command(const char *); houji_wifi_command(u.command+5); bzero(u.command,sizeof(u.command)); }
    else if (!strcmp(u.command,"reboot bootloader")) {
        extern int houji_reboot_reason_bootloader(void);
        static bool requested;thread_t t;int rc=houji_reboot_reason_bootloader();
        if (rc) printf("HOUJI DEBUG: reboot reason not set (%d); not rebooting\n",rc);
        else if (!requested && kernel_thread_start(reboot_thread,(void *)1,&t)==KERN_SUCCESS) { requested=true;thread_deallocate(t); }
    }
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
    } else if (u.command_n) printf("HOUJI DEBUG: commands help status ping replay reboot top tap swipe home shot shotr wifi bt ls cat hex sshot sysctl readfile\n");
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
        int n=complete(ep);
        if (n==-2) { if (!u.busy[0] && !u.busy[1]) setup_arm(); return; }
        if (n<0) return;
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
/* One round of controller events. */
static bool usb_events(void)
{
    unsigned count=rd(GEVNTCOUNT)&0xffff;
    if (count>EVENT_SIZE || (count&3)) { fail("event ring size/alignment");return false; }
    if (count) {
        sync_dma(EVENTS,EVENT_SIZE);
        for (unsigned i=0;i<count && !u.failed;i+=4) {
            uint32_t e=*(volatile uint32_t *)(u.dma+u.pos);
            u.pos=(u.pos+4)%EVENT_SIZE;u.events++;
            event(e);
        }
        wr(GEVNTCOUNT,count);
    }
    return true;
}
/* Restart after a panic (halt_all_cpus; interrupts off, the other CPUs stopped, the poll thread will never run again): send what
 * the RAM log still holds, the panic text included, for up to two seconds or until the host has read all of it. */
int houji_usb_connected(void);
int houji_usb_connected(void) { return usb_started && u.configured && u.dtr && !u.failed; }
void houji_usb_panic_drain(void);
void houji_usb_panic_drain(void)
{
    extern void houji_debug_panic_mode(void);
    extern unsigned panic_active(void);
    static bool once;
    if (once || !usb_started || u.failed) return;
    /* An orderly restart (reboot(2) from launchd or a daemon) comes here too, without a panic: its last log lines would be lost
     * with the USB thread, so they are sent the same way. */
    const bool panicking=panic_active()!=0;
    once=true;houji_debug_panic_mode();
    if (!panicking) printf("HOUJI RESET: restart without a panic; sending the rest of the log\n");
    else printf("HOUJI RESET: restart after a panic\n");
    /* An early panic can come before the host has enumerated the console: wait up to 20 s for a reader. */
    for (unsigned i=0;i<40000 && !u.failed && !(u.configured && u.dtr);i++) {
        if (!usb_events()) break;
        delay(500);
    }
    if (!u.configured || !u.dtr || u.failed) return;
    for (unsigned i=0;i<4000 && !u.failed;i++) {
        if (!usb_events()) break;
        transmit();
        if (!u.busy[3] && u.cursor>=houji_debug_total() && i>20) break;
        delay(500);
    }
}
static void usb_thread(void *arg __unused,wait_result_t result __unused)
{
    while (!u.failed) {
        if (!usb_events()) break;
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
    wr(DEVTEN,0);dctl_write(rd(DCTL)&~RUN);
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
/* houji_usb_watchdog_s=N (tests of a power-on boot): no host has configured the console N seconds after the driver started ->
 * reboot to the bootloader (fastboot), where a PC can take over. */
static void watchdog_thread(void *arg,wait_result_t result __unused)
{
    unsigned seconds=(unsigned)(uintptr_t)arg;
    for (unsigned i=0;i<seconds;i++) {
        if (u.configured) { printf("HOUJI USB: watchdog satisfied after %u s\n",i);thread_terminate(current_thread());return; }
        gesture_sleep_ms(1000);
    }
    unote("HOUJI USB: watchdog: console not configured after %u s (failed=%u events=%u); rebooting\n",seconds,u.failed,u.events);
    houji_usb_report();
    gesture_sleep_ms(25000);   /* one more saved checkpoint */
    { extern int houji_reboot_reason_bootloader(void); printf("HOUJI USB: watchdog: bootloader reason %d\n",houji_reboot_reason_bootloader()); }
    houji_restart((char *)"houji usb watchdog");
    thread_terminate(current_thread());
}
/* houji_reset_after_s=N: N seconds after start the phone restarts into the bootloader (fastboot), whatever state the system is
 * in, so an unattended test boot ends by itself. */
static void reset_after_thread(void *arg,wait_result_t result __unused)
{
    extern int houji_reboot_reason_bootloader(void);
    gesture_sleep_ms(1000u*(unsigned)(uintptr_t)arg);
    printf("HOUJI RESET: houji_reset_after_s elapsed; bootloader reason %d\n",houji_reboot_reason_bootloader());
    gesture_sleep_ms(500);
    houji_restart((char *)"houji_reset_after_s");
    thread_terminate(current_thread());
}
void houji_usb_debug_start(void)
{
    static bool attempted;
    { static bool posters_armed; if (!posters_armed) { posters_armed=true; posters_arm(); } }
    {
        static bool armed;unsigned seconds=0;thread_t t;
        if (!armed && PE_parse_boot_argn("houji_reset_after_s",&seconds,sizeof(seconds)) && seconds>=5) {
            armed=true;
            if (kernel_thread_start(reset_after_thread,(void *)(uintptr_t)seconds,&t)==KERN_SUCCESS) thread_deallocate(t);
        }
    }
    { extern void houji_power_start(void); static bool power; if (!power) { power=true; houji_power_start(); } }
    { extern void houji_pon_report(void); static bool pon; if (!pon) { pon=true; houji_pon_report(); } }
    { extern void houji_thermal_start(void); houji_thermal_start(); }
    { extern void houji_cpu_dvfs_start(void); houji_cpu_dvfs_start(); }
    {
        static bool hb;unsigned ms=0;thread_t th;
        if (!hb && PE_parse_boot_argn("houji_heartbeat_ms",&ms,sizeof(ms)) && ms>=10 && ms<=1000) {
            hb=true;
            if (kernel_thread_start(heartbeat_thread,(void *)(uintptr_t)ms,&th)==KERN_SUCCESS) thread_deallocate(th);
        }
    }
    {
        /* houji_reboot_bootloader=1: every restart (console, iOS, panic) stops in the bootloader instead of starting boot_a again. */
        static bool reason;int always=0;extern int houji_reboot_reason_prepare(int);
        if (!reason && PE_parse_boot_argn("houji_reboot_bootloader",&always,sizeof(always)) && always==1) {
            reason=true;printf("HOUJI REBOOT: houji_reboot_bootloader=1, prepared %d\n",houji_reboot_reason_prepare(1));
        }
    }
    int enabled=0;PE_parse_boot_argn("houji_usb",&enabled,sizeof(enabled));
    if ((enabled!=1 && enabled!=2) || attempted) return;
    PE_parse_boot_argn("houji_usb_fail_panic",&fail_panic,sizeof(fail_panic));
    attempted=true;
    {
        unsigned watchdog=0;thread_t t;
        PE_parse_boot_argn("houji_usb_watchdog_s",&watchdog,sizeof(watchdog));
        if (watchdog>=30 && kernel_thread_start(watchdog_thread,(void *)(uintptr_t)watchdog,&t)==KERN_SUCCESS) thread_deallocate(t);
    }
    u.mps=512;u.coding[0]=0;u.coding[1]=0xc2;u.coding[2]=1;u.coding[6]=8;
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
    int cold=0;PE_parse_boot_argn("houji_usb_cold",&cold,sizeof(cold));
    if (cold==1) {
        /* Test: the state a power-on boot starts from, on a phone booted through fastboot. */
        houji_usb_phy_cold();
        uint32_t g=rd(GCTL);
        if (((g>>12)&3)!=2) wr(GCTL,(g&~(3u<<12))|(2u<<12));
        unote("HOUJI USB: cold test gctl %08x->%08x dctl=%08x dsts=%08x phy2=%08x pipe3=%08x\n",
            g,rd(GCTL),rd(DCTL),rd(DSTS),rd(GUSB2PHYCFG0),rd(GUSB3PIPECTL0));
    }
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
    if (!vusb_dma_allowed(u.pa,DMA_SIZE,width)) { unote("HOUJI USB: owned DMA width/range guard refused pa=%llx width=%u base=%lx size=%lx\n",u.pa,width,gPhysBase,gPhysSize);return; }
    for (unsigned off=0;off<DMA_SIZE;off+=PAGE_SIZE) {
        ppnum_t page=pmap_find_phys(kernel_pmap,(addr64_t)(u.dma+off));
        if (!page || ptoa_64(page)!=u.pa+off) return;
    }
    vm_offset_t qs=ml_io_map(0xa6f8800,0x100);
    if (!qs) return;
    /* First stop any old device DMA; soft reset must finish before replacing
     * descriptors or enabling a previously faulting SMMU stream. */
    reset_state("before halt");
    wr(DEVTEN,0);dctl_write(rd(DCTL)&~RUN);
    unsigned i;
    for (i=0;i<500 && !(rd(DSTS)&HALTED);i++) delay(1000);
    if (!(rd(DSTS)&HALTED)) { fail("controller halt timeout");return; }
    /* The working ABL fastboot handoff has DSBLCLKGTNG=1, unlike its
     * normal-boot shutdown. Keep controller-local clocks ungated through
     * reset and polled gadget operation; do not change GCC or PHY resets. */
    uint32_t gctl=rd(GCTL);
    if (!(gctl&GCTL_DSBLCLKGTNG)) {
        wr(GCTL,gctl|GCTL_DSBLCLKGTNG);
        if (!(rd(GCTL)&GCTL_DSBLCLKGTNG)) { fail("clock gating disable readback");return; }
    }
    unote("HOUJI USB: reset clocks utmi=%08x sleep=%08x master-rcg=%08x utmi-rcg=%08x\n",
        read_reg(gcc,0x28),read_reg(gcc,0x24),read_reg(gcc,0x2c),read_reg(gcc,0x44));
    /* Upstream DWC3 clears GUSB2PHYCFG.SUSPHY before PHY init and before
     * issuing DCTL.CSFTRST. The failed standalone capture had SUSPHY=0,
     * so this is not an established fix for that reset timeout. This is a
     * controller-local, read-back-verified correction: no GCC, SPMI,
     * regulator, block-reset or external-PHY writes. */
    uint32_t phy2=rd(GUSB2PHYCFG0);
    /* Normal boot captured 00102500; working assisted boot 00102400.
     * Disable the UTMI sleep request as well as suspend before reset.
     * This does not initialize an unpowered external PHY. */
    if (phy2&(USB2_SUSPHY|USB2_ENBLSLPM)) {
        wr(GUSB2PHYCFG0,phy2&~(USB2_SUSPHY|USB2_ENBLSLPM));
        uint32_t phy2back=rd(GUSB2PHYCFG0);
        unote("HOUJI USB: clear USB2 suspend/sleep before reset: %08x->%08x\n",
            phy2,phy2back);
        if (phy2back&(USB2_SUSPHY|USB2_ENBLSLPM)) { fail("USB2 sleep clear readback");return; }
        delay(10);
    }
    reset_state("before reset");
    dctl_write((rd(DCTL)&~RUN)|RESET);
    for (i=0;i<500 && (rd(DCTL)&RESET);i++) delay(1000);
    if (rd(DCTL)&RESET) {
        /* Power-on boot: nobody started the PHYs, and the soft reset waits for their clocks. Start the USB 2 PHY the way Linux
         * does, then reset again. After "fastboot boot" the reset completes and none of this runs. */
        reset_state("reset timeout");
        houji_usb_phy_state("before");
        int phy=houji_usb_phy_start();
        for (i=0;i<500 && (rd(DCTL)&RESET);i++) delay(1000);
        houji_usb_phy_state("after");
        unote("HOUJI USB: PHY bring-up rc=%d; first reset %s after %u ms\n",phy,(rd(DCTL)&RESET)?"still pending":"completed",i);
        if (rd(DCTL)&RESET) { reset_state("reset timeout after PHY bring-up");fail("controller reset timeout after PHY bring-up");return; }
        uint32_t g=rd(GCTL);
        if (!(g&GCTL_DSBLCLKGTNG)) wr(GCTL,g|GCTL_DSBLCLKGTNG);
        phy2=rd(GUSB2PHYCFG0);
        if (phy2&(USB2_SUSPHY|USB2_ENBLSLPM)) wr(GUSB2PHYCFG0,phy2&~(USB2_SUSPHY|USB2_ENBLSLPM));
        wr(GUSB3PIPECTL0,rd(GUSB3PIPECTL0)&~(1u<<17));
        dctl_write((rd(DCTL)&~RUN)|RESET);
        for (i=0;i<500 && (rd(DCTL)&RESET);i++) delay(1000);
        reset_state("after PHY bring-up");
        if (rd(DCTL)&RESET) { fail("second controller reset timeout");return; }
    }
    if (!(rd(DSTS)&HALTED) || (rd(DCTL)&RUN)) { fail("controller not halted after reset");return; }
    if (route==2) {
        uint32_t r[3]={0};
        const char *why=houji_smmu_route_stream(0x40,u.pa,DMA_SIZE,r);
        unote("HOUJI USB: SID40 bypass route %s slot=%u bank=%u cbar=%08x sctlr=%08x\n",
            why?why:"ok",r[0]&0xffff,r[0]>>16,r[1],r[2]);
        if (why) return;
    }
    sync_dma(0,DMA_SIZE);
    wr(GEVNTADRLO,(uint32_t)u.pa);wr(GEVNTADRHI,(uint32_t)(u.pa>>32));
    if (rd(GEVNTADRLO)!=(uint32_t)u.pa||rd(GEVNTADRHI)!=(uint32_t)(u.pa>>32)) { fail("event address readback");return; }
    wr(GEVNTSIZ,(1u<<31)|EVENT_SIZE);wr(GEVNTCOUNT,rd(GEVNTCOUNT)&0xffff);
    if ((rd(GEVNTSIZ)&0x1ffffu)!=EVENT_SIZE) { fail("event size readback");return; }
    wr(DCFG,rd(DCFG)&~(7u|(127u<<3))); /* high speed only */
    if (epcmd(0,START_CONFIG,0,0,0) ||
        epcmd(0,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10),0) || epcmd(0,SET_RESOURCE,1,0,0) ||
        epcmd(1,SET_CONFIG,64u<<3,(1u<<8)|(1u<<10)|(1u<<25),0) || epcmd(1,SET_RESOURCE,1,0,0)) return;
    wr(DALEPENA,3);setup_arm();if (u.failed) return;
    uint32_t hs=read_reg(qs,0x10),ss=read_reg(qs,0x30);
    *(volatile uint32_t *)(qs+0x10)=hs|(1u<<20)|(1u<<28);
    *(volatile uint32_t *)(qs+0x30)=ss|(1u<<24);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(DEVTEN,7|(1u<<9)|(1u<<11));
    uint32_t dctl=rd(DCTL);
    if (dctl&RESET) { fail("reset asserted before RUN");wr(DEVTEN,0);return; }
    dctl_write(dctl|RUN);
    for (i=0;i<500 && (rd(DSTS)&HALTED);i++) delay(1000);
    if (rd(DSTS)&HALTED) { fail("controller run timeout");wr(DEVTEN,0);dctl_write(rd(DCTL)&~RUN);return; }
    thread_t thread;
    if (kernel_thread_start(usb_thread,NULL,&thread)!=KERN_SUCCESS) { fail("poll thread creation");wr(DEVTEN,0);dctl_write(rd(DCTL)&~RUN);return; }
    thread_deallocate(thread);
    unote("HOUJI USB: owned RAM base=%lx bytes=%lx; mode=%d\n",gPhysBase,gPhysSize,enabled);
    usb_started=true;
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
/* System sleep (SweetCPU::bootSleep, interrupts masked, no threads): phase 0 before, 1 on every poll, 2 after wake. Register reads
 * only; the poll thread handles the events after wake. */
static uint32_t sleep_events_max,sleep_dsts_first,sleep_dsts_last,sleep_dsts_changes,sleep_failed_before;
void houji_usb_sleep_probe(int phase)
{
    if (!u.regs) return;
    const uint32_t count=rd(GEVNTCOUNT)&0xffff,dsts=rd(DSTS);
    if (phase==0) { sleep_events_max=count;sleep_dsts_first=sleep_dsts_last=dsts;sleep_dsts_changes=0;sleep_failed_before=u.failed;return; }
    if (count>sleep_events_max) sleep_events_max=count;
    if (dsts!=sleep_dsts_last) { sleep_dsts_changes++;sleep_dsts_last=dsts; }
    if (phase==2)
        printf("HOUJI USB SLEEP: event bytes pending max %u (ring %u) now %u; dsts %08x -> %08x, %u changes; failed before %u now %u; configured %u\n",
            sleep_events_max,EVENT_SIZE,count,sleep_dsts_first,dsts,sleep_dsts_changes,sleep_failed_before,u.failed,u.configured);
}
void houji_usb_report(void)
{
    if (!usb_notes_n) { printf("HOUJI USB REPORT: driver never started\n");return; }
    printf("HOUJI USB REPORT BEGIN\n%sHOUJI USB REPORT END\n",usb_notes);
    if (u.regs) printf("HOUJI USB STATE: events=%u setups=%u configured=%u dtr=%u failed=%u tx=%llu rx=%llu dsts=%08x dctl=%08x gevntcount=%08x\n",
        u.events,u.setups,u.configured,u.dtr,u.failed,u.tx_bytes,u.rx_bytes,rd(DSTS),rd(DCTL),rd(GEVNTCOUNT));
}
