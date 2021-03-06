/*		-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <sys/param.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>
#include <rte_lpm.h>
#include <rte_lpm6.h>

#include <rte_ip_frag.h>

#define FRAG
#define IPV4_MTU_DEFAULT		ETHER_MTU

#define MAX_PKT_BURST 32

#define RTE_LOGTYPE_IP_RSMBL RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_IP_FRAG RTE_LOGTYPE_USER2

#define	BUF_SIZE	RTE_MBUF_DEFAULT_DATAROOM
#define MBUF_SIZE	(BUF_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)

#define NB_MBUF 	40100

/* allow max jumbo frame 9.5 KB */
#define JUMBO_FRAME_MAX_SIZE	0x2600

#define	MAX_FLOW_NUM	UINT16_MAX
#define	MIN_FLOW_NUM	1
#define	DEF_FLOW_NUM	0x1000

/* TTL numbers are in ms. */
#define	MAX_FLOW_TTL	(3600 * MS_PER_S)
#define	MIN_FLOW_TTL	1
#define	DEF_FLOW_TTL	MS_PER_S			/* timeout in 1 sec */

#define MAX_FRAG_NUM RTE_LIBRTE_IP_FRAG_MAX_FRAG

/* Should be power of two. */
#define	IP_FRAG_TBL_BUCKET_ENTRIES	16

/* Configure how many packets ahead to prefetch, when reading packets */
#define PREFETCH_OFFSET	3

static struct app_config_t {
	uint32_t max_flow_num;
	uint32_t max_flow_ttl;
	uint32_t tx_pps;
	uint32_t display_pps;
	uint32_t dump:1,
			 stat:1,
			 gc:1,	/* garbage collection */
			 reserved:29;
	uint32_t error;	/* error case, 1: missing last fragment */
	uint32_t mtu;
	uint32_t frags;
	uint32_t log_level;
	uint64_t count;
	uint64_t enq_fail;
} app_config = {
	.max_flow_num = DEF_FLOW_NUM,
	.max_flow_ttl = DEF_FLOW_TTL,
	.tx_pps = 1,
	.display_pps = 1,
	.count = 1,
	.enq_fail = 0,
	.log_level = RTE_LOG_INFO,
	.mtu = IPV4_MTU_DEFAULT,
	.error = 0,
	.dump = 0,
	.stat = 0,
	.gc = 0,
};

struct mbuf_table {
	uint32_t len;
	uint32_t head;
	uint32_t tail;
	struct rte_mbuf *m_table[0];
};

/* reassembly */
struct rte_mempool *pool;
struct rte_ring *ring;
#define RING_NAME		"RING"

/* fragmentation */
struct rte_mempool *direct_pool;
struct rte_mempool *indirect_pool;
#define DIR_MP_NAME		"DIR_MP"
#define INDIR_MP_NAME	"INDIR_MP"

struct lcore_queue_conf {
	struct rte_ip_frag_tbl *frag_tbl;
	struct rte_ip_frag_death_row death_row;
} __rte_cache_aligned;
static struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode        = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = JUMBO_FRAME_MAX_SIZE,
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 1, /**< IP checksum offload enabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 1, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.rx_adv_conf = {
			.rss_conf = {
				.rss_key = NULL,
				.rss_hf = ETH_RSS_IP,
		},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};


#ifdef RTE_LIBRTE_IP_FRAG_TBL_STAT
#define TX_LCORE_STAT_UPDATE(s, f, v)   ((s)->f += (v))
#else
#define TX_LCORE_STAT_UPDATE(s, f, v)   do {} while (0)
#endif /* RTE_LIBRTE_IP_FRAG_TBL_STAT */


static inline struct rte_mbuf *
reassemble(struct rte_mbuf *m, uint8_t portid, uint32_t queue,
	struct lcore_queue_conf *qconf, uint64_t tms)
{
	struct rte_ip_frag_tbl *tbl;
	struct rte_ip_frag_death_row *dr;

