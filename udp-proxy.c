#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <systemd/sd-daemon.h>
#include <systemd/sd-journal.h>
#include <systemd/sd-event.h>

typedef union {
	struct sockaddr sa;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
} sockaddr_union;

static int udp_forward(struct msghdr *msg, struct sockaddr *dstaddr) {
	auto out = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
	if (out < 0) {
		sd_journal_print(LOG_ERR, "Error creating outbound socket. (#%d %s)\n", errno, strerror(errno));
		return -2;
	}

	int n = 1;
	if (setsockopt(out, SOL_IP, IP_TRANSPARENT, &n, sizeof(int)) != 0) {
		sd_journal_print(LOG_ERR, "Error setting transparency towards destination. (#%d %s)\n", errno, strerror(errno));
		close(out);
		return -3;
	}

	// spoof the sender
	if (bind(out, (struct sockaddr *)msg->msg_name, sizeof(struct sockaddr)) < 0) {
		sd_journal_print(LOG_ERR, "Error binding to destination. (#%d %s)\n", errno, strerror(errno));
		close(out);
		return -4;
	}

	ssize_t ret = sendto(out, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, 0,
		dstaddr, sizeof(*dstaddr));
	if (ret <= 0) {
		sd_journal_print(LOG_ERR, "Error sending to destination. (#%d %s)\n", errno, strerror(errno));
		close(out);
		return -5;
	}

	close(out);
	return 0;
}

static int udp_receive(sd_event_source *es, int fd, uint32_t revents, void *userdata) {
	ssize_t num_octets = 0;
	if (ioctl(fd, FIONREAD, &num_octets) < 0) { // usually far less than 64k, more like 1.4k
		return -errno;
	}
	void *buffer = alloca(num_octets + 512); // Add some offset to account for overhead.

	struct msghdr msg;
	struct iovec iov[1];
	sockaddr_union sa;
	char cntrlbuf[64];
	memset(&sa, 0, sizeof(sa));
	memset(cntrlbuf, 0, sizeof(cntrlbuf));
	iov[0].iov_base = buffer;
	iov[0].iov_len = num_octets;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_name = &sa.sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_control = cntrlbuf;
	msg.msg_controllen = sizeof(cntrlbuf);

	/* receive */
	num_octets = recvmsg(fd, &msg, 0);
	if (num_octets < 0) {
		if (errno == EAGAIN) {
			return 0;
		}
		sd_journal_print(LOG_ERR, "Error calling recvmsg(). err (#%d %s)\n", errno, strerror(errno));
		return -errno;
	}
	msg.msg_iov[0].iov_len = num_octets;

	/* forward */
	struct sockaddr *dstaddr = userdata;
	udp_forward(&msg, dstaddr);

	return 0;
}

static int set_nonblocking(int fd) {
	int opt = fcntl(fd, F_GETFL, 0);
	if (opt == -1) {
		opt = 0;
	}
	if ((opt | O_NONBLOCK) == opt) {
		return 0;
	}
	int rc = fcntl(fd, F_SETFL, opt | O_NONBLOCK);
	return rc;
}

static int hostnametoaddr(sockaddr_union *dstaddr, const char *hostname, int preferred_sa_family) {
	struct hostent *hostinfo = gethostbyname2(hostname, preferred_sa_family);
	if (hostinfo == NULL) {
		hostinfo = gethostbyname(hostname);
		if (hostinfo == NULL) {
			return -1;
		}
	}
	dstaddr->sa.sa_family = hostinfo->h_addrtype;

	if (hostinfo->h_addrtype == AF_INET6) {
		memcpy(&(dstaddr->in6.sin6_addr.s6_addr), hostinfo->h_addr,
			sizeof(dstaddr->in6.sin6_addr.s6_addr));
	} else {
		memcpy(&(dstaddr->in.sin_addr.s_addr), hostinfo->h_addr,
			sizeof(dstaddr->in.sin_addr.s_addr));
	}

	return 0;
}

static int fill_dstaddr(sockaddr_union *dstaddr, const sockaddr_union srcaddr, const char *arg_remote_host) {
	const char *node, *service;

	auto port = srcaddr.in.sin_port;
	if (srcaddr.sa.sa_family == AF_INET6) {
		port = srcaddr.in6.sin6_port;
	}
	service = strrchr(arg_remote_host, ':');
	if (service) { // use the given port
		node = strndupa(arg_remote_host, service - arg_remote_host);
		service++;
		auto portno = atoi(service);
		port = htons(portno);
	} else { // stick with the same port
		node = arg_remote_host;
	}

	if (hostnametoaddr(dstaddr, node, srcaddr.sa.sa_family) < 0) {
		return -1;
	}

	if (dstaddr->sa.sa_family == AF_INET6) {
		dstaddr->in6.sin6_port = port;
	} else {
		dstaddr->in.sin_port = port;
	}

	return 0;
}

