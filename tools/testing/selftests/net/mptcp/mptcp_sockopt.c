// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netdb.h>
#include <netinet/in.h>

#include <linux/tcp.h>

static int pf = AF_INET;

#ifndef IPPROTO_MPTCP
#define IPPROTO_MPTCP 262
#endif
#ifndef SOL_MPTCP
#define SOL_MPTCP 284
#endif

#ifndef MPTCP_INFO
struct mptcp_info {
	__u8	mptcpi_subflows;
	__u8	mptcpi_add_addr_signal;
	__u8	mptcpi_add_addr_accepted;
	__u8	mptcpi_subflows_max;
	__u8	mptcpi_add_addr_signal_max;
	__u8	mptcpi_add_addr_accepted_max;
	__u32	mptcpi_flags;
	__u32	mptcpi_token;
	__u64	mptcpi_write_seq;
	__u64	mptcpi_snd_una;
	__u64	mptcpi_rcv_nxt;
	__u8	mptcpi_local_addr_used;
	__u8	mptcpi_local_addr_max;
	__u8	mptcpi_csum_enabled;
	__u32	mptcpi_retransmits;
	__u64	mptcpi_bytes_retrans;
	__u64	mptcpi_bytes_sent;
	__u64	mptcpi_bytes_received;
	__u64	mptcpi_bytes_acked;
};

struct mptcp_subflow_data {
	__u32		size_subflow_data;		/* size of this structure in userspace */
	__u32		num_subflows;			/* must be 0, set by kernel */
	__u32		size_kernel;			/* must be 0, set by kernel */
	__u32		size_user;			/* size of one element in data[] */
} __attribute__((aligned(8)));

struct mptcp_subflow_addrs {
	union {
		__kernel_sa_family_t sa_family;
		struct sockaddr sa_local;
		struct sockaddr_in sin_local;
		struct sockaddr_in6 sin6_local;
		struct __kernel_sockaddr_storage ss_local;
	};
	union {
		struct sockaddr sa_remote;
		struct sockaddr_in sin_remote;
		struct sockaddr_in6 sin6_remote;
		struct __kernel_sockaddr_storage ss_remote;
	};
};

#define MPTCP_INFO		1
#define MPTCP_TCPINFO		2
#define MPTCP_SUBFLOW_ADDRS	3
#endif

#ifndef MPTCP_FULL_INFO
struct mptcp_subflow_info {
	__u32				id;
	struct mptcp_subflow_addrs	addrs;
};

struct mptcp_full_info {
	__u32		size_tcpinfo_kernel;	/* must be 0, set by kernel */
	__u32		size_tcpinfo_user;
	__u32		size_sfinfo_kernel;	/* must be 0, set by kernel */
	__u32		size_sfinfo_user;
	__u32		num_subflows;		/* must be 0, set by kernel (real subflow count) */
	__u32		size_arrays_user;	/* max subflows that userspace is interested in;
						 * the buffers at subflow_info/tcp_info
						 * are respectively at least:
						 *  size_arrays * size_sfinfo_user
						 *  size_arrays * size_tcpinfo_user
						 * bytes wide
						 */
	__aligned_u64		subflow_info;
	__aligned_u64		tcp_info;
	struct mptcp_info	mptcp_info;
};

#define MPTCP_FULL_INFO		4
#endif

struct so_state {
	struct mptcp_info mi;
	struct mptcp_info last_sample;
	struct tcp_info tcp_info;
	struct mptcp_subflow_addrs addrs;
	uint64_t mptcpi_rcv_delta;
	uint64_t tcpi_rcv_delta;
	bool pkt_stats_avail;
};

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static void die_perror(const char *msg)
{
	perror(msg);
	exit(1);
}

static void die_usage(int r)
{
	fprintf(stderr, "Usage: mptcp_sockopt [-6]\n");
	exit(r);
}