	(void)portid;
	(void)queue;

	/* if packet is IPv4 */
	if (1 /*m->ol_flags & (PKT_RX_IPV4_HDR) */) {
		struct ipv4_hdr *ip_hdr;

		ip_hdr = rte_pktmbuf_mtod(m, struct ipv4_hdr *);

		 /* if it is a fragmented packet, then try to reassemble. */
		if (rte_ipv4_frag_pkt_is_fragmented(ip_hdr)) {
			struct rte_mbuf *mo;

			tbl = qconf->frag_tbl;
			dr = &qconf->death_row;

			/* prepare mbuf: setup l2_len/l3_len. */
			m->l2_len = 0;
			m->l3_len = sizeof(*ip_hdr);

			/* process this fragment. */
			mo = rte_ipv4_frag_reassemble_packet(tbl, dr, m, tms, ip_hdr);
			if (mo == NULL)
				/* no packet to send out. */
				return NULL;

			/* we have our packet reassembled. */
			if (mo != m) {
				m = mo;
				ip_hdr = rte_pktmbuf_mtod(m, struct ipv4_hdr *);
			}

			RTE_LOG(INFO, IP_RSMBL, "[%p] reassembled. offset %d t_length %u\n", 
					m, 
					ip_hdr->fragment_offset,
					rte_be_to_cpu_16(ip_hdr->total_length));
			return m;
		}
	}
}

static inline struct rte_mbuf *build_pkt(void)
{
	static uint64_t tx_count = 0;
	static uint16_t packet_id = 0;
	uint32_t frag_size;
   
	struct rte_mbuf *m;
	struct ipv4_hdr *ip;
   
#ifdef FRAG
	frag_size = 1510;
#else
	frag_size = 1480;	/* should be multile of 8 */
#endif
	m = rte_pktmbuf_alloc(pool);
	if (m == NULL)
		return NULL;

	ip = rte_pktmbuf_mtod(m, struct ipv4_hdr *);
	ip->dst_addr = 0x01020304;
	ip->src_addr = 0x02030405;

#ifdef FRAG
	packet_id = rte_rand() & 0xFFFF;
	ip->fragment_offset = 0;
	ip->total_length = rte_cpu_to_be_16(frag_size);
	m->pkt_len = m->data_len = frag_size;
#else
	if ((tx_count % 2) == 0) {
		packet_id = rte_rand() & 0xFFFF;
		ip->fragment_offset = rte_cpu_to_be_16(IPV4_HDR_MF_FLAG);
		ip->total_length = rte_cpu_to_be_16(20 + frag_size);
		m->pkt_len = m->data_len = 20 + frag_size;
	} else {
		ip->fragment_offset = rte_cpu_to_be_16(frag_size/8);
		ip->total_length = rte_cpu_to_be_16(20 + 10);
		m->pkt_len = m->data_len = 20 + 10;
	}
#endif

	ip->packet_id = rte_cpu_to_be_16(packet_id);

	RTE_LOG(DEBUG, IP_RSMBL, "%10ju id(N) %5u\n", tx_count, ip->packet_id);

	m->l2_len = 0;
	m->l3_len = sizeof(struct ipv4_hdr);
	tx_count++;

	return m;
}

#define INTERVAL_US	10		/* 10us per packet -> 100,000 pps*/
static int
producer(void)
{
	uint64_t diff_tsc;
	uint64_t cur_tsc;
	uint64_t prev_tsc;
	uint64_t interval_tsc;
	struct rte_mbuf *m = NULL;
   
	interval_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * (1000000/app_config.tx_pps);
	prev_tsc = 0;

	while (1) {
		m = NULL;

		cur_tsc = rte_rdtsc();
		diff_tsc = cur_tsc - prev_tsc;

		if (diff_tsc > interval_tsc)
		{
			prev_tsc = cur_tsc;

			m = build_pkt();

			if (unlikely(m != NULL)) {
				if (rte_ring_enqueue(ring, m) < 0) {
					rte_pktmbuf_free(m);
					app_config.enq_fail += 1;
				}
			} else {
				RTE_LOG(ERR, IP_RSMBL, "mbuf alloc fail\n");
			}
		}
	}
}