int main(int argc, char *argv[]) {
	int n_systemd_sockets = sd_listen_fds(0);
	if ((n_systemd_sockets + 1) != argc) {
		sd_journal_print(LOG_ERR, "Mismatch in received sockets %d != %d destinations.", n_systemd_sockets, (argc - 1));
		return 1;
	}

	int exit_code = 0;
	sd_event_source *event_source = NULL;
	sd_event *event = NULL;

	if (sd_event_default(&event) < 0) {
		sd_journal_print(LOG_DEBUG, "Cannot instantiate the event loop.");
		exit_code = 72;
		goto finish;
	}

	/* Register events without a callback, to trigger exit from the main event-loop call. */
	sigset_t ss;
	if (sigemptyset(&ss) < 0 || sigaddset(&ss, SIGTERM) < 0 || sigaddset(&ss, SIGINT) < 0) {
		exit_code = errno;
		goto finish;
	}
	if (sigprocmask(SIG_BLOCK, &ss, NULL) < 0) {
		exit_code = errno;
		goto finish;
	}
	if (sd_event_add_signal(event, NULL, SIGTERM, NULL, NULL) < 0 || sd_event_add_signal(event, NULL, SIGINT, NULL, NULL) < 0) {
		exit_code = 73;
		goto finish;
	}

	/* Pull the watchdog, if requested. */
	if (sd_event_set_watchdog(event, true) < 0) {
		sd_journal_print(LOG_DEBUG, "Cannot pull the watchdog.");
		exit_code = 74;
		goto finish;
	}

	/* Setup the sockets. */
	for (int i = 0; i < n_systemd_sockets; i++) {
		auto fd = (SD_LISTEN_FDS_START + i);

/*		int r = sd_is_socket(fd, 0, SOCK_DGRAM, 1);
		if (r < 0) {
			sd_journal_print(LOG_ERR, "Failed to determine socket type.");
			exit_code = 4;
			goto finish;
		} else if (r == 0) {
			sd_journal_print(LOG_ERR, "Passed in socket is not a datagram socket.");
			exit_code = 5;
			goto finish;
		} */

		// set to non-blocking
		if (set_nonblocking(fd) < 0) {
			sd_journal_print(LOG_CRIT, "Cannot set the socket to nonblocking: %d", i);
			exit_code = 10;
			goto finish;
		}

		// get the destination
		sockaddr_union *dstaddr = alloca(sizeof(sockaddr_union));
		memset(dstaddr, 0, sizeof(sockaddr_union));

		sockaddr_union addr;
		memset(&addr, 0, sizeof(addr));
		socklen_t len = sizeof(addr);
		getsockname(fd, (struct sockaddr *) &addr, &len);

		if (fill_dstaddr(dstaddr, addr, argv[i+1]) < 0) {
			sd_journal_print(LOG_ERR, "Cannot get the destination for socket: %d", i);
			exit_code = 6;
			goto finish;
		}

		// register
		if (sd_event_add_io(event, &event_source, fd, EPOLLIN, udp_receive, dstaddr) < 0) {
			sd_journal_print(LOG_CRIT, "event_add_io failed for socket no: %d", i);
			exit_code = 72;
			goto finish;
		}
	}

	/* Block on main event-loop call. */
	sd_journal_print(LOG_INFO, "Written by W. Mark Kubacki <wmark@hurrikane.de> https://github.com/wmark");
	sd_journal_print(LOG_INFO, "Done setting everything up. Serving.");
	(void) sd_notifyf(false, "READY=1\n" "STATUS=Serving incoming datagrams.");
	int r = sd_event_loop(event);
	if (r < 0) {
		sd_journal_print(LOG_ERR, "Failure: %s\n", strerror(-r));
		exit_code = -r;
	}

finish:
	sd_journal_print(LOG_DEBUG, "Freeing reverences to event-source and the event-loop.");
	event_source = sd_event_source_unref(event_source);
	event = sd_event_unref(event);

	sd_journal_print(LOG_INFO, "Closing sockets before exiting.");
	for (int i = 0; i < argc; ++i) {
		// fdclean(SD_LISTEN_FDS_START + i); -- No needed here.
		close(SD_LISTEN_FDS_START + i);
	}

	return exit_code;
}