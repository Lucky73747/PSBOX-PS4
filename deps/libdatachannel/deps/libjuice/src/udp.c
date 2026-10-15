/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "udp.h"
#include "addr.h"
#include "log.h"
#include "random.h"
#include "thread.h" // for mutexes
#include "timestamp.h"

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__ORBIS__) || defined(__PS4__)
#include <orbis/Net.h>
#endif

#if defined(__ORBIS__) || defined(__PS4__)
static const char *ice_errno_name(int error) {
	switch (error) {
	case EINVAL: return "EINVAL";
	case EACCES: return "EACCES";
	case EADDRINUSE: return "EADDRINUSE";
	case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
	case EAGAIN: return "EAGAIN";
	case EDESTADDRREQ: return "EDESTADDRREQ";
	case EFAULT: return "EFAULT";
	case EINTR: return "EINTR";
	case EAFNOSUPPORT: return "EAFNOSUPPORT";
	case ECONNREFUSED: return "ECONNREFUSED";
	case ECONNRESET: return "ECONNRESET";
	case EISCONN: return "EISCONN";
	case EMSGSIZE: return "EMSGSIZE";
	case ENETDOWN: return "ENETDOWN";
	case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
	case ENOBUFS: return "ENOBUFS";
	case ENOMEM: return "ENOMEM";
	case ENOTSOCK: return "ENOTSOCK";
	case ENOPROTOOPT: return "ENOPROTOOPT";
	case ENETUNREACH: return "ENETUNREACH";
	case EHOSTUNREACH: return "EHOSTUNREACH";
	default: return "other";
	}
}

static void ice_log_sockaddr(const char *where, const struct sockaddr *sa, socklen_t len) {
	char hex[3 * 28 + 1];
	const unsigned char *bytes = (const unsigned char *)sa;
	size_t count = len < 28 ? (size_t)len : 28;
	size_t used = 0;
	for (size_t i = 0; i < count && used + 4 < sizeof(hex); ++i) {
		int n = snprintf(hex + used, sizeof(hex) - used, "%s%02x", i ? " " : "", bytes[i]);
		if (n < 0) break;
		used += (size_t)n;
	}
	hex[used] = '\0';
	int family = len >= sizeof(sa->sa_family) ? sa->sa_family : -1;
	int len_field = -1;
	unsigned port = 0;
	if (family == AF_INET && len >= sizeof(struct sockaddr_in)) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
		len_field = sin->sin_len;
		port = ntohs(sin->sin_port);
	} else if (family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
		len_field = sin6->sin6_len;
		port = ntohs(sin6->sin6_port);
	}
	JLOG_WARN("ICE_DIAG sockaddr where=%s socklen=%u family=%d AF_INET=%d AF_INET6=%d sa_len=%d port=%u bytes=%s",
	          where, (unsigned)len, family, AF_INET, AF_INET6, len_field, port, hex);
}

static void ice_log_getsockname(socket_t sock, const char *where) {
	struct sockaddr_storage addr;
	memset(&addr, 0, sizeof(addr));
	socklen_t len = sizeof(addr);
	int rc = getsockname(sock, (struct sockaddr *)&addr, &len);
	int error = rc < 0 ? sockerrno : 0;
	int family = rc == 0 ? addr_storage_family(&addr) : -1;
	char ip[INET6_ADDRSTRLEN] = "?";
	unsigned port = 0;
	if (rc == 0 && family == AF_INET && len >= sizeof(struct sockaddr_in)) {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)&addr;
		port = ntohs(sin->sin_port);
		inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
	} else if (rc == 0 && family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&addr;
		port = ntohs(sin6->sin6_port);
		inet_ntop(AF_INET6, &sin6->sin6_addr, ip, sizeof(ip));
	}
	JLOG_WARN("ICE_DIAG getsockname where=%s rc=%d errno=%d(%s) socklen=%u family=%d address=%s port=%u",
	          where, rc, error, ice_errno_name(error), (unsigned)len, family, ip, port);
	if (rc == 0)
		ice_log_sockaddr(where, (const struct sockaddr *)&addr, len);
}

static volatile int32_t ice_datagram_count = 0;
static volatile int32_t ice_first_datagram_logged = 0;
static volatile int64_t ice_first_stun_send_ms = 0;
static volatile int64_t ice_first_stun_recv_ms = 0;

static bool ice_is_stun_packet(const char *data, size_t size) {
	const uint8_t *bytes = (const uint8_t *)data;
	return data && size >= 20 && (bytes[0] & 0xc0) == 0 && bytes[4] == 0x21 &&
	       bytes[5] == 0x12 && bytes[6] == 0xa4 && bytes[7] == 0x42;
}
#endif

static struct addrinfo *find_family(struct addrinfo *ai_list, int family) {
	struct addrinfo *ai = ai_list;
	while (ai && ai->ai_family != family)
		ai = ai->ai_next;
	return ai;
}

static uint16_t get_next_port_in_range(uint16_t begin, uint16_t end) {
	if (begin == 0)
		begin = 1024;
	if (end == 0)
		end = 0xFFFF;
	if (begin == end)
		return begin;

	static volatile uint32_t count = 0;
	if (count == 0)
		count = juice_rand32();

	static mutex_t mutex = MUTEX_INITIALIZER;
	mutex_lock(&mutex);
	uint32_t diff = end > begin ? end - begin : 0;
	uint16_t next = begin + count++ % (diff + 1);
	mutex_unlock(&mutex);
	return next;
}