static void xerror(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static const char *getxinfo_strerr(int err)
{
	if (err == EAI_SYSTEM)
		return strerror(errno);

	return gai_strerror(err);
}

static void xgetaddrinfo(const char *node, const char *service,
			 struct addrinfo *hints,
			 struct addrinfo **res)
{
again:
	int err = getaddrinfo(node, service, hints, res);

	if (err) {
		const char *errstr;

		if (err == EAI_SOCKTYPE) {
			hints->ai_protocol = IPPROTO_TCP;
			goto again;
		}

		errstr = getxinfo_strerr(err);

		fprintf(stderr, "Fatal: getaddrinfo(%s:%s): %s\n",
			node ? node : "", service ? service : "", errstr);
		exit(1);
	}
}

static int sock_listen_mptcp(const char * const listenaddr,
			     const char * const port)
{
	int sock = -1;
	struct addrinfo hints = {
		.ai_protocol = IPPROTO_MPTCP,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE | AI_NUMERICHOST
	};

	hints.ai_family = pf;

	struct addrinfo *a, *addr;
	int one = 1;

	xgetaddrinfo(listenaddr, port, &hints, &addr);
	hints.ai_family = pf;

	for (a = addr; a; a = a->ai_next) {
		sock = socket(a->ai_family, a->ai_socktype, IPPROTO_MPTCP);
		if (sock < 0)
			continue;

		if (-1 == setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one,
				     sizeof(one)))
			perror("setsockopt");

		if (bind(sock, a->ai_addr, a->ai_addrlen) == 0)
			break; /* success */

		perror("bind");
		close(sock);
		sock = -1;
	}

	freeaddrinfo(addr);

	if (sock < 0)
		xerror("could not create listen socket");

	if (listen(sock, 20))
		die_perror("listen");

	return sock;
}

static int sock_connect_mptcp(const char * const remoteaddr,
			      const char * const port, int proto)
{
	struct addrinfo hints = {
		.ai_protocol = IPPROTO_MPTCP,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *a, *addr;
	int sock = -1;

	hints.ai_family = pf;

	xgetaddrinfo(remoteaddr, port, &hints, &addr);
	for (a = addr; a; a = a->ai_next) {
		sock = socket(a->ai_family, a->ai_socktype, proto);
		if (sock < 0)
			continue;

		if (connect(sock, a->ai_addr, a->ai_addrlen) == 0)
			break; /* success */

		die_perror("connect");
	}

	if (sock < 0)
		xerror("could not create connect socket");

	freeaddrinfo(addr);
	return sock;
}

static void parse_opts(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "h6")) != -1) {
		switch (c) {
		case 'h':
			die_usage(0);
			break;
		case '6':
			pf = AF_INET6;
			break;
		default:
			die_usage(1);
			break;
		}
	}
}

static void do_getsockopt_bogus_sf_data(int fd, int optname)
{
	struct mptcp_subflow_data good_data;
	struct bogus_data {
		struct mptcp_subflow_data d;
		char buf[2];
	} bd;
	socklen_t olen, _olen;
	int ret;

	memset(&bd, 0, sizeof(bd));
	memset(&good_data, 0, sizeof(good_data));

	olen = sizeof(good_data);
	good_data.size_subflow_data = olen;

	ret = getsockopt(fd, SOL_MPTCP, optname, &bd, &olen);
	assert(ret < 0); /* 0 size_subflow_data */
	assert(olen == sizeof(good_data));

	bd.d = good_data;

	ret = getsockopt(fd, SOL_MPTCP, optname, &bd, &olen);
	assert(ret == 0);
	assert(olen == sizeof(good_data));
	assert(bd.d.num_subflows == 1);
	assert(bd.d.size_kernel > 0);
	assert(bd.d.size_user == 0);

	bd.d = good_data;
	_olen = rand() % olen;
	olen = _olen;
	ret = getsockopt(fd, SOL_MPTCP, optname, &bd, &olen);
	assert(ret < 0);	/* bogus olen */
	assert(olen == _olen);	/* must be unchanged */

	bd.d = good_data;
	olen = sizeof(good_data);
	bd.d.size_kernel = 1;
	ret = getsockopt(fd, SOL_MPTCP, optname, &bd, &olen);
	assert(ret < 0); /* size_kernel not 0 */

	bd.d = good_data;
	olen = sizeof(good_data);
	bd.d.num_subflows = 1;
	ret = getsockopt(fd, SOL_MPTCP, optname, &bd, &olen);
	assert(ret < 0); /* num_subflows not 0 */

	/* forward compat check: larger struct mptcp_subflow_data on 'old' kernel */
	bd.d = good_data;
	olen = sizeof(bd);
	bd.d.size_subflow_data = sizeof(bd);

	ret = getsockopt(fd, SOL_MPTCP, optname, &bd, &olen);
	assert(ret == 0);

	/* olen must be truncated to real data size filled by kernel: */
	assert(olen == sizeof(good_data));

	assert(bd.d.size_subflow_data == sizeof(bd));

	bd.d = good_data;
	bd.d.size_subflow_data += 1;
	bd.d.size_user = 1;
	olen = bd.d.size_subflow_data + 1;
	_olen = olen;

	ret = getsockopt(fd, SOL_MPTCP, optname, &bd, &_olen);
	assert(ret == 0);

	/* no truncation, kernel should have filled 1 byte of optname payload in buf[1]: */
	assert(olen == _olen);

	assert(bd.d.size_subflow_data == sizeof(good_data) + 1);
	assert(bd.buf[0] == 0);
}

