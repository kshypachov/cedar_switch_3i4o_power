// diag_report.c — safe build across Zephyr versions: memory + sockets only

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_stats.h>

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>
#include "../zbus_topics.h"
#include "diag_report.h"


LOG_MODULE_REGISTER(net_diag, LOG_LEVEL_INF);

static struct k_work_delayable stats_timer;

#define CONFIG_SAMPLE_PERIOD 30

#if defined(CONFIG_NET_STATISTICS_PER_INTERFACE)
#define GET_STAT(iface, s) (iface ? iface->stats.s : data->s)
#else
//#define GET_STAT(iface, s) data->s
#endif


#if defined(CONFIG_MEM_POOL_HEAP) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
#include <zephyr/sys/mem_stats.h>
#endif

/* ===== Thread ===== */
#ifndef NET_DIAG_STACK
#define NET_DIAG_STACK 2048
#endif
#ifndef NET_DIAG_PRIO
#define NET_DIAG_PRIO 9
#endif

K_THREAD_STACK_DEFINE(net_diag_stack, NET_DIAG_STACK);
K_THREAD_STACK_DEFINE(outputs_send_stack, NET_DIAG_STACK);
static struct k_thread net_diag_thread;
static struct k_thread outputs_send_thread;

static struct {
	k_timeout_t interval;
	volatile bool run;
} g_diag = { .interval = K_SECONDS(5), .run = false };

/* ===== Helpers ===== */

// static void print_stats(struct net_if *iface, struct net_stats *data)
// {
// 	if (iface) {
// 		LOG_INF("Statistics for interface %p [%d]\n", iface,
// 		       net_if_get_by_iface(iface));
// 	} else {
// 		LOG_INF("Global network statistics\n");
// 	}
//
// #if defined(CONFIG_NET_IPV6)
// 	LOG_INF("IPv6 recv      %u\tsent\t%u\tdrop\t%u\tforwarded\t%u\n",
// 	       GET_STAT(iface, ipv6.recv),
// 	       GET_STAT(iface, ipv6.sent),
// 	       GET_STAT(iface, ipv6.drop),
// 	       GET_STAT(iface, ipv6.forwarded));
// #if defined(CONFIG_NET_IPV6_ND)
// 	LOG_INF("IPv6 ND recv   %u\tsent\t%u\tdrop\t%u\n",
// 	       GET_STAT(iface, ipv6_nd.recv),
// 	       GET_STAT(iface, ipv6_nd.sent),
// 	       GET_STAT(iface, ipv6_nd.drop));
// #endif /* CONFIG_NET_IPV6_ND */
// #if defined(CONFIG_NET_IPV6_PMTU)
// 	LOG_INF("IPv6 PMTU recv %u\tsent\t%u\tdrop\t%u\n",
// 	       GET_STAT(iface, ipv6_pmtu.recv),
// 	       GET_STAT(iface, ipv6_pmtu.sent),
// 	       GET_STAT(iface, ipv6_pmtu.drop));
// #endif /* CONFIG_NET_IPV6_PMTU */
// #if defined(CONFIG_NET_STATISTICS_MLD)
// 	LOG_INF("IPv6 MLD recv  %u\tsent\t%u\tdrop\t%u\n",
// 	       GET_STAT(iface, ipv6_mld.recv),
// 	       GET_STAT(iface, ipv6_mld.sent),
// 	       GET_STAT(iface, ipv6_mld.drop));
// #endif /* CONFIG_NET_STATISTICS_MLD */
// #endif /* CONFIG_NET_IPV6 */
//
// #if defined(CONFIG_NET_IPV4)
// 	LOG_INF("IPv4 recv      %u\tsent\t%u\tdrop\t%u\tforwarded\t%u\n",
// 	       GET_STAT(iface, ipv4.recv),
// 	       GET_STAT(iface, ipv4.sent),
// 	       GET_STAT(iface, ipv4.drop),
// 	       GET_STAT(iface, ipv4.forwarded));
// #endif /* CONFIG_NET_IPV4 */
//
// 	LOG_INF("IP vhlerr      %u\thblener\t%u\tlblener\t%u\n",
// 	       GET_STAT(iface, ip_errors.vhlerr),
// 	       GET_STAT(iface, ip_errors.hblenerr),
// 	       GET_STAT(iface, ip_errors.lblenerr));
// 	LOG_INF("IP fragerr     %u\tchkerr\t%u\tprotoer\t%u\n",
// 	       GET_STAT(iface, ip_errors.fragerr),
// 	       GET_STAT(iface, ip_errors.chkerr),
// 	       GET_STAT(iface, ip_errors.protoerr));
//
// #if defined(CONFIG_NET_IPV4_PMTU)
// 	LOG_INF("IPv4 PMTU recv %u\tsent\t%u\tdrop\t%u\n",
// 	       GET_STAT(iface, ipv4_pmtu.recv),
// 	       GET_STAT(iface, ipv4_pmtu.sent),
// 	       GET_STAT(iface, ipv4_pmtu.drop));
// #endif /* CONFIG_NET_IPV4_PMTU */
//
// 	LOG_INF("ICMP recv      %u\tsent\t%u\tdrop\t%u\n",
// 	       GET_STAT(iface, icmp.recv),
// 	       GET_STAT(iface, icmp.sent),
// 	       GET_STAT(iface, icmp.drop));
// 	LOG_INF("ICMP typeer    %u\tchkerr\t%u\n",
// 	       GET_STAT(iface, icmp.typeerr),
// 	       GET_STAT(iface, icmp.chkerr));
//
// #if defined(CONFIG_NET_UDP)
// 	LOG_INF("UDP recv       %u\tsent\t%u\tdrop\t%u\n",
// 	       GET_STAT(iface, udp.recv),
// 	       GET_STAT(iface, udp.sent),
// 	       GET_STAT(iface, udp.drop));
// 	LOG_INF("UDP chkerr     %u\n",
// 	       GET_STAT(iface, udp.chkerr));
// #endif
//
// #if defined(CONFIG_NET_STATISTICS_TCP)
// 	LOG_INF("TCP bytes recv %llu\tsent\t%llu\n",
// 	       GET_STAT(iface, tcp.bytes.received),
// 	       GET_STAT(iface, tcp.bytes.sent));
// 	LOG_INF("TCP seg recv   %u\tsent\t%u\tdrop\t%u\n",
// 	       GET_STAT(iface, tcp.recv),
// 	       GET_STAT(iface, tcp.sent),
// 	       GET_STAT(iface, tcp.drop));
// 	LOG_INF("TCP seg resent %u\tchkerr\t%u\tackerr\t%u\n",
// 	       GET_STAT(iface, tcp.resent),
// 	       GET_STAT(iface, tcp.chkerr),
// 	       GET_STAT(iface, tcp.ackerr));
// 	LOG_INF("TCP seg rsterr %u\trst\t%u\tre-xmit\t%u\n",
// 	       GET_STAT(iface, tcp.rsterr),
// 	       GET_STAT(iface, tcp.rst),
// 	       GET_STAT(iface, tcp.rexmit));
// 	LOG_INF("TCP conn drop  %u\tconnrst\t%u\n",
// 	       GET_STAT(iface, tcp.conndrop),
// 	       GET_STAT(iface, tcp.connrst));
// #endif
//
// 	LOG_INF("Bytes received %llu\n", GET_STAT(iface, bytes.received));
// 	LOG_INF("Bytes sent     %llu\n", GET_STAT(iface, bytes.sent));
// 	LOG_INF("Processing err %u\n", GET_STAT(iface, processing_error));
// }