static socket_t create_socket_for_addrinfo(const udp_socket_config_t *config,
                                           const struct addrinfo *ai) {
	const char *family_name = ai->ai_family == AF_INET6 ? "IPv6" :
	                          ai->ai_family == AF_INET ? "IPv4" : "other";
	// Create socket
	int socket_type = ai->ai_socktype;
#if defined(__ORBIS__) || defined(__PS4__)
	/* Set nonblocking atomically at creation: app policy rejects later F_SETFL. */
	socket_type |= SOCK_NONBLOCK;
#endif
	JLOG_WARN("ICE_DIAG socket create family=%s type=0x%x", family_name, socket_type);
	socket_t sock = socket(ai->ai_family, socket_type, ai->ai_protocol);
	if (sock == INVALID_SOCKET) {
		const int err = sockerrno;
		JLOG_WARN("ICE_DIAG socket family=%s failed errno=%d", family_name, err);
#if defined(__ORBIS__) || defined(__PS4__)
		JLOG_WARN("STAGE L1 FAIL event=socket family=%s errno=%d(%s)", family_name, err,
		          ice_errno_name(err));
#endif
		return INVALID_SOCKET;
	}
	JLOG_WARN("ICE_DIAG socket family=%s created fd=%d", family_name, (int)sock);

	// Listen on both IPv6 and IPv4
	const sockopt_t disabled = 0;
	if (ai->ai_family == AF_INET6) {
		const int v6only_set_rc =
		    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&disabled, sizeof(disabled));
#if defined(__ORBIS__) || defined(__PS4__)
		const int v6only_set_errno = v6only_set_rc < 0 ? sockerrno : 0;
		int v6only_effective = -1;
		socklen_t v6only_len = sizeof(v6only_effective);
		const int v6only_get_rc =
		    getsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only_effective, &v6only_len);
		const int v6only_get_errno = v6only_get_rc < 0 ? sockerrno : 0;
		JLOG_WARN("ICE_DIAG IPV6_V6ONLY set_rc=%d set_errno=%d get_rc=%d get_errno=%d effective=%d value_len=%u",
		          v6only_set_rc, v6only_set_errno, v6only_get_rc, v6only_get_errno,
		          v6only_effective, (unsigned)v6only_len);
#endif
	}

		// Set DF flag
#ifndef NO_PMTUDISC
	const sockopt_t val = IP_PMTUDISC_DO;
	setsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, (const char *)&val, sizeof(val));
#ifdef IPV6_MTU_DISCOVER
	if (ai->ai_family == AF_INET6)
		setsockopt(sock, IPPROTO_IPV6, IPV6_MTU_DISCOVER, (const char *)&val, sizeof(val));
#endif
#else
	// It seems Mac OS lacks a way to set the DF flag...
	const sockopt_t enabled = 1;
#ifdef IP_DONTFRAG
	setsockopt(sock, IPPROTO_IP, IP_DONTFRAG, (const char *)&enabled, sizeof(enabled));
#endif
#ifdef IPV6_DONTFRAG
	if (ai->ai_family == AF_INET6)
		setsockopt(sock, IPPROTO_IPV6, IPV6_DONTFRAG, (const char *)&enabled, sizeof(enabled));
#endif
#endif

	// Set buffer size up to 1 MiB for performance
	const sockopt_t buffer_size = 1 * 1024 * 1024;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer_size, sizeof(buffer_size));
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&buffer_size, sizeof(buffer_size));

#if defined(__ORBIS__) || defined(__PS4__)
	/* Confirm the create-time flag took effect before entering the poll loop. */
	const int flags_after = fcntl(sock, F_GETFL, 0);
	if (flags_after < 0) {
		const int err = sockerrno;
		JLOG_ERROR("ICE_DIAG SOCK_NONBLOCK verify F_GETFL family=%s fd=%d rc=%d errno=%d",
		           family_name, (int)sock, flags_after, err);
		goto error;
	}
	if ((flags_after & O_NONBLOCK) == 0) {
		JLOG_ERROR("ICE_DIAG SOCK_NONBLOCK verification failed family=%s fd=%d type=0x%x flags_after=0x%x",
		           family_name, (int)sock, socket_type, flags_after);
		goto error;
	}
	JLOG_WARN("ICE_DIAG SOCK_NONBLOCK family=%s fd=%d type=0x%x verify_rc=0 flags_after=0x%x nonblock=1",
	          family_name, (int)sock, socket_type, flags_after);
#else
	ctl_t nbio = 1;
	if (ioctlsocket(sock, FIONBIO, &nbio)) {
		const int err = sockerrno;
		JLOG_ERROR("ICE_DIAG FIONBIO family=%s fd=%d failed errno=%d", family_name, (int)sock, err);
		goto error;
	}
	JLOG_WARN("ICE_DIAG FIONBIO family=%s fd=%d success", family_name, (int)sock);