/**
 * print LRU entries
 * 
 * @param tbl
 */
static void rte_print_lru(struct rte_ip_frag_tbl *tbl)
{
	struct ip_frag_pkt *fp;
	uint64_t max_cycles;
	uint64_t cur_tsc;
	uint32_t expired = 0;
	struct ip_pkt_list *lru;
	uint32_t i;
	uint32_t count = 0;

	max_cycles = tbl->max_cycles;
	cur_tsc = rte_rdtsc();

	lru = &tbl->lru;

	RTE_LOG(INFO, IP_RSMBL, "----------------------------------------\n");
	RTE_LOG(INFO, IP_RSMBL, "Print LRU list\n");
	TAILQ_FOREACH(fp, lru, lru) {
		if ((max_cycles + fp->start) < cur_tsc) {
			expired = 1;
		} else {
			expired = 0;
		}

		/* Note. Assume that the first fragment is received */
		RTE_LOG(INFO, IP_RSMBL, "[%4u] lru %p mbuf[1] %p id(N) %5u last_idx %u Elapsed:%16ju(%s)\n", 
				count, fp, fp->frags[1].mb, fp->key.id, fp->last_idx, 
				cur_tsc - fp->start, expired == 1 ? "expired" : "");
		for (i = 0 ; i < IP_MAX_FRAG_NUM; i++) {
			RTE_LOG(INFO, IP_RSMBL, "\t[%u] %p\n", i, fp->frags[i].mb);
		}

		if (app_config.dump)
			rte_pktmbuf_dump(stdout, fp->frags[1].mb, 20);

		count++;
	}
	RTE_LOG(INFO, IP_RSMBL, "----------------------------------------\n");
}


inline static void print_mempool_status(void)
{
	RTE_LOG(NOTICE, IP_RSMBL, ">>>>> mbuf count %u %u %u\n", 
			rte_mempool_count(pool),
			rte_mempool_count(direct_pool),
			rte_mempool_count(indirect_pool));
	RTE_LOG(NOTICE, IP_RSMBL, ">>>>> free mbuf count %u %u %u\n", 
			rte_mempool_free_count(pool),
			rte_mempool_free_count(direct_pool),
			rte_mempool_free_count(indirect_pool));
}

