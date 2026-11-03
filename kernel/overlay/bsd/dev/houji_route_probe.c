// SPDX-License-Identifier: BSD-2-Clause-Patent
/* houji: what the IPv4 routing table says about an address, for the Wi-Fi log: the interface, the gateway and the route's flags, unscoped
 * (what an ordinary socket gets) or scoped to an interface index. Read only. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/route_private.h>
#include <netinet/in.h>

void houji_route_describe(uint32_t addr_be, unsigned int ifscope, char *out, size_t size);
void
houji_route_describe(uint32_t addr_be, unsigned int ifscope, char *out, size_t size)
{
	struct sockaddr_in dst;
	bzero(&dst, sizeof(dst));
	dst.sin_len = sizeof(dst);
	dst.sin_family = AF_INET;
	dst.sin_addr.s_addr = addr_be;
	struct rtentry *rt = rtalloc1_scoped((struct sockaddr *)&dst, 0, 0, ifscope ? ifscope : IFSCOPE_NONE);
	if (rt == NULL) {
		snprintf(out, size, "no route");
		return;
	}
	char gw[24] = "-";
	if (rt->rt_gateway && rt->rt_gateway->sa_family == AF_INET) {
		const uint8_t *g = (const uint8_t *)&((struct sockaddr_in *)(void *)rt->rt_gateway)->sin_addr;
		snprintf(gw, sizeof(gw), "%u.%u.%u.%u", g[0], g[1], g[2], g[3]);
	} else if (rt->rt_gateway) {
		snprintf(gw, sizeof(gw), "af%u", rt->rt_gateway->sa_family);
	}
	snprintf(out, size, "if %s%d gw %s flags %x", rt->rt_ifp ? rt->rt_ifp->if_name : "?", rt->rt_ifp ? rt->rt_ifp->if_unit : -1, gw, rt->rt_flags);
	rtfree(rt);
}