#endif

	// Bind it
	if (config->port_begin == 0 && config->port_end == 0) {
		JLOG_WARN("ICE_DIAG bind begin family=%s fd=%d port=ephemeral", family_name, (int)sock);
		addr_set_sockaddr_len((struct sockaddr *)ai->ai_addr);
		if (bind(sock, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) {
			JLOG_WARN("ICE_DIAG bind success family=%s fd=%d port=%hu", family_name, (int)sock,
			          udp_get_port(sock));
#if defined(__ORBIS__) || defined(__PS4__)
			ice_log_getsockname(sock, "post_bind");
			JLOG_WARN("STAGE L1 OK event=bound family=%s fd=%d", family_name, (int)sock);
#endif
			return sock;
		}
		const int err = sockerrno;
		JLOG_WARN("ICE_DIAG bind failed family=%s fd=%d errno=%d", family_name, (int)sock, err);
#if defined(__ORBIS__) || defined(__PS4__)
		JLOG_WARN("STAGE L1 FAIL event=bind family=%s len=%u errno=%d(%s)", family_name,
		          (unsigned)ai->ai_addrlen, err, ice_errno_name(err));
#endif

	} else if (config->port_begin == config->port_end) {
		uint16_t port = config->port_begin;
		struct sockaddr_storage addr;
		socklen_t addrlen = (socklen_t)ai->ai_addrlen;
		memcpy(&addr, ai->ai_addr, addrlen);
		addr_set_port((struct sockaddr *)&addr, port);
		addr_set_sockaddr_len((struct sockaddr *)&addr);

		if (bind(sock, (struct sockaddr *)&addr, addrlen) == 0) {
			JLOG_DEBUG("UDP socket bound to %s:%hu",
			           config->bind_address ? config->bind_address : "any", port);
#if defined(__ORBIS__) || defined(__PS4__)
			ice_log_getsockname(sock, "post_bind");
			JLOG_WARN("STAGE L1 OK event=bound family=%s fd=%d port=%hu", family_name,
			          (int)sock, port);
#endif
			return sock;
		}

		const int err = sockerrno;
		JLOG_WARN("ICE_DIAG bind failed family=%s fd=%d port=%hu errno=%d", family_name,
		          (int)sock, port, err);
#if defined(__ORBIS__) || defined(__PS4__)
		JLOG_WARN("STAGE L1 FAIL event=bind family=%s len=%u errno=%d(%s)", family_name,
		          (unsigned)addrlen, err, ice_errno_name(err));
#endif

	} else {
		struct sockaddr_storage addr;
		socklen_t addrlen = (socklen_t)ai->ai_addrlen;
		memcpy(&addr, ai->ai_addr, addrlen);
		addr_set_sockaddr_len((struct sockaddr *)&addr);

		int retries = config->port_end - config->port_begin;
		do {
			uint16_t port = get_next_port_in_range(config->port_begin, config->port_end);
			addr_set_port((struct sockaddr *)&addr, port);
			addr_set_sockaddr_len((struct sockaddr *)&addr);
			if (bind(sock, (struct sockaddr *)&addr, addrlen) == 0) {
				JLOG_DEBUG("UDP socket bound to %s:%hu",
				           config->bind_address ? config->bind_address : "any", port);
#if defined(__ORBIS__) || defined(__PS4__)
				ice_log_getsockname(sock, "post_bind");
				JLOG_WARN("STAGE L1 OK event=bound family=%s fd=%d port=%hu", family_name,
				          (int)sock, port);
#endif
				return sock;
			}
		} while ((sockerrno == SEADDRINUSE || sockerrno == SEACCES) && retries-- > 0);

		const int err = sockerrno;
		JLOG_WARN("ICE_DIAG bind failed family=%s fd=%d range=%hu-%hu errno=%d", family_name,
		          (int)sock, config->port_begin, config->port_end, err);
#if defined(__ORBIS__) || defined(__PS4__)
		JLOG_WARN("STAGE L1 FAIL event=bind family=%s len=%u errno=%d(%s)", family_name,
		          (unsigned)addrlen, err, ice_errno_name(err));
#endif
	}

error:
	if (sock != INVALID_SOCKET)
		closesocket(sock);

	return INVALID_SOCKET;
}

