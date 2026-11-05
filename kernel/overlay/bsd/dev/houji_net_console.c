// SPDX-License-Identifier: BSD-2-Clause-Patent
/* houji: TCP command channel over the working Wi-Fi (en0) — the same
 * read-only command set as the CDC console, plus a reboot-to-recovery
 * command that arms the no-wipe recovery BCB first. The listener binds
 * once DHCP gives en0 an IPv4 address and answers any LAN peer. It is
 * on whenever the Wi-Fi stack is (houji_wifi), matching the CDC
 * console's trust level. Nothing here writes storage except the single
 * BCB block of the "recovery" command (verified readback, then reboot). */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/kpi_mbuf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/sched_prim.h>
#include <mach/thread_act.h>
#include <machine/atomic.h>

#define HNC_PORT 5555
#define HNC_LINE_MAX 160
#define HNC_OUT_MAX 4096

extern int reboot_kernel(int, char *);
extern unsigned houji_debug_copy(uint64_t *, char *, unsigned);
extern uint64_t houji_debug_total(void);
extern uint64_t houji_debug_dropped(void);
extern void houji_wifi_command(const char *);
extern int houji_ufs_arm_recovery_bcb(void);   /* HoujiUfsRead.cpp */

static int _Atomic hnc_started;
static int _Atomic hnc_busy;
static volatile int hnc_reboot_requested;

/* en0's first IPv4 address, or 0. */
static uint32_t hnc_en0_addr(void)
{
    uint32_t addr_be = 0;
    ifnet_t ifp = NULL;
    if (ifnet_find_by_name("en0", &ifp) != 0 || !ifp) return 0;
    ifaddr_t *list = NULL;
    if (ifnet_get_address_list_family(ifp, &list, AF_INET) == 0 && list) {
        if (list[0]) {
            struct sockaddr_in a;
            bzero(&a, sizeof(a));
            if (ifaddr_address(list[0], (struct sockaddr *)&a, sizeof(a)) == 0)
                addr_be = a.sin_addr.s_addr;
        }
        ifnet_free_address_list(list);
    }
    ifnet_release(ifp);
    return addr_be;
}

/* Answer one command line into out; returns bytes written. Same command
 * set as the CDC console; "replay" streams the retained RAM log ring. */
static unsigned hnc_reply(const char *cmd, char *out, unsigned max)
{
    unsigned n = 0;
    if (!strcmp(cmd, "ping")) {
        n = (unsigned)snprintf(out, max, "HOUJI NET: pong HOUJIDBG001\n");
    } else if (!strcmp(cmd, "status")) {
        uint32_t a = hnc_en0_addr();
        if (a) {
            const uint8_t *b = (const uint8_t *)&a;
            n = (unsigned)snprintf(out, max, "HOUJI NET: en0 %u.%u.%u.%u log=%llu dropped=%llu\n",
                b[0], b[1], b[2], b[3],
                (unsigned long long)houji_debug_total(),
                (unsigned long long)houji_debug_dropped());
        } else {
            n = (unsigned)snprintf(out, max, "HOUJI NET: en0 has no IPv4 address\n");
        }
    } else if (!strcmp(cmd, "replay")) {
        uint64_t pos = 0;
        n = houji_debug_copy(&pos, out, max - 64);   /* room for the notice */
        if (pos < houji_debug_total())
            n += (unsigned)snprintf(out + n, max - n,
                "\nHOUJI NET: replay truncated at %llu of %llu\n",
                (unsigned long long)pos, (unsigned long long)houji_debug_total());
    } else if (!strncmp(cmd, "wifi ", 5)) {
        houji_wifi_command(cmd + 5);
        n = (unsigned)snprintf(out, max, "HOUJI WIFI: command queued; see console log\n");
    } else if (!strcmp(cmd, "recovery")) {
        int r = houji_ufs_arm_recovery_bcb();
        if (r == 0) {
            n = (unsigned)snprintf(out, max, "HOUJI NET: recovery BCB armed; rebooting\n");
            hnc_reboot_requested = 1;   /* serve() sends the reply, then reboots */
        } else {
            n = (unsigned)snprintf(out, max, "HOUJI NET: recovery BCB arm FAILED: %d\n", r);
        }
    } else {
        n = (unsigned)snprintf(out, max,
            "HOUJI NET: commands ping status replay 'wifi scan|join|leave|status' recovery\n");
    }
    return n >= max ? max - 1 : n;
}

/* One connection: read lines, answer each, close on EOF or error.
 * Blocking receive keeps the poller idle unless a peer is connected. */