#if defined(CONFIG_NET_STATISTICS_PER_INTERFACE)
static void iface_cb(struct net_if *iface, void *user_data)
{
	struct net_stats *data = user_data;

	net_mgmt(NET_REQUEST_STATS_GET_ALL, iface, data, sizeof(*data));

	print_stats(iface, data);
}
#endif

#if defined(CONFIG_NET_STATISTICS_ETHERNET)
static void print_eth_stats(struct net_if *iface, struct net_stats_eth *data)
{
	LOG_INF("Statistics for Ethernet interface %p [%d]\n", iface,
		   net_if_get_by_iface(iface));

	LOG_INF("Bytes received   : %llu\n", data->bytes.received);
	LOG_INF("Bytes sent       : %llu\n", data->bytes.sent);
	LOG_INF("Packets received : %u\n", data->pkts.rx);
	LOG_INF("Packets sent     : %u\n", data->pkts.tx);
	LOG_INF("Bcast received   : %u\n", data->broadcast.rx);
	LOG_INF("Bcast sent       : %u\n", data->broadcast.tx);
	LOG_INF("Mcast received   : %u\n", data->multicast.rx);
	LOG_INF("Mcast sent       : %u\n", data->multicast.tx);
}

static void eth_iface_cb(struct net_if *iface, void *user_data)
{
	struct net_stats_eth eth_data;
	int ret;

	if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET)) {
		return;
	}

	ret = net_mgmt(NET_REQUEST_STATS_GET_ETHERNET, iface, &eth_data,
			   sizeof(eth_data));
	if (ret < 0) {
		return;
	}

	print_eth_stats(iface, &eth_data);
}
#endif