static void do_getsockopt_mptcp_info(struct so_state *s, int fd, size_t w)
{
	struct mptcp_info i;
	socklen_t olen;
	int ret;

	olen = sizeof(i);
	ret = getsockopt(fd, SOL_MPTCP, MPTCP_INFO, &i, &olen);

	if (ret < 0)
		die_perror("getsockopt MPTCP_INFO");

	s->pkt_stats_avail = olen >= sizeof(i);

	s->last_sample = i;
	if (s->mi.mptcpi_write_seq == 0)
		s->mi = i;

	assert(s->mi.mptcpi_write_seq + w == i.mptcpi_write_seq);

	s->mptcpi_rcv_delta = i.mptcpi_rcv_nxt - s->mi.mptcpi_rcv_nxt;
}

static void do_getsockopt_tcp_info(struct so_state *s, int fd, size_t r, size_t w)
{
	struct my_tcp_info {
		struct mptcp_subflow_data d;
		struct tcp_info ti[2];
	} ti;
	int ret, tries = 5;
	socklen_t olen;

	do {
		memset(&ti, 0, sizeof(ti));

		ti.d.size_subflow_data = sizeof(struct mptcp_subflow_data);
		ti.d.size_user = sizeof(struct tcp_info);
		olen = sizeof(ti);

		ret = getsockopt(fd, SOL_MPTCP, MPTCP_TCPINFO, &ti, &olen);
		if (ret < 0)
			xerror("getsockopt MPTCP_TCPINFO (tries %d, %m)");

		assert(olen <= sizeof(ti));
		assert(ti.d.size_kernel > 0);
		assert(ti.d.size_user ==
		       MIN(ti.d.size_kernel, sizeof(struct tcp_info)));
		assert(ti.d.num_subflows == 1);

		assert(olen > (socklen_t)sizeof(struct mptcp_subflow_data));
		olen -= sizeof(struct mptcp_subflow_data);
		assert(olen == ti.d.size_user);

		s->tcp_info = ti.ti[0];

		if (ti.ti[0].tcpi_bytes_sent == w &&
		    ti.ti[0].tcpi_bytes_received == r)
			goto done;

		if (r == 0 && ti.ti[0].tcpi_bytes_sent == w &&
		    ti.ti[0].tcpi_bytes_received) {
			s->tcpi_rcv_delta = ti.ti[0].tcpi_bytes_received;
			goto done;
		}

		/* wait and repeat, might be that tx is still ongoing */
		sleep(1);
	} while (tries-- > 0);

	xerror("tcpi_bytes_sent %" PRIu64 ", want %zu. tcpi_bytes_received %" PRIu64 ", want %zu",
		ti.ti[0].tcpi_bytes_sent, w, ti.ti[0].tcpi_bytes_received, r);

done:
	do_getsockopt_bogus_sf_data(fd, MPTCP_TCPINFO);
}

static void do_getsockopt_subflow_addrs(struct so_state *s, int fd)
{
	struct sockaddr_storage remote, local;
	socklen_t olen, rlen, llen;
	int ret;
	struct my_addrs {
		struct mptcp_subflow_data d;
		struct mptcp_subflow_addrs addr[2];
	} addrs;

	memset(&addrs, 0, sizeof(addrs));
	memset(&local, 0, sizeof(local));
	memset(&remote, 0, sizeof(remote));

	addrs.d.size_subflow_data = sizeof(struct mptcp_subflow_data);
	addrs.d.size_user = sizeof(struct mptcp_subflow_addrs);
	olen = sizeof(addrs);

	ret = getsockopt(fd, SOL_MPTCP, MPTCP_SUBFLOW_ADDRS, &addrs, &olen);
	if (ret < 0)
		die_perror("getsockopt MPTCP_SUBFLOW_ADDRS");

	assert(olen <= sizeof(addrs));
	assert(addrs.d.size_kernel > 0);
	assert(addrs.d.size_user ==
	       MIN(addrs.d.size_kernel, sizeof(struct mptcp_subflow_addrs)));
	assert(addrs.d.num_subflows == 1);

	assert(olen > (socklen_t)sizeof(struct mptcp_subflow_data));
	olen -= sizeof(struct mptcp_subflow_data);
	assert(olen == addrs.d.size_user);

	llen = sizeof(local);
	ret = getsockname(fd, (struct sockaddr *)&local, &llen);
	if (ret < 0)
		die_perror("getsockname");
	rlen = sizeof(remote);
	ret = getpeername(fd, (struct sockaddr *)&remote, &rlen);
	if (ret < 0)
		die_perror("getpeername");

	assert(rlen > 0);
	assert(rlen == llen);

	assert(remote.ss_family == local.ss_family);

	assert(memcmp(&local, &addrs.addr[0].ss_local, sizeof(local)) == 0);
	assert(memcmp(&remote, &addrs.addr[0].ss_remote, sizeof(remote)) == 0);
	s->addrs = addrs.addr[0];

	memset(&addrs, 0, sizeof(addrs));

	addrs.d.size_subflow_data = sizeof(struct mptcp_subflow_data);
	addrs.d.size_user = sizeof(sa_family_t);
	olen = sizeof(addrs.d) + sizeof(sa_family_t);

	ret = getsockopt(fd, SOL_MPTCP, MPTCP_SUBFLOW_ADDRS, &addrs, &olen);
	assert(ret == 0);
	assert(olen == sizeof(addrs.d) + sizeof(sa_family_t));

	assert(addrs.addr[0].sa_family == pf);
	assert(addrs.addr[0].sa_family == local.ss_family);

	assert(memcmp(&local, &addrs.addr[0].ss_local, sizeof(local)) != 0);
	assert(memcmp(&remote, &addrs.addr[0].ss_remote, sizeof(remote)) != 0);

	do_getsockopt_bogus_sf_data(fd, MPTCP_SUBFLOW_ADDRS);
}

static void do_getsockopt_mptcp_full_info(struct so_state *s, int fd)
{
	size_t data_size = sizeof(struct mptcp_full_info);
	struct mptcp_subflow_info sfinfo[2];
	struct tcp_info tcp_info[2];
	struct mptcp_full_info mfi;
	socklen_t olen;
	int ret;

	memset(&mfi, 0, data_size);
	memset(tcp_info, 0, sizeof(tcp_info));
	memset(sfinfo, 0, sizeof(sfinfo));

	mfi.size_tcpinfo_user = sizeof(struct tcp_info);
	mfi.size_sfinfo_user = sizeof(struct mptcp_subflow_info);
	mfi.size_arrays_user = 2;
	mfi.subflow_info = (unsigned long)&sfinfo[0];
	mfi.tcp_info = (unsigned long)&tcp_info[0];
	olen = data_size;

	ret = getsockopt(fd, SOL_MPTCP, MPTCP_FULL_INFO, &mfi, &olen);
	if (ret < 0) {
		if (errno == EOPNOTSUPP) {
			perror("MPTCP_FULL_INFO test skipped");
			return;
		}
		xerror("getsockopt MPTCP_FULL_INFO");
	}

	assert(olen <= data_size);
	assert(mfi.size_tcpinfo_kernel > 0);
	assert(mfi.size_tcpinfo_user ==
	       MIN(mfi.size_tcpinfo_kernel, sizeof(struct tcp_info)));
	assert(mfi.size_sfinfo_kernel > 0);
	assert(mfi.size_sfinfo_user ==
	       MIN(mfi.size_sfinfo_kernel, sizeof(struct mptcp_subflow_info)));
	assert(mfi.num_subflows == 1);

	/* Tolerate future extension to mptcp_info struct and running newer
	 * test on top of older kernel.
	 * Anyway any kernel supporting MPTCP_FULL_INFO must at least include
	 * the following in mptcp_info.
	 */
	assert(olen > (socklen_t)__builtin_offsetof(struct mptcp_full_info, tcp_info));
	assert(mfi.mptcp_info.mptcpi_subflows == 0);
	assert(mfi.mptcp_info.mptcpi_bytes_sent == s->last_sample.mptcpi_bytes_sent);
	assert(mfi.mptcp_info.mptcpi_bytes_received == s->last_sample.mptcpi_bytes_received);

	assert(sfinfo[0].id == 1);
	assert(tcp_info[0].tcpi_bytes_sent == s->tcp_info.tcpi_bytes_sent);
	assert(tcp_info[0].tcpi_bytes_received == s->tcp_info.tcpi_bytes_received);
	assert(!memcmp(&sfinfo->addrs, &s->addrs, sizeof(struct mptcp_subflow_addrs)));
}

static void do_getsockopts(struct so_state *s, int fd, size_t r, size_t w)
{
	do_getsockopt_mptcp_info(s, fd, w);

	do_getsockopt_tcp_info(s, fd, r, w);

	do_getsockopt_subflow_addrs(s, fd);

	if (r)
		do_getsockopt_mptcp_full_info(s, fd);
}

static void connect_one_server(int fd, int pipefd)
{
	char buf[4096], buf2[4096];
	size_t len, i, total;
	struct so_state s;
	bool eof = false;
	ssize_t ret;

	memset(&s, 0, sizeof(s));

	len = rand() % (sizeof(buf) - 1);

	if (len < 128)
		len = 128;

	for (i = 0; i < len ; i++) {
		buf[i] = rand() % 26;
		buf[i] += 'A';
	}

	buf[i] = '\n';

	do_getsockopts(&s, fd, 0, 0);

	/* un-block server */
	ret = read(pipefd, buf2, 4);
	assert(ret == 4);
	close(pipefd);

	assert(strncmp(buf2, "xmit", 4) == 0);

	ret = write(fd, buf, len);
	if (ret < 0)
		die_perror("write");

	if (ret != (ssize_t)len)
		xerror("short write");

	total = 0;
	do {
		ret = read(fd, buf2 + total, sizeof(buf2) - total);
		if (ret < 0)
			die_perror("read");
		if (ret == 0) {
			eof = true;
			break;
		}

		total += ret;
	} while (total < len);

	if (total != len)
		xerror("total %lu, len %lu eof %d\n", total, len, eof);

	if (memcmp(buf, buf2, len))
		xerror("data corruption");

	if (s.tcpi_rcv_delta)
		assert(s.tcpi_rcv_delta <= total);

	do_getsockopts(&s, fd, ret, ret);

	if (eof)
		total += 1; /* sequence advances due to FIN */

	assert(s.mptcpi_rcv_delta == (uint64_t)total);
	close(fd);
}

static void process_one_client(int fd, int pipefd)
{
	ssize_t ret, ret2, ret3;
	struct so_state s;
	char buf[4096];

	memset(&s, 0, sizeof(s));
	do_getsockopts(&s, fd, 0, 0);

	ret = write(pipefd, "xmit", 4);
	assert(ret == 4);

	ret = read(fd, buf, sizeof(buf));
	if (ret < 0)
		die_perror("read");

	assert(s.mptcpi_rcv_delta <= (uint64_t)ret);

	if (s.tcpi_rcv_delta)
		assert(s.tcpi_rcv_delta == (uint64_t)ret);

	ret2 = write(fd, buf, ret);
	if (ret2 < 0)
		die_perror("write");

	/* wait for hangup */
	ret3 = read(fd, buf, 1);
	if (ret3 != 0)
		xerror("expected EOF, got %lu", ret3);

	do_getsockopts(&s, fd, ret, ret2);
	if (s.mptcpi_rcv_delta != (uint64_t)ret + 1)
		xerror("mptcpi_rcv_delta %" PRIu64 ", expect %" PRIu64, s.mptcpi_rcv_delta, ret + 1, s.mptcpi_rcv_delta - ret);

	/* be nice when running on top of older kernel */
	if (s.pkt_stats_avail) {
		if (s.last_sample.mptcpi_bytes_sent != ret2)
			xerror("mptcpi_bytes_sent %" PRIu64 ", expect %" PRIu64,
			       s.last_sample.mptcpi_bytes_sent, ret2,
			       s.last_sample.mptcpi_bytes_sent - ret2);
		if (s.last_sample.mptcpi_bytes_received != ret)
			xerror("mptcpi_bytes_received %" PRIu64 ", expect %" PRIu64,
			       s.last_sample.mptcpi_bytes_received, ret,
			       s.last_sample.mptcpi_bytes_received - ret);
		if (s.last_sample.mptcpi_bytes_acked != ret)
			xerror("mptcpi_bytes_acked %" PRIu64 ", expect %" PRIu64,
			       s.last_sample.mptcpi_bytes_acked, ret2,
			       s.last_sample.mptcpi_bytes_acked - ret2);
	}

	close(fd);
}

static int xaccept(int s)
{
	int fd = accept(s, NULL, 0);

	if (fd < 0)
		die_perror("accept");

	return fd;
}