static void hnc_serve(socket_t so)
{
    char line[HNC_LINE_MAX];
    char *out = (char *)kalloc_data(HNC_OUT_MAX, Z_WAITOK | Z_ZERO);
    unsigned fill = 0;
    if (!out) { sock_close(so); return; }
    for (;;) {
        struct msghdr msg;
        bzero(&msg, sizeof(msg));
        mbuf_t m = NULL;
        size_t got = 65536;   /* max receive; sock_receivembuf requires nonzero */
        errno_t e = sock_receivembuf(so, &msg, &m, 0, &got);   /* blocking */
        if (e || !m) break;   /* EOF or error */
        for (mbuf_t c = m; c; c = mbuf_next(c)) {
            const char *p = (const char *)mbuf_data(c);
            size_t len = mbuf_len(c);
            for (size_t i = 0; i < len; i++) {
                char ch = p[i];
                if (ch == '\n' || ch == '\r') {
                    if (fill == 0) continue;
                    line[fill] = 0;
                    if (strcmp(line, "recovery") == 0)
                        printf("HOUJI NET: recovery command received\n");
                    unsigned n = hnc_reply(line, out, HNC_OUT_MAX);
                    fill = 0;
                    if (n) {
                        mbuf_t om = NULL;
                        if (mbuf_gethdr(MBUF_WAITOK, MBUF_TYPE_DATA, &om) == 0 &&
                            mbuf_copyback(om, 0, n, out, MBUF_WAITOK) == 0) {
                            size_t sent = 0;
                            (void)sock_sendmbuf(so, NULL, om, 0, &sent);
                            /* sock_sendmbuf frees the chain either way */
                        }
                    }
                    if (hnc_reboot_requested) {
                        /* Reply is queued; close and reboot from a fresh
                         * thread so the socket teardown completes. */
                        kfree_data(out, HNC_OUT_MAX);
                        sock_close(so);
                        printf("HOUJI NET: recovery requested over Wi-Fi; BCB verified; rebooting\n");
                        reboot_kernel(RB_AUTOBOOT, (char *)"houji net recovery reboot");
                        thread_terminate(current_thread());
                    }
                } else if (fill < HNC_LINE_MAX - 1) {
                    line[fill++] = ch;
                } else {
                    fill = 0;   /* overflow: drop the line */
                }
            }
        }
        mbuf_freem(m);
    }
    kfree_data(out, HNC_OUT_MAX);
    sock_close(so);
}

/* Wait for en0 to have an IPv4 address, then bind and serve forever. */
static void hnc_thread(void *arg __unused, wait_result_t result __unused)
{
    uint32_t a = 0;
    for (;;) {
        a = hnc_en0_addr();
        if (a) break;
        assert_wait_timeout((event_t)&hnc_thread, THREAD_UNINT, 1000, NSEC_PER_MSEC);
        thread_block(THREAD_CONTINUE_NULL);
    }
    const uint8_t *b = (const uint8_t *)&a;
    printf("HOUJI NET: en0 %u.%u.%u.%u; binding TCP %u\n", b[0], b[1], b[2], b[3], HNC_PORT);
    socket_t lso = NULL;
    errno_t e = sock_socket(AF_INET, SOCK_STREAM, 0, NULL, NULL, &lso);
    if (e) { printf("HOUJI NET: socket error %d\n", e); return; }
    struct sockaddr_in sin;
    bzero(&sin, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(HNC_PORT);
    sin.sin_addr.s_addr = INADDR_ANY;
    e = sock_bind(lso, (struct sockaddr *)&sin);
    if (e) { printf("HOUJI NET: bind error %d\n", e); sock_close(lso); return; }
    e = sock_listen(lso, 1);
    if (e) { printf("HOUJI NET: listen error %d\n", e); sock_close(lso); return; }
    for (;;) {
        socket_t so = NULL;
        e = sock_accept(lso, NULL, 0, 0, NULL, NULL, &so);   /* blocking */
        if (e || !so) { printf("HOUJI NET: accept error %d\n", e); break; }
        hnc_serve(so);
    }
    sock_close(lso);
}

/* Started by the Wi-Fi module once the stack is up; also callable from
 * the CDC console. Single instance, no allocation after thread start. */
void houji_net_console_start(void)
{
    if (os_atomic_load(&hnc_started, acquire)) return;
    if (!os_atomic_cmpxchg(&hnc_busy, 0, 1, acquire)) return;
    os_atomic_store(&hnc_started, 1, release);
    thread_t t;
    if (kernel_thread_start(hnc_thread, NULL, &t) != KERN_SUCCESS) {
        printf("HOUJI NET: thread start failed\n");
        os_atomic_store(&hnc_started, 0, release);
    } else {
        thread_deallocate(t);
    }
    os_atomic_store(&hnc_busy, 0, release);
}