// static void stats(struct k_work *work)
// {
// 	struct net_stats data;
//
// 	/* It is also possible to query some specific statistics by setting
// 	 * the first request parameter properly. See include/net/net_stats.h
// 	 * what requests are available.
// 	 */
// 	net_mgmt(NET_REQUEST_STATS_GET_ALL, NULL, &data, sizeof(data));
//
// 	print_stats(NULL, &data);
//
// #if defined(CONFIG_NET_STATISTICS_PER_INTERFACE)
// 	net_if_foreach(iface_cb, &data);
// #endif
//
// #if defined(CONFIG_NET_STATISTICS_ETHERNET)
// 	net_if_foreach(eth_iface_cb, &data);
// #endif
//
// 	k_work_reschedule(&stats_timer, K_SECONDS(CONFIG_SAMPLE_PERIOD));
// }


static void diag_mem(void)
{
#if defined(CONFIG_MEM_POOL_HEAP) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats st;
	if (sys_heap_runtime_stats_get(k_heap_system_heap(), &st) == 0) {
		LOG_INF("DIAG MEM free=%u alloc=%u max_alloc=%u",
			(unsigned)st.free_bytes,
			(unsigned)st.allocated_bytes,
			(unsigned)st.max_allocated_bytes);
	} else {
		LOG_INF("DIAG MEM stats_unavail");
	}
#elif defined(CONFIG_HEAP_MEM_POOL_SIZE)
	LOG_INF("DIAG MEM pool_size=%u (compile-time only)",
		(unsigned)CONFIG_HEAP_MEM_POOL_SIZE);
#else
	LOG_INF("DIAG MEM disabled");
#endif
}

static void diag_iface_brief(void)
{
	struct net_if *iface = net_if_get_default();
	if (!iface) {
		LOG_INF("DIAG IF none");
		return;
	}

	bool up = net_if_is_up(iface);
	uint16_t mtu = net_if_get_mtu(iface);
	LOG_INF("DIAG IF up=%d mtu=%u", up, mtu);
}

static void diag_sockets(void)
{
#if defined(CONFIG_NET_SOCKETS)
	int max =
#ifdef CONFIG_NET_SOCKETS_MAX
		CONFIG_NET_SOCKETS_MAX;
#else
		-1;
#endif
	int poll_max =
#ifdef CONFIG_NET_SOCKETS_POLL_MAX
		CONFIG_NET_SOCKETS_POLL_MAX;
#else
		-1;
#endif
	/* Zephyr пока не даёт публичного «used sockets», поэтому печатаем лимиты. */
	LOG_INF("DIAG SOCK max=%d poll_max=%d", max, poll_max);
#else
	LOG_INF("DIAG SOCK disabled");
#endif
}



// static void print_stats_for_iface(struct net_if *iface)
// {
// 	const struct net_stats *s = net_if_get_stats(iface);
// 	if (!s) {
// 		LOG_INF("Stats unavailable for iface %p [%d]", iface, net_if_get_by_iface(iface));
// 		return;
// 	}
//
// 	LOG_INF("Statistics for interface %p [%d]", iface, net_if_get_by_iface(iface));
//
// #if defined(CONFIG_NET_IPV4)
// 	LOG_INF("IPv4 recv %u sent %u drop %u fwd %u",
// 			s->ipv4.recv, s->ipv4.sent, s->ipv4.drop, s->ipv4.forwarded);
// #endif
//
// 	LOG_INF("IP vhlerr %u hblener %u lblener %u",
// 			s->ip_errors.vhlerr, s->ip_errors.hblenerr, s->ip_errors.lblenerr);
// 	LOG_INF("IP fragerr %u chkerr %u protoer %u",
// 			s->ip_errors.fragerr, s->ip_errors.chkerr, s->ip_errors.protoerr);
//
// #if defined(CONFIG_NET_UDP)
// 	LOG_INF("UDP recv %u sent %u drop %u chkerr %u",
// 			s->udp.recv, s->udp.sent, s->udp.drop, s->udp.chkerr);
// #endif
//
// #if defined(CONFIG_NET_STATISTICS_TCP)
// 	LOG_INF("TCP bytes recv %" PRIu64 " sent %" PRIu64,
// 			s->tcp.bytes.received, s->tcp.bytes.sent);
// 	LOG_INF("TCP seg recv %u sent %u drop %u",
// 			s->tcp.recv, s->tcp.sent, s->tcp.drop);
// 	LOG_INF("TCP seg resent %u chkerr %u ackerr %u",
// 			s->tcp.resent, s->tcp.chkerr, s->tcp.ackerr);
// 	LOG_INF("TCP seg rsterr %u rst %u re-xmit %u",
// 			s->tcp.rsterr, s->tcp.rst, s->tcp.rexmit);
// 	LOG_INF("TCP conn drop %u connrst %u",
// 			s->tcp.conndrop, s->tcp.connrst);
// #endif
//
// 	LOG_INF("Bytes received %" PRIu64, s->bytes.received);
// 	LOG_INF("Bytes sent     %" PRIu64, s->bytes.sent);
// 	LOG_INF("Processing err %u", s->processing_error);
// }