#define REPORT_INTERVAL_US	1000000
static int
consumer(void)
{
	unsigned lcore_id;
	uint64_t diff_tsc;
	uint64_t cur_tsc;
	uint64_t prev_print_tsc;
	uint64_t prev_tsc;

	struct lcore_queue_conf *qconf;
	const uint64_t interval_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S
		* (1000000/app_config.tx_pps);
	const uint64_t display_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S
		* (1000000) * app_config.display_pps;
	uint64_t count = 0;					/* number of packet processed */
	uint64_t reasm_count = 0;			/* reassembled count */

	uint64_t last_count = 0;
	uint64_t last_reasm = 0;

	prev_tsc = 0;
	prev_print_tsc = 0;

	lcore_id = rte_lcore_id();

	qconf = &lcore_queue_conf[lcore_id];

	RTE_LOG(INFO, IP_RSMBL, "entering main loop on lcore %u\n", lcore_id);
	RTE_LOG(INFO, IP_RSMBL, "process %ju packets\n", app_config.count);

	while (count < app_config.count) {
		struct rte_mbuf *m = NULL;
		struct ipv4_hdr *ip;

		cur_tsc = rte_rdtsc();

#if 0
		/* FIXME */
		nb_rx = 0;
		nb_rx = rte_ring_dequeue(ring, (void **)&m);
		if (nb_rx < 0) {
			continue;
		}
#else
		diff_tsc = cur_tsc - prev_tsc;

		if (diff_tsc > interval_tsc) {
			prev_tsc = cur_tsc;
			m = build_pkt();
			if (unlikely(m == NULL)) {
				rte_panic("mbuf alloc fail\n");
			}
#endif

			ip = rte_pktmbuf_mtod(m, struct ipv4_hdr *);

			RTE_LOG(INFO, IP_RSMBL, "\n\n");
			RTE_LOG(INFO, IP_RSMBL, "====================================\n");
			RTE_LOG(INFO, IP_RSMBL, "[%p New mbuf id(N) %5u, offset %u\n", 
					m, ip->packet_id, ip->fragment_offset);
#ifdef FRAG
			{
#define NB_FRAGS		4
				struct rte_mbuf *m_table[NB_FRAGS];
				int ret;
				int i;

				ret = rte_ipv4_fragment_packet(m, (struct rte_mbuf **)&m_table, 
						NB_FRAGS, app_config.mtu, direct_pool, indirect_pool);
				rte_pktmbuf_free(m);
				RTE_LOG(INFO, IP_RSMBL, "%d fragments\n", ret);

				if (ret < 0) {
					RTE_LOG(ERR, IP_RSMBL, "fail to fragment (%d)\n", ret);
					print_mempool_status();
					continue;
				}

				/* not good to modify ret */
				for (i = 0; i < ret-1; i++) {
					RTE_LOG(INFO, IP_RSMBL, "[%p] fragments : for reassembly\n", 
							m_table[i]);
					m = reassemble(m_table[i], 0, 0, qconf, cur_tsc);
				}

				if (m != NULL) {
					RTE_LOG(ERR, IP_RSMBL, "[%p] Errorenous reassembly\n", m);
					rte_panic("Error in reassembly\n");
				}

				if (app_config.error == 1) {
					RTE_LOG(INFO, IP_RSMBL, "[%p] fragments : freed\n", 
							m_table[ret-1]);
					rte_pktmbuf_free(m_table[ret-1]);
				} else {
					RTE_LOG(INFO, IP_RSMBL, "[%p] fragments : for reassembly\n", 
							m_table[ret-1]);
					m = reassemble(m_table[ret-1], 0, 0, qconf, cur_tsc);
				}

				count++;
			}
#else
			m = reassemble(m, 0, i, qconf, cur_tsc);
			count++;
#endif

			if (m == NULL) {
				RTE_LOG(DEBUG, IP_RSMBL, "fail to reassemble\n");
			} else {
				reasm_count++;
				RTE_LOG(INFO, IP_RSMBL, "Free reassembled mbuf %p\n", m);
				rte_pktmbuf_free(m);
			}

			rte_print_lru(qconf->frag_tbl);
		}

		/* print stats */
		diff_tsc = cur_tsc - prev_print_tsc;

		if (diff_tsc > display_tsc)
		{
			uint64_t incr_rx;
			uint64_t incr_reasm;

			prev_print_tsc = cur_tsc;
			incr_rx = count - last_count;
			incr_reasm = reasm_count - last_reasm;

			RTE_LOG(INFO, IP_RSMBL, "rx %10ju(+%7ju) reasm %10ju(+%7ju), %ju Mbps, enq_fail %ju\n",
					count, incr_rx, 
					reasm_count, incr_reasm,
					incr_rx * 1500*8/1000/1000,
					app_config.enq_fail);

			print_mempool_status();

			if (app_config.stat)
				rte_ip_frag_table_statistics_dump(stdout, qconf->frag_tbl);

			last_count = count;
			last_reasm = reasm_count;
		}

		if (app_config.gc)
			rte_ip_frag_check_lru(qconf->frag_tbl, &qconf->death_row, cur_tsc);

		rte_ip_frag_free_death_row(&qconf->death_row, PREFETCH_OFFSET);
	}

	/* garbage colect repeately */
	while (rte_mempool_free_count(pool) || 
			rte_mempool_free_count(direct_pool) || 
			rte_mempool_free_count(indirect_pool)) {

		rte_delay_ms(10);	
		cur_tsc = rte_rdtsc();

		rte_ip_frag_check_lru(qconf->frag_tbl, &qconf->death_row, cur_tsc);
		rte_ip_frag_free_death_row(&qconf->death_row, PREFETCH_OFFSET);

		rte_ip_frag_table_statistics_dump(stdout, qconf->frag_tbl);
		print_mempool_status();
	}

	if (app_config.stat)
		rte_ip_frag_table_statistics_dump(stdout, qconf->frag_tbl);
}