socket_t udp_create_socket(const udp_socket_config_t *config) {
	// Obtain local Address
	struct addrinfo *ai_list = NULL;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	#if defined(__ORBIS__) || defined(__PS4__)
	hints.ai_family = AF_INET;
	JLOG_WARN("STAGE L1 begin requested_family=AF_INET bind=%s",
	          config->bind_address ? config->bind_address : "0.0.0.0");
	#else
	hints.ai_family = AF_UNSPEC;
	#endif
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	JLOG_WARN("ICE_DIAG getaddrinfo begin bind=%s", config->bind_address ? config->bind_address : "any");
	const int gai_rc = getaddrinfo(config->bind_address, "0", &hints, &ai_list);
	if (gai_rc != 0) {
		JLOG_ERROR("ICE_DIAG getaddrinfo failed rc=%d message=%s errno=%d", gai_rc,
		           gai_strerror(gai_rc), sockerrno);
#if defined(__ORBIS__) || defined(__PS4__)
		JLOG_WARN("STAGE L1 FAIL event=getaddrinfo family=AF_INET gai_rc=%d errno=%d", gai_rc,
		          sockerrno);
#endif
		return INVALID_SOCKET;
	}
	JLOG_WARN("ICE_DIAG getaddrinfo success");

	#if defined(__ORBIS__) || defined(__PS4__)
	const int families[1] = {AF_INET};
	const char *names[1] = {"IPv4"};
	const int family_count = 1;
	#else
	const int families[2] = {AF_INET6, AF_INET}; // Prefer IPv6
	const char *names[2] = {"IPv6", "IPv4"};
	const int family_count = 2;
	#endif
	for (int i = 0; i < family_count; ++i) {
		const struct addrinfo *ai = find_family(ai_list, families[i]);
		if (!ai)
			continue;

		JLOG_WARN("ICE_DIAG trying address family=%s", names[i]);
		socket_t sock = create_socket_for_addrinfo(config, ai);
		if (sock != INVALID_SOCKET) {
			freeaddrinfo(ai_list);
			return sock;
		}
	}

	JLOG_ERROR("ICE_DIAG all UDP address families failed");
#if defined(__ORBIS__) || defined(__PS4__)
	JLOG_WARN("STAGE L1 FAIL event=socket_or_bind family=AF_INET errno=%d(%s)", sockerrno,
	          ice_errno_name(sockerrno));
#endif
	freeaddrinfo(ai_list);
	return INVALID_SOCKET;
}

void udp_log_stats_before_close(socket_t sock) {
#if defined(__ORBIS__) || defined(__PS4__)
	JLOG_WARN("STAGE L3 OK event=datagram_final fd=%d received=%d first_stun_send_ms=%lld first_stun_response_ms=%lld",
	          (int)sock, __sync_fetch_and_add(&ice_datagram_count, 0),
	          (long long)__sync_fetch_and_add(&ice_first_stun_send_ms, 0),
	          (long long)__sync_fetch_and_add(&ice_first_stun_recv_ms, 0));
#else
	(void)sock;
#endif
}

int udp_recvfrom(socket_t sock, char *buffer, size_t size, addr_record_t *src) {
	while (true) {
		src->len = sizeof(src->addr);
		int recv_flags = 0;
#if defined(__ORBIS__) || defined(__PS4__)
		/* Orbis' socket flag is verified at creation, but use a per-call guard too:
		 * conn_mux_process drains this socket while holding the registry mutex. */
		recv_flags = MSG_DONTWAIT;
#endif
		int len = recvfrom(sock, buffer, (socklen_t)size, recv_flags,
		                   (struct sockaddr *)&src->addr, &src->len);
		if (len >= 0) {
			addr_unmap_inet6_v4mapped((struct sockaddr *)&src->addr, &src->len);
#if defined(__ORBIS__) || defined(__PS4__)
			if (len > 0) {
				const int count = __sync_add_and_fetch(&ice_datagram_count, 1);
				const bool is_stun = ice_is_stun_packet(buffer, (size_t)len);
				const int64_t now = current_timestamp();
				if (__sync_bool_compare_and_swap(&ice_first_datagram_logged, 0, 1)) {
					JLOG_WARN("STAGE L3 OK event=first_datagram bytes=%d stun=%d family=%d len=%u",
					          len, is_stun ? 1 : 0, (int)addr_storage_family(&src->addr),
					          (unsigned)src->len);
					ice_log_sockaddr("first_recvfrom_source", (const struct sockaddr *)&src->addr, src->len);
				}
				if (is_stun && __sync_bool_compare_and_swap(&ice_first_stun_recv_ms, 0, now)) {
					const int64_t sent = __sync_fetch_and_add(&ice_first_stun_send_ms, 0);
					JLOG_WARN("STAGE L3 OK event=first_stun_response elapsed_ms=%lld datagrams=%d",
					          (long long)(sent > 0 ? now - sent : -1), count);
				}
				if (count % 100 == 0)
					JLOG_WARN("STAGE L3 OK event=datagram_summary received=%d latest_bytes=%d latest_stun=%d",
					          count, len, is_stun ? 1 : 0);
			}
#endif

		} else if (sockerrno == SECONNRESET || sockerrno == SENETRESET ||
		           sockerrno == SECONNREFUSED) {
			// On Windows, if a UDP socket receives an ICMP port unreachable response after
			// sending a datagram, this error is stored, and the next call to recvfrom() returns
			// WSAECONNRESET (port unreachable) or WSAENETRESET (TTL expired).
			// Therefore, it may be ignored.
			JLOG_DEBUG("Ignoring %s returned by recvfrom",
			           sockerrno == SECONNRESET
			               ? "ECONNRESET"
			               : (sockerrno == SENETRESET ? "ENETRESET" : "ECONNREFUSED"));
			continue;
		}
		return len;
	}
}