/* Аккумулятор для суммирования по всем интерфейсам */
struct totals {
	uint64_t rx;
	uint64_t tx;
};

/* Коллбек для net_if_foreach */
// static void sum_and_print_cb(struct net_if *iface, void *user_data)
// {
// 	struct totals *t = (struct totals *)user_data;
// 	struct net_stats stats;
// 	int ret;
//
// 	memset(&stats, 0, sizeof(stats));
//
// 	ret = net_mgmt(NET_REQUEST_STATS_GET_ALL,
// 				   iface, &stats, sizeof(stats));
// 	if (ret < 0) {
// 		LOG_WRN("Cannot get stats for iface %p (%d)", iface, ret);
// 		return;
// 	}
//
// 	print_stats(iface, &stats);
//
// 	t->rx += stats.bytes.received;
// 	t->tx += stats.bytes.sent;
// }

// static void stats_work(struct k_work *work)
// {
// 	ARG_UNUSED(work);
//
// 	struct totals t = {0, 0};
//
// 	net_if_foreach(sum_and_print_cb, &t);
//
// 	LOG_INF("Total bytes rx=%" PRIu64 " tx=%" PRIu64, t.rx, t.tx);
//
// 	k_work_reschedule(&stats_timer, K_SECONDS(CONFIG_SAMPLE_PERIOD));
// }

/* ===== Worker thread ===== */

static void net_diag_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (g_diag.run) {
		int64_t up = k_uptime_get();
		LOG_INF("DIAG TICK up_ms=%lld", (long long)up);

		diag_iface_brief();
		diag_mem();
		diag_sockets();

		k_sleep(g_diag.interval);
	}
}

/* ===== API ===== */

// static void init_app(void)
// {
// 	k_work_init_delayable(&stats_timer, stats);
// 	k_work_reschedule(&stats_timer, K_SECONDS(CONFIG_SAMPLE_PERIOD));
// }

// void net_diag_start_periodic(k_timeout_t interval)
// {
//
// 	init_app();
// 	stats_work(NULL);
//
//
// 	if (g_diag.run) {
// 		return;
// 	}
// 	g_diag.interval = interval;
// 	g_diag.run = true;
//
// 	k_tid_t tid = k_thread_create(
// 		&net_diag_thread,
// 		net_diag_stack,
// 		K_THREAD_STACK_SIZEOF(net_diag_stack),
// 		net_diag_thread_fn,
// 		NULL, NULL, NULL,
// 		NET_DIAG_PRIO,
// 		0,
// 		K_NO_WAIT);
// 	k_thread_name_set(tid, "net_diag");
// }

void net_diag_stop(void)
{
	g_diag.run = false;
}



static void send_data_to_outputs_pin_fn(void * a, void * b, void * c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	io_event_data_t evt = {
		.event_direction = TO_INTERFACE,
		.io_event = OUTPUT_1_CONTROL,
		.bool_data = true
	};

	while (1) {
		int ret = zbus_chan_pub(&io_events, &evt, K_NO_WAIT);
		if (ret != 0) {
			LOG_ERR("Failed to publish zbus event: %d", ret);
		} else {
			LOG_INF("Published io_event: %d, bool: %d", evt.io_event, evt.bool_data);
		}

		evt.bool_data = !evt.bool_data;
		k_sleep(K_SECONDS(2));
	}
}

void start_send_data_to_outputs_pin(void) {
	k_tid_t tid = k_thread_create(
		&outputs_send_thread,
		outputs_send_stack,
		K_THREAD_STACK_SIZEOF(outputs_send_stack),
		send_data_to_outputs_pin_fn,
		NULL, NULL, NULL,
		NET_DIAG_PRIO,
		0,
		K_NO_WAIT);

	k_thread_name_set(tid, "outputs_send_thread");
}