static int
main_loop(__attribute__((unused)) void *dummy)
{
	unsigned lcore_id;
	lcore_id = rte_lcore_id();

	if (lcore_id == 0) {
		printf("[%u] Run consumer\n", lcore_id);
		consumer();
	} else {
		printf("[%u] Run producer\n", lcore_id);
		producer();
	}
}

/* display usage */
static void
print_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]"
		"  [--max-pkt-len PKTLEN]"
		"  [--maxflows=<flows>]  [--flowttl=<ttl>[(s|ms)]]\n"
		"  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
		"  -q NQ: number of RX queues per lcore\n"
		"  --maxflows=<flows>: optional, maximum number of flows "
		"supported\n"
		"  --flowttl=<ttl>[(s|ms)]: optional, maximum TTL for each "
		"flow\n"
		"  --tx_pps=<pps>:Tx PPS\n"
		"  --count=<pps>:number of packet to test "
		"  --log=<log_level>:1:Emergency, 4:Error, 7:Info, 8:Debug"
		"  --mtu=<mtu>:1500 is default value"
		"  --error=<code>:0 No error, 1 miss last fragment"
		"  --dump:1:Dump"
		"  --stat:1:Print Stats"
		"  --gc:1:Garbage colection",
		prgname);
}

static uint32_t
parse_flow_num(const char *str, uint32_t min, uint32_t max, uint32_t *val)
{
	char *end;
	uint64_t v;

	/* parse decimal string */
	errno = 0;
	v = strtoul(str, &end, 10);
	if (errno != 0 || *end != '\0')
		return (-EINVAL);

	if (v < min || v > max)
		return (-EINVAL);

	*val = (uint32_t)v;
	return (0);
}

static int
parse_flow_ttl(const char *str, uint32_t min, uint32_t max, uint32_t *val)
{
	char *end;
	uint64_t v;

	static const char frmt_sec[] = "s";
	static const char frmt_msec[] = "ms";

	/* parse decimal string */
	errno = 0;
	v = strtoul(str, &end, 10);
	if (errno != 0)
		return (-EINVAL);

	if (*end != '\0') {
		if (strncmp(frmt_sec, end, sizeof(frmt_sec)) == 0)
			v *= MS_PER_S;
		else if (strncmp(frmt_msec, end, sizeof (frmt_msec)) != 0)
			return (-EINVAL);
	}

	if (v < min || v > max)
		return (-EINVAL);

	*val = (uint32_t)v;
	return (0);
}