int udp_sendto(socket_t sock, const char *data, size_t size, const addr_record_t *dst) {
#ifndef __linux__
	addr_record_t tmp = *dst;
	addr_set_sockaddr_len((struct sockaddr *)&tmp.addr);
	addr_record_t name;
	memset(&name, 0, sizeof(name));
	name.len = sizeof(name.addr);
	bool mapped_v4 = false;
	#if defined(__ORBIS__) || defined(__PS4__)
	const bool stun_packet = ice_is_stun_packet(data, size);
	ice_log_sockaddr("sendto_input_original", (const struct sockaddr *)&dst->addr, dst->len);
	ice_log_sockaddr("sendto_input", (const struct sockaddr *)&tmp.addr, tmp.len);
	#endif
	if (getsockname(sock, (struct sockaddr *)&name.addr, &name.len) == 0) {
		#if defined(__ORBIS__) || defined(__PS4__)
		ice_log_sockaddr("sendto_socket", (const struct sockaddr *)&name.addr, name.len);
		#endif
		if (addr_storage_family(&name.addr) == AF_INET6) {
			#if defined(__ORBIS__) || defined(__PS4__)
			JLOG_WARN("ICE_DIAG map_v4 begin input_family=%d input_len=%u sockaddr_storage=%u addr_record=%u",
			          (int)addr_storage_family(&tmp.addr), (unsigned)tmp.len,
			          (unsigned)sizeof(tmp.addr), (unsigned)sizeof(tmp));
			#endif
			mapped_v4 = addr_map_inet6_v4mapped(&tmp.addr, &tmp.len);
			#if defined(__ORBIS__) || defined(__PS4__)
			JLOG_WARN("ICE_DIAG map_v4 end mapped=%d output_family=%d output_len=%u",
			          mapped_v4 ? 1 : 0, (int)addr_storage_family(&tmp.addr), (unsigned)tmp.len);
			ice_log_sockaddr("sendto_output", (const struct sockaddr *)&tmp.addr, tmp.len);
			#endif
		}
	} else {
		JLOG_WARN("getsockname failed, errno=%d", sockerrno);
	}
	addr_set_sockaddr_len((struct sockaddr *)&tmp.addr);
	char destination_text[ADDR_MAX_STRING_LEN] = "?";
	addr_record_to_string(&tmp, destination_text, sizeof(destination_text));
	const int send_result =
	    sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&tmp.addr, tmp.len);
#if defined(__ORBIS__) || defined(__PS4__)
	if (stun_packet && send_result >= 0) {
		const int64_t now = current_timestamp();
		(void)__sync_bool_compare_and_swap(&ice_first_stun_send_ms, 0, now);
		JLOG_WARN("STAGE L2 OK event=stun_send bytes=%d socket_family=%d destination_family=%d destination_len=%u destination=%s",
		          send_result, (int)addr_storage_family(&name.addr),
		          (int)addr_storage_family(&tmp.addr), (unsigned)tmp.len, destination_text);
	}
	if (send_result < 0) {
		const int send_error = sockerrno;
		const struct sockaddr *send_addr = (const struct sockaddr *)&tmp.addr;
		int addr_len_field = -1;
		int mapped_prefix_ok = -1;
		int mapped_payload_nonzero = -1;
		int port_nonzero = -1;
		if (send_addr->sa_family == AF_INET && tmp.len >= sizeof(struct sockaddr_in)) {
			const struct sockaddr_in *sin = (const struct sockaddr_in *)send_addr;
			addr_len_field = sin->sin_len;
			port_nonzero = sin->sin_port != 0;
		} else if (send_addr->sa_family == AF_INET6 && tmp.len >= sizeof(struct sockaddr_in6)) {
			const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)send_addr;
			const uint8_t *bytes = (const uint8_t *)&sin6->sin6_addr;
			addr_len_field = sin6->sin6_len;
			mapped_prefix_ok = 1;
			for (int i = 0; i < 10; ++i)
				if (bytes[i] != 0) mapped_prefix_ok = 0;
			if (bytes[10] != 0xff || bytes[11] != 0xff) mapped_prefix_ok = 0;
			mapped_payload_nonzero = bytes[12] || bytes[13] || bytes[14] || bytes[15];
			port_nonzero = sin6->sin6_port != 0;
		}
		JLOG_WARN("ICE_DIAG sendto failure errno=%d(%s) socket_family=%d dst_family=%d dst_len=%u addr_len_field=%d mapped_v4=%d mapped_prefix_ok=%d mapped_payload_nonzero=%d port_nonzero=%d",
		          send_error, ice_errno_name(send_error), (int)addr_storage_family(&name.addr), (int)send_addr->sa_family,
		          (unsigned)tmp.len, addr_len_field, mapped_v4 ? 1 : 0,
		          mapped_prefix_ok, mapped_payload_nonzero, port_nonzero);
		if (stun_packet)
			JLOG_WARN("STAGE L2 FAIL event=stun_send socket_family=%d destination_family=%d destination_len=%u destination=%s errno=%d(%s)",
			          (int)addr_storage_family(&name.addr), (int)send_addr->sa_family,
			          (unsigned)tmp.len, destination_text, send_error, ice_errno_name(send_error));
	}
#endif
	return send_result;
#else
	return sendto(sock, data, size, 0, (const struct sockaddr *)&dst->addr, dst->len);
#endif
}

int udp_sendto_self(socket_t sock, const char *data, size_t size) {
	addr_record_t local;
	if (udp_get_local_addr(sock, AF_UNSPEC, &local) < 0)
		return -1;

	int ret;
#ifndef __linux__
	// We know local has the same address family as sock here
	addr_set_sockaddr_len((struct sockaddr *)&local.addr);
	ret = sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&local.addr, local.len);
#else
	ret = sendto(sock, data, size, 0, (const struct sockaddr *)&local.addr, local.len);
#endif
	if (ret >= 0 || addr_storage_family(&local.addr) != AF_INET6)
		return ret;

	// Fallback as IPv6 may be disabled on the loopback interface
	if (udp_get_local_addr(sock, AF_INET, &local) < 0)
		return -1;

#ifndef __linux__
	addr_map_inet6_v4mapped(&local.addr, &local.len);
	addr_set_sockaddr_len((struct sockaddr *)&local.addr);
	return sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&local.addr, local.len);
#else
	return sendto(sock, data, size, 0, (const struct sockaddr *)&local.addr, local.len);
#endif
}

int udp_set_diffserv(socket_t sock, int ds) {
#ifdef _WIN32
	// IP_TOS has been intentionally broken on Windows in favor of a convoluted proprietary
	// mechanism called qWave. Thank you Microsoft!
	// TODO: Investigate if DSCP can be still set directly without administrator flow configuration.
	(void)sock;
	(void)ds;
	JLOG_INFO("IP Differentiated Services are not supported on Windows");
	return -1;
#else
	addr_record_t name;
	name.len = sizeof(name.addr);
	if (getsockname(sock, (struct sockaddr *)&name.addr, &name.len) < 0) {
		JLOG_WARN("getsockname failed, errno=%d", sockerrno);
		return -1;
	}

	switch (addr_storage_family(&name.addr)) {
	case AF_INET:
#ifdef IP_TOS
		if (setsockopt(sock, IPPROTO_IP, IP_TOS, &ds, sizeof(ds)) < 0) {
			JLOG_WARN("Setting IP ToS failed, errno=%d", sockerrno);
			return -1;
		}
		return 0;
#else
		JLOG_INFO("Setting IP ToS is not supported");
		return -1;
#endif

	case AF_INET6:
#ifdef IPV6_TCLASS
		if (setsockopt(sock, IPPROTO_IPV6, IPV6_TCLASS, &ds, sizeof(ds)) < 0) {
			JLOG_WARN("Setting IPv6 traffic class failed, errno=%d", sockerrno);
			return -1;
		}
#ifdef IP_TOS
		// Attempt to also set IP_TOS for IPv4, in case the system requires it
		setsockopt(sock, IPPROTO_IP, IP_TOS, &ds, sizeof(ds));
#endif
		return 0;
#else
		JLOG_INFO("Setting IPv6 traffic class is not supported");
		return -1;
#endif
	default:
		return -1;
	}
#endif
}

uint16_t udp_get_port(socket_t sock) {
	addr_record_t record;
	if (udp_get_bound_addr(sock, &record) < 0)
		return 0;
	return addr_get_port((struct sockaddr *)&record.addr);
}

int udp_get_bound_addr(socket_t sock, addr_record_t *record) {
	record->len = sizeof(record->addr);
	if (getsockname(sock, (struct sockaddr *)&record->addr, &record->len)) {
		JLOG_WARN("getsockname failed, errno=%d", sockerrno);
		return -1;
	}
	return 0;
}

int udp_get_local_addr(socket_t sock, int family_hint, addr_record_t *record) {
	if (udp_get_bound_addr(sock, record) < 0)
		return -1;

	// If the socket is bound to a particular address, return it
	if (!addr_is_any((struct sockaddr *)&record->addr)) {
		if (addr_storage_family(&record->addr) == AF_INET && family_hint == AF_INET6)
			addr_map_inet6_v4mapped(&record->addr, &record->len);

		return 0;
	}

	if (addr_storage_family(&record->addr) == AF_INET6 && family_hint == AF_INET) {
		// Generate an IPv4 instead (socket is listening to any IPv4 or IPv6)

		uint16_t port = addr_get_port((struct sockaddr *)&record->addr);
		if (port == 0)
			return -1;

		struct sockaddr_in *sin = (struct sockaddr_in *)&record->addr;
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		record->len = sizeof(*sin);
		addr_set_sockaddr_len((struct sockaddr *)sin);
	}

	switch (addr_storage_family(&record->addr)) {
	case AF_INET: {
		struct sockaddr_in *sin = (struct sockaddr_in *)&record->addr;
		const uint8_t localhost[4] = {127, 0, 0, 1};
		memcpy(&sin->sin_addr, localhost, 4);
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&record->addr;
		uint8_t *b = (uint8_t *)&sin6->sin6_addr;
		memset(b, 0, 15);
		b[15] = 0x01; // localhost
		break;
	}
	default:
		// Ignore
		break;
	}
	addr_set_sockaddr_len((struct sockaddr *)&record->addr);

	if (addr_storage_family(&record->addr) == AF_INET && family_hint == AF_INET6)
		addr_map_inet6_v4mapped(&record->addr, &record->len);

	return 0;
}

// Helper function to check if a similar address already exists in records
// This function ignores the port
static int has_duplicate_addr(struct sockaddr *addr, const addr_record_t *records, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		const addr_record_t *record = records + i;
		if (addr_storage_family(&record->addr) == addr->sa_family) {
			switch (addr->sa_family) {
			case AF_INET: {
				// For IPv4, compare the whole address
				const struct sockaddr_in *rsin = (const struct sockaddr_in *)&record->addr;
				const struct sockaddr_in *asin = (const struct sockaddr_in *)addr;
				if (memcmp(&rsin->sin_addr, &asin->sin_addr, 4) == 0)
					return true;
				break;
			}
			case AF_INET6: {
				// For IPv6, compare the network part only
				const struct sockaddr_in6 *rsin6 = (const struct sockaddr_in6 *)&record->addr;
				const struct sockaddr_in6 *asin6 = (const struct sockaddr_in6 *)addr;
				if (memcmp(&rsin6->sin6_addr, &asin6->sin6_addr, 8) == 0) // compare first 64 bits
					return true;
				break;
			}
			}
		}
	}
	return false;
}

#if !defined(_WIN32) && defined(NO_IFADDRS)
// Helper function to get the IPv6 address of the default interface
static int get_local_default_inet6(uint16_t port, struct sockaddr_in6 *result) {
	const char *dummy_host = "2001:db8::1"; // dummy public unreachable address
	const uint16_t dummy_port = 9;          // discard port

	struct sockaddr_in6 sin6;
	memset(&sin6, 0, sizeof(sin6));
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons(dummy_port);
	addr_set_sockaddr_len((struct sockaddr *)&sin6);
	if (inet_pton(AF_INET6, dummy_host, &sin6.sin6_addr) != 1)
		return -1;

	socket_t sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return -1;

	addr_set_sockaddr_len((struct sockaddr *)&sin6);
	if (connect(sock, (const struct sockaddr *)&sin6, sizeof(sin6)))
		goto error;

	socklen_t result_len = sizeof(*result);
	if (getsockname(sock, (struct sockaddr *)result, &result_len))
		goto error;

	if (result_len != sizeof(*result))
		goto error;

	addr_set_port((struct sockaddr *)result, port);
	closesocket(sock);
	return 0;

error:
	closesocket(sock);
	return -1;
}
#endif