static int server(int pipefd)
{
	int fd = -1, r;

	switch (pf) {
	case AF_INET:
		fd = sock_listen_mptcp("127.0.0.1", "15432");
		break;
	case AF_INET6:
		fd = sock_listen_mptcp("::1", "15432");
		break;
	default:
		xerror("Unknown pf %d\n", pf);
		break;
	}

	r = write(pipefd, "conn", 4);
	assert(r == 4);

	alarm(15);
	r = xaccept(fd);

	process_one_client(r, pipefd);

	return 0;
}

static void test_ip_tos_sockopt(int fd)
{
	uint8_t tos_in, tos_out;
	socklen_t s;
	int r;

	tos_in = rand() & 0xfc;
	r = setsockopt(fd, SOL_IP, IP_TOS, &tos_in, sizeof(tos_out));
	if (r != 0)
		die_perror("setsockopt IP_TOS");

	tos_out = 0;
	s = sizeof(tos_out);
	r = getsockopt(fd, SOL_IP, IP_TOS, &tos_out, &s);
	if (r != 0)
		die_perror("getsockopt IP_TOS");

	if (tos_in != tos_out)
		xerror("tos %x != %x socklen_t %d\n", tos_in, tos_out, s);

	if (s != 1)
		xerror("tos should be 1 byte");

	s = 0;
	r = getsockopt(fd, SOL_IP, IP_TOS, &tos_out, &s);
	if (r != 0)
		die_perror("getsockopt IP_TOS 0");
	if (s != 0)
		xerror("expect socklen_t == 0");

	s = -1;
	r = getsockopt(fd, SOL_IP, IP_TOS, &tos_out, &s);
	if (r != -1 && errno != EINVAL)
		die_perror("getsockopt IP_TOS did not indicate -EINVAL");
	if (s != -1)
		xerror("expect socklen_t == -1");
}

static int client(int pipefd)
{
	int fd = -1;

	alarm(15);

	switch (pf) {
	case AF_INET:
		fd = sock_connect_mptcp("127.0.0.1", "15432", IPPROTO_MPTCP);
		break;
	case AF_INET6:
		fd = sock_connect_mptcp("::1", "15432", IPPROTO_MPTCP);
		break;
	default:
		xerror("Unknown pf %d\n", pf);
	}

	test_ip_tos_sockopt(fd);

	connect_one_server(fd, pipefd);

	return 0;
}

static pid_t xfork(void)
{
	pid_t p = fork();

	if (p < 0)
		die_perror("fork");

	return p;
}

static int rcheck(int wstatus, const char *what)
{
	if (WIFEXITED(wstatus)) {
		if (WEXITSTATUS(wstatus) == 0)
			return 0;
		fprintf(stderr, "%s exited, status=%d\n", what, WEXITSTATUS(wstatus));
		return WEXITSTATUS(wstatus);
	} else if (WIFSIGNALED(wstatus)) {
		xerror("%s killed by signal %d\n", what, WTERMSIG(wstatus));
	} else if (WIFSTOPPED(wstatus)) {
		xerror("%s stopped by signal %d\n", what, WSTOPSIG(wstatus));
	}

	return 111;
}

static void init_rng(void)
{
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd >= 0) {
		unsigned int foo;
		ssize_t ret;

		/* can't fail */
		ret = read(fd, &foo, sizeof(foo));
		assert(ret == sizeof(foo));

		close(fd);
		srand(foo);
	} else {
		srand(time(NULL));
	}
}

int main(int argc, char *argv[])
{
	int e1, e2, wstatus;
	pid_t s, c, ret;
	int pipefds[2];

	parse_opts(argc, argv);

	init_rng();

	e1 = pipe(pipefds);
	if (e1 < 0)
		die_perror("pipe");

	s = xfork();
	if (s == 0)
		return server(pipefds[1]);

	close(pipefds[1]);

	/* wait until server bound a socket */
	e1 = read(pipefds[0], &e1, 4);
	assert(e1 == 4);

	c = xfork();
	if (c == 0)
		return client(pipefds[0]);

	close(pipefds[0]);

	ret = waitpid(s, &wstatus, 0);
	if (ret == -1)
		die_perror("waitpid");
	e1 = rcheck(wstatus, "server");
	ret = waitpid(c, &wstatus, 0);
	if (ret == -1)
		die_perror("waitpid");
	e2 = rcheck(wstatus, "client");

	return e1 ? e1 : e2;
}