/* Parse the argument given in the command line of the application */
static int
parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{"max-pkt-len", 1, 0, 0},
		{"maxflows", 1, 0, 0},
		{"flowttl", 1, 0, 0},
		{"tx_pps", 1, 0, 0},
		{"count", 1, 0, 0},
		{"display_pps", 1, 0, 0},
		{"mtu", 1, 0, 0},
		{"log", 1, 0, 0},
		{"error", 1, 0, 0},
		{"dump", 0, 0, 0},
		{"stat", 0, 0, 0},
		{"gc", 0, 0, 0},
		{NULL, 0, 0, 0}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "q:",
				lgopts, &option_index)) != EOF) {

		switch (opt) {

		/* long options */
		case 0:
			if (!strncmp(lgopts[option_index].name,
					"maxflows", 8)) {
				if ((ret = parse_flow_num(optarg, MIN_FLOW_NUM,
						MAX_FLOW_NUM,
						&app_config.max_flow_num)) != 0) {
					printf("invalid value: \"%s\" for "
						"parameter %s\n",
						optarg,
						lgopts[option_index].name);
					print_usage(prgname);
					return (ret);
				}
			}

			if (!strncmp(lgopts[option_index].name, "flowttl", 7)) {
				if ((ret = parse_flow_ttl(optarg, MIN_FLOW_TTL,
						MAX_FLOW_TTL,
						&app_config.max_flow_ttl)) != 0) {
					printf("invalid value: \"%s\" for "
							"parameter %s\n",
							optarg,
							lgopts[option_index].name);
					print_usage(prgname);
					return (ret);
				}
			}

			if (!strncmp(lgopts[option_index].name, "tx_pps", 6)) {
				app_config.tx_pps = strtol(optarg, NULL, 0);

				if (app_config.tx_pps == 0) {
					printf("invalid pps\n");
					print_usage(prgname);
					return -1;
				}
			}

			if (!strncmp(lgopts[option_index].name, "display_pps", 6)) {
				app_config.display_pps = (uint32_t)strtol(optarg, NULL, 0);

				if (app_config.display_pps == 0) {
					printf("invalid pps\n");
					print_usage(prgname);
					return -1;
				}
			}

			if (!strncmp(lgopts[option_index].name, "count", 5)) {
				app_config.count = (uint32_t)strtol(optarg, NULL, 0);

				if (app_config.count == 0) {
					printf("invalid count\n");
					print_usage(prgname);
					return -1;
				}
			}

			if (!strncmp(lgopts[option_index].name, "error", 5)) {
				app_config.error = (uint32_t)strtol(optarg, NULL, 0);
			}

			if (!strncmp(lgopts[option_index].name, "mtu", 3)) {
				app_config.mtu = (uint32_t)strtol(optarg, NULL, 0);

				if ((app_config.mtu == 0) || (app_config.mtu > IPV4_MTU_DEFAULT)) {
					printf("invalid MTU\n");
					print_usage(prgname);
					return -1;
				}
				RTE_LOG(INFO, IP_RSMBL, "MTU : %u\n", app_config.mtu);
			}

			if (!strncmp(lgopts[option_index].name, "log", 3)) {
				app_config.log_level = (uint32_t)strtol(optarg, NULL, 0);

				if ((app_config.log_level < RTE_LOG_EMERG) || 
						app_config.log_level > RTE_LOG_DEBUG) {
					printf("invalid log_level\n");
					print_usage(prgname);
					return -1;
				}
			}

			if (!strncmp(lgopts[option_index].name, "dump", 3)) {
				app_config.dump = 1;
			}

			if (!strncmp(lgopts[option_index].name, "stat", 3)) {
				app_config.stat = 1;
			}

			if (!strncmp(lgopts[option_index].name, "gc", 2)) {
				app_config.gc = 1;
			}

			break;

		default:
			print_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}



static int
setup_queue_tbl(uint32_t lcore, uint32_t queue)
{
	int socket;
	uint32_t nb_mbuf;
	uint64_t frag_cycles;
	char buf[RTE_MEMPOOL_NAMESIZE];
	struct lcore_queue_conf *qconf;

	qconf = &lcore_queue_conf[lcore];

	socket = rte_lcore_to_socket_id(lcore);
	if (socket == SOCKET_ID_ANY)
		socket = 0;

	frag_cycles = (rte_get_tsc_hz() + MS_PER_S - 1) / MS_PER_S *
		app_config.max_flow_ttl;

	if ((qconf->frag_tbl = rte_ip_frag_table_create(app_config.max_flow_num,
			IP_FRAG_TBL_BUCKET_ENTRIES, app_config.max_flow_num, frag_cycles,
			socket)) == NULL) {
		RTE_LOG(ERR, IP_RSMBL, "ip_frag_tbl_create(%u) on "
			"lcore: %u for queue: %u failed\n",
			app_config.max_flow_num, lcore, queue);
		return -1;
	}

	/*
	 * At any given moment up to <max_flow_num * (MAX_FRAG_NUM)>
	 * mbufs could be stored int the fragment table.
	 * Plus, each TX queue can hold up to <max_flow_num> packets.
	 */

	nb_mbuf = RTE_MAX(app_config.max_flow_num, 2UL * MAX_PKT_BURST) * MAX_FRAG_NUM;
	nb_mbuf *= (port_conf.rxmode.max_rx_pkt_len + BUF_SIZE - 1) / BUF_SIZE;
	nb_mbuf *= 2; /* ipv4 and ipv6 */
	nb_mbuf += 1024;//RTE_TEST_RX_DESC_DEFAULT + RTE_TEST_TX_DESC_DEFAULT;

	nb_mbuf = RTE_MAX(nb_mbuf, (uint32_t)NB_MBUF);

	snprintf(buf, sizeof(buf), "mbuf_pool_%u_%u", lcore, queue);

	if ((pool = rte_mempool_create(buf, nb_mbuf, MBUF_SIZE, 0,
			sizeof(struct rte_pktmbuf_pool_private),
			rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL,
			socket, MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET)) == NULL) {
		RTE_LOG(ERR, IP_RSMBL, "mempool_create(%s) failed(%u, %ju)", buf, nb_mbuf, MBUF_SIZE);
		return -1;
	}

	return 0;
}

static int
setup_frag(void)
{
	int socket = 0;

	direct_pool = rte_pktmbuf_pool_create(DIR_MP_NAME, NB_MBUF, 32,
			0, RTE_MBUF_DEFAULT_BUF_SIZE, socket);
	if (direct_pool == NULL) {
		RTE_LOG(ERR, IP_FRAG, "Cannot create direct mempool\n");
		return -1;
	}
	RTE_LOG(ERR, IP_FRAG, "Direct_pool %p\n", direct_pool); 

	indirect_pool = rte_pktmbuf_pool_create(INDIR_MP_NAME, NB_MBUF, 32, 0, 0,
			socket);
	if (indirect_pool == NULL) {
		RTE_LOG(ERR, IP_FRAG, "Cannot create indirect mempool\n");
		return -1;
	}
	RTE_LOG(ERR, IP_FRAG, "Indirect_pool %p\n", indirect_pool); 

	return 0;
}


static int 
setup_ring(void)
{
	ring = rte_ring_create(RING_NAME, 4096, rte_socket_id(), 0);
	if (ring == NULL)
		return -1;

	RTE_LOG(INFO, IP_RSMBL, "ring created\n");

	return 0;
}

static void
signal_handler(int signum)
{
	if (signum != SIGUSR1)
		rte_exit(0, "received signal: %d, exiting\n", signum);
}

int
main(int argc, char **argv)
{
	int ret;
	unsigned lcore_id = 0;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	argc -= ret;
	argv += ret;


	/* parse application arguments (after the EAL ones) */
	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid IP reassembly parameters\n");

	printf("Set log level %d\n", app_config.log_level);
	rte_set_log_level(app_config.log_level);

	if (setup_ring() < 0)
		rte_exit(EXIT_FAILURE, "setup_ring failed\n");

	if (setup_queue_tbl(0, 0) < 0)
		rte_exit(EXIT_FAILURE, "fail to init reassembly\n");

	if (setup_frag() < 0)
		rte_exit(EXIT_FAILURE, "fail to init fragmentation\n");


	signal(SIGUSR1, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0)
			return -1;
	}

	return 0;
}