int udp_get_addrs(socket_t sock, addr_record_t *records, size_t count) {
	addr_record_t bound;
	if (udp_get_bound_addr(sock, &bound) < 0) {
		JLOG_ERROR("Getting UDP bound address failed");
		return -1;
	}

	if (!addr_is_any((struct sockaddr *)&bound.addr)) {
		if (count > 0)
			records[0] = bound;

		return 1;
	}

	uint16_t port = addr_get_port((struct sockaddr *)&bound.addr);

	// RFC 8445 5.1.1.1. Host Candidates:
	// Addresses from a loopback interface MUST NOT be included in the candidate addresses.
	// [...]
	// If gathering one or more host candidates that correspond to an IPv6 address that was
	// generated using a mechanism that prevents location tracking [RFC7721], host candidates
	// that correspond to IPv6 addresses that do allow location tracking, are configured on the
	// same interface, and are part of the same network prefix MUST NOT be gathered. Similarly,
	// when host candidates corresponding to an IPv6 address generated using a mechanism that
	// prevents location tracking are gathered, then host candidates corresponding to IPv6
	// link-local addresses [RFC4291] MUST NOT be gathered. The IPv6 default address selection
	// specification [RFC6724] specifies that temporary addresses [RFC4941] are to be preferred
	// over permanent addresses.

	// IPv6 IIDs generated by modern systems are opaque so there is no way to reliably differentiate
	// privacy-enabled IPv6 addresses here. Therefore, we hope the preferred addresses are listed
	// first, and we never list link-local addresses.

	addr_record_t *current = records;
	addr_record_t *end = records + count;
	int ret = 0;

#if JUICE_ENABLE_LOCALHOST_ADDRESS
	// Add localhost for test purposes
	addr_record_t local;
	if (addr_storage_family(&bound.addr) == AF_INET6 && udp_get_local_addr(sock, AF_INET6, &local) == 0) {
		++ret;
		if (current != end) {
			*current = local;
			++current;
		}
	}
	if (udp_get_local_addr(sock, AF_INET, &local) == 0) {
		++ret;
		if (current != end) {
			*current = local;
			++current;
		}
	}
#endif

#ifdef _WIN32
	char buf[4096];
	DWORD len = 0;
	if (WSAIoctl(sock, SIO_ADDRESS_LIST_QUERY, NULL, 0, buf, sizeof(buf), &len, NULL, NULL)) {
		JLOG_ERROR("WSAIoctl with SIO_ADDRESS_LIST_QUERY failed, errno=%d", WSAGetLastError());
		return -1;
	}

	SOCKET_ADDRESS_LIST *list = (SOCKET_ADDRESS_LIST *)buf;
	for (int i = 0; i < list->iAddressCount; ++i) {
		struct sockaddr *sa = list->Address[i].lpSockaddr;
		socklen_t len = list->Address[i].iSockaddrLength;
		if ((sa->sa_family == AF_INET ||
		     (sa->sa_family == AF_INET6 && addr_storage_family(&bound.addr) == AF_INET6)) &&
		    !addr_is_local(sa)) {
			if (!has_duplicate_addr(sa, records, current - records)) {
				++ret;
				if (current != end) {
					memcpy(&current->addr, sa, len);
					current->len = len;
					addr_unmap_inet6_v4mapped((struct sockaddr *)&current->addr, &current->len);
					addr_set_port((struct sockaddr *)&current->addr, port);
					++current;
				}
			}
		}
	}
#else // POSIX
#ifndef NO_IFADDRS
	struct ifaddrs *ifas;
	if (getifaddrs(&ifas)) {
		const int err = sockerrno;
		JLOG_ERROR("getifaddrs failed, errno=%d", err);
#if defined(__ORBIS__) || defined(__PS4__)
		/* Orbis exposes interface addresses through sceNetGetIfList; its
		 * getifaddrs shim can return EPROTONOSUPPORT even with networking up. */
		int added = 0;
		for (int if_name = SCE_NET_IF_NAME_ETH0; if_name < SCE_NET_IF_NAME_MAX; ++if_name) {
			struct OrbisNetIfEntry entry;
			memset(&entry, 0, sizeof(entry));
			int rc = sceNetGetIfList((enum OrbisNetIfName)if_name, &entry, 1);
			uint32_t ip = (uint32_t)entry.IPAddress;
			if (rc != 1 || ip == 0) {
				JLOG_WARN("ICE_DIAG Orbis interface enum=%d rc=%d ip=0x%08x", if_name, rc, ip);
				continue;
			}

			struct sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_len = sizeof(addr);
			addr.sin_family = AF_INET;
			addr.sin_port = htons(port);
			addr.sin_addr.s_addr = ip;
			if (addr_is_local((struct sockaddr *)&addr) ||
			    has_duplicate_addr((struct sockaddr *)&addr, records, added)) {
				JLOG_WARN("ICE_DIAG Orbis interface enum=%d ignored local_or_duplicate=1", if_name);
				continue;
			}

			if (current != end) {
				memcpy(&current->addr, &addr, sizeof(addr));
				current->len = sizeof(addr);
				++current;
			}
			++added;
			char ip_text[INET_ADDRSTRLEN] = "?";
			inet_ntop(AF_INET, &addr.sin_addr, ip_text, sizeof(ip_text));
			JLOG_WARN("ICE_DIAG Orbis host address enum=%d ip=%s port=%hu", if_name,
			          ip_text, port);
		}
		JLOG_WARN("ICE_DIAG Orbis interface fallback errno=%d candidates=%d", err, added);
		return added;
#else
		return -1;
#endif
	}

	for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
		unsigned int flags = ifa->ifa_flags;
		if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK))
			continue;
		if (strcmp(ifa->ifa_name, "docker0") == 0)
			continue;

		struct sockaddr *sa = ifa->ifa_addr;
		socklen_t len;
		if (sa &&
		    (sa->sa_family == AF_INET ||
		     (sa->sa_family == AF_INET6 && addr_storage_family(&bound.addr) == AF_INET6)) &&
		    !addr_is_local(sa) && (len = addr_get_len(sa)) > 0) {
			if (!has_duplicate_addr(sa, records, current - records)) {
				++ret;
				if (current != end) {
					memcpy(&current->addr, sa, len);
					current->len = len;
					addr_set_port((struct sockaddr *)&current->addr, port);
					++current;
				}
			}
		}
	}

	freeifaddrs(ifas);

#else // NO_IFADDRS defined
	char buf[4096];
	struct ifconf ifc;
	memset(&ifc, 0, sizeof(ifc));
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;

	if (ioctlsocket(sock, SIOCGIFCONF, &ifc)) {
		JLOG_ERROR("ioctl for SIOCGIFCONF failed, errno=%d", sockerrno);
		return -1;
	}

	bool ifconf_has_inet6 = false;
	int n = ifc.ifc_len / sizeof(struct ifreq);
	for (int i = 0; i < n; ++i) {
		struct ifreq *ifr = ifc.ifc_req + i;
		struct sockaddr *sa = &ifr->ifr_addr;
		if (sa->sa_family == AF_INET6)
			ifconf_has_inet6 = true;

		socklen_t len;
		if ((sa->sa_family == AF_INET ||
		     (sa->sa_family == AF_INET6 && addr_storage_family(&bound.addr) == AF_INET6)) &&
		    !addr_is_local(sa) && (len = addr_get_len(sa)) > 0) {
			if (!has_duplicate_addr(sa, records, current - records)) {
				++ret;
				if (current != end) {
					memcpy(&current->addr, sa, len);
					current->len = len;
					addr_set_port((struct sockaddr *)&current->addr, port);
					++current;
				}
			}
		}
	}

	if (!ifconf_has_inet6 && addr_storage_family(&bound.addr) == AF_INET6) {
		struct sockaddr_in6 sin6;
		if (get_local_default_inet6(port, &sin6) == 0) {
			if (!addr_is_local((const struct sockaddr *)&sin6)) {
				++ret;
				if (current != end) {
					memcpy(&current->addr, &sin6, sizeof(sin6));
					current->len = sizeof(sin6);
					++current;
				}
			}
		}
	}
#endif
#endif

	return ret;
}
