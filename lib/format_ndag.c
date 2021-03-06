
#define _GNU_SOURCE

#include "config.h"
#include "common.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "format_erf.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "format_ndag.h"

#define NDAG_IDLE_TIMEOUT (600)
#define ENCAP_BUFSIZE (10000)
#define CTRL_BUF_SIZE (10000)
#define ENCAP_BUFFERS (1000)

#define RECV_BATCH_SIZE (50)

#define FORMAT_DATA ((ndag_format_data_t *)libtrace->format_data)

static struct libtrace_format_t ndag;

volatile int ndag_paused = 0;

typedef struct monitor {
        uint16_t monitorid;
        uint64_t laststart;
} ndag_monitor_t;


typedef struct streamsource {
        uint16_t monitor;
        char *groupaddr;
        char *localiface;
        uint16_t port;
} streamsource_t;

typedef struct streamsock {
        char *groupaddr;
        int sock;
        struct addrinfo *srcaddr;
        uint16_t port;
        uint32_t expectedseq;
        ndag_monitor_t *monitorptr;
        char **saved;
        char *nextread;
        int nextreadind;
        int nextwriteind;
        int savedsize[ENCAP_BUFFERS];
        uint32_t startidle;
        uint64_t recordcount;

        int bufavail;

#if HAVE_RECVMMSG
        struct mmsghdr mmsgbufs[RECV_BATCH_SIZE];
#else
	struct msghdr singlemsg;
#endif

} streamsock_t;

typedef struct recvstream {
        streamsock_t *sources;
        uint16_t sourcecount;
        libtrace_message_queue_t mqueue;
        int threadindex;
        ndag_monitor_t *knownmonitors;
        uint16_t monitorcount;

        uint64_t dropped_upstream;
        uint64_t missing_records;
        uint64_t received_packets;
} recvstream_t;

typedef struct ndag_format_data {
        char *multicastgroup;
        char *portstr;
        char *localiface;
        uint16_t nextthreadid;
        recvstream_t *receivers;

        pthread_t controlthread;
        libtrace_message_queue_t controlqueue;
} ndag_format_data_t;

enum {
        NDAG_CLIENT_HALT = 0x01,
        NDAG_CLIENT_RESTARTED = 0x02,   // redundant
        NDAG_CLIENT_NEWGROUP = 0x03
};

typedef struct ndagreadermessage {
        uint8_t type;
        streamsource_t contents;
} ndag_internal_message_t;


static inline int seq_cmp(uint32_t seq_a, uint32_t seq_b) {

        /* Calculate seq_a - seq_b, taking wraparound into account */
        if (seq_a == seq_b) return 0;

        if (seq_a > seq_b) {
                return (int) (seq_a - seq_b);
        }

        /* -1 for the wrap and another -1 because we don't use zero */
        return (int) (0xffffffff - ((seq_b - seq_a) - 2));
}

static uint8_t check_ndag_header(char *msgbuf, uint32_t msgsize) {
        ndag_common_t *header = (ndag_common_t *)msgbuf;

        if (msgsize < sizeof(ndag_common_t)) {
                fprintf(stderr,
                        "nDAG message does not have a complete nDAG header.\n");
                return 0;
        }

        if (ntohl(header->magic) != NDAG_MAGIC_NUMBER) {
                fprintf(stderr,
                        "nDAG message does not have a valid magic number.\n");
                return 0;
        }

        if (header->version > NDAG_EXPORT_VERSION || header->version == 0) {
                fprintf(stderr,
                        "nDAG message has an invalid header version: %u\n",
                                header->version);
                return 0;
        }

        return header->type;
}

static int join_multicast_group(char *groupaddr, char *localiface,
        char *portstr, uint16_t portnum, struct addrinfo **srcinfo) {

        struct addrinfo hints;
        struct addrinfo *gotten;
        struct addrinfo *group;
        unsigned int interface;
        char pstr[16];
        struct group_req greq;
        int bufsize;

        int sock;

        if (portstr == NULL) {
                snprintf(pstr, 15, "%u", portnum);
                portstr = pstr;
        }

        interface = if_nametoindex(localiface);
        if (interface == 0) {
                fprintf(stderr, "Failed to lookup interface %s -- %s\n",
                                localiface, strerror(errno));
                return -1;
        }

        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;
        hints.ai_protocol = 0;

        if (getaddrinfo(NULL, portstr, &hints, &gotten) != 0) {
                fprintf(stderr,
                        "Call to getaddrinfo failed for NULL:%s -- %s\n",
                                portstr, strerror(errno));
                return -1;
        }

        if (getaddrinfo(groupaddr, NULL, &hints, &group) != 0) {
                fprintf(stderr, "Call to getaddrinfo failed for %s -- %s\n",
                                groupaddr, strerror(errno));
                return -1;
        }

        *srcinfo = gotten;
        sock = socket(gotten->ai_family, gotten->ai_socktype, 0);
        if (sock < 0) {
                fprintf(stderr,
                        "Failed to create multicast socket for %s:%s -- %s\n",
                                groupaddr, portstr, strerror(errno));
                goto sockcreateover;
        }

        if (bind(sock, (struct sockaddr *)gotten->ai_addr, gotten->ai_addrlen) < 0)
        {
                fprintf(stderr,
                        "Failed to bind to multicast socket %s:%s -- %s\n",
                                groupaddr, portstr, strerror(errno));
                close(sock);
                sock = -1;
                goto sockcreateover;
        }

        greq.gr_interface = interface;
        memcpy(&(greq.gr_group), group->ai_addr, group->ai_addrlen);

        if (setsockopt(sock, IPPROTO_IP, MCAST_JOIN_GROUP, &greq,
                        sizeof(greq)) < 0) {
                fprintf(stderr,
                        "Failed to join multicast group %s:%s -- %s\n",
                                groupaddr, portstr, strerror(errno));
                close(sock);
                sock = -1;
                goto sockcreateover;
        }

        bufsize = 16 * 1024 * 1024;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize,
                                (socklen_t)sizeof(int)) < 0) {

                fprintf(stderr,
                        "Failed to increase buffer size for multicast group %s:%s -- %s\n",
                                groupaddr, portstr, strerror(errno));
                close(sock);
                sock = -1;
                goto sockcreateover;
        }

sockcreateover:
        freeaddrinfo(group);
        return sock;
}


static int ndag_init_input(libtrace_t *libtrace) {

        char *scan = NULL;
        char *next = NULL;

        libtrace->format_data = (ndag_format_data_t *)malloc(
                        sizeof(ndag_format_data_t));

        FORMAT_DATA->multicastgroup = NULL;
        FORMAT_DATA->portstr = NULL;
        FORMAT_DATA->localiface = NULL;
        FORMAT_DATA->nextthreadid = 0;
        FORMAT_DATA->receivers = NULL;

        scan = strchr(libtrace->uridata, ',');
        if (scan == NULL) {
                trace_set_err(libtrace, TRACE_ERR_BAD_FORMAT,
                        "Bad ndag URI. Should be ndag:<interface>,<multicast group>,<port number>");
                return -1;
        }
        FORMAT_DATA->localiface = strndup(libtrace->uridata,
                        (size_t)(scan - libtrace->uridata));
        next = scan + 1;

        scan = strchr(next, ',');
        if (scan == NULL) {
                FORMAT_DATA->portstr = strdup("9001");
                FORMAT_DATA->multicastgroup = strdup(next);
        } else {
                FORMAT_DATA->multicastgroup = strndup(next, (size_t)(scan - next));

                FORMAT_DATA->portstr = strdup(scan + 1);
        }
        return 0;
}

static void new_group_alert(libtrace_t *libtrace, uint16_t threadid,
                uint16_t portnum, uint16_t monid) {

        ndag_internal_message_t alert;

        alert.type = NDAG_CLIENT_NEWGROUP;
        alert.contents.groupaddr = FORMAT_DATA->multicastgroup;
        alert.contents.localiface = FORMAT_DATA->localiface;
        alert.contents.port = portnum;
        alert.contents.monitor = monid;

        libtrace_message_queue_put(&(FORMAT_DATA->receivers[threadid].mqueue),
                        (void *)&alert);

}
        
static int ndag_parse_control_message(libtrace_t *libtrace, char *msgbuf,
                int msgsize, uint16_t *ptmap) {

        int i;
        ndag_common_t *ndaghdr = (ndag_common_t *)msgbuf;
        uint8_t msgtype;

        msgtype = check_ndag_header(msgbuf, (uint32_t)msgsize);
        if (msgtype == 0) {
                return -1;
        }

        msgsize -= sizeof(ndag_common_t);
        if (msgtype == NDAG_PKT_BEACON) {
                /* If message is a beacon, make sure every port included in the
                 * beacon is assigned to a receive thread.
                 */
                uint16_t *ptr, numstreams;

                if ((uint32_t)msgsize < sizeof(uint16_t)) {
                        fprintf(stderr, "Malformed beacon (missing number of streams).\n");
                        return -1;
                }

                ptr = (uint16_t *)(msgbuf + sizeof(ndag_common_t));
                numstreams = ntohs(*ptr);
                ptr ++;

                if ((uint32_t)msgsize != ((numstreams + 1) * sizeof(uint16_t)))
                {
                        fprintf(stderr, "Malformed beacon (length doesn't match number of streams).\n");
                        fprintf(stderr, "%u %u\n", msgsize, numstreams);
                        return -1;
                }

                for (i = 0; i < numstreams; i++) {
                        uint16_t streamport = ntohs(*ptr);

                        if (ptmap[streamport] == 0xffff) {
                                new_group_alert(libtrace,
                                        FORMAT_DATA->nextthreadid, streamport,
                                        ntohs(ndaghdr->monitorid));

                                ptmap[streamport] = FORMAT_DATA->nextthreadid;

                                if (libtrace->perpkt_thread_count == 0) {
                                        FORMAT_DATA->nextthreadid = 0;
                                } else {
                                        FORMAT_DATA->nextthreadid =
                                                ((FORMAT_DATA->nextthreadid + 1) % libtrace->perpkt_thread_count);
                                }
                        }

                        ptr ++;
                }
        } else {
                fprintf(stderr,
                        "Unexpected message type on control channel: %u\n",
                         msgtype);
                return -1;
        }

        return 0;

}

static void *ndag_controller_run(void *tdata) {

        libtrace_t *libtrace = (libtrace_t *)tdata;
        uint16_t ptmap[65536];
        int sock = -1;
        struct addrinfo *receiveaddr = NULL;
        fd_set listening;
        struct timeval timeout;

        /* ptmap is a dirty hack to allow us to quickly check if we've already
         * assigned a stream to a thread.
         */
        memset(ptmap, 0xff, 65536 * sizeof(uint16_t));

        sock = join_multicast_group(FORMAT_DATA->multicastgroup,
                        FORMAT_DATA->localiface, FORMAT_DATA->portstr, 0,
                        &receiveaddr);
        if (sock == -1) {
                trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
                        "Unable to join multicast group for nDAG control channel");
                trace_interrupt();
        }

        ndag_paused = 0;
        while ((is_halted(libtrace) == -1) && !ndag_paused) {
                int ret;
                char buf[CTRL_BUF_SIZE];

                FD_ZERO(&listening);
                FD_SET(sock, &listening);

                timeout.tv_sec = 0;
                timeout.tv_usec = 500000;

                ret = select(sock + 1, &listening, NULL, NULL, &timeout);
                if (ret < 0) {
                        fprintf(stderr, "Error while waiting for nDAG control messages: %s\n", strerror(errno));
                        break;
                }

                if (!FD_ISSET(sock, &listening)) {
                        continue;
                }

                ret = recvfrom(sock, buf, CTRL_BUF_SIZE, 0,
                                receiveaddr->ai_addr,
                                &(receiveaddr->ai_addrlen));
                if (ret < 0) {
                        fprintf(stderr, "Error while receiving nDAG control message: %s\n", strerror(errno));
                        break;
                }

                if (ret == 0) {
                        break;
                }

                if (ndag_parse_control_message(libtrace, buf, ret, ptmap) < 0) {
                        fprintf(stderr, "Error while parsing nDAG control message.\n");
                        continue;
                }
        }

        if (sock >= 0) {
                close(sock);
        }

        /* Control channel has fallen over, should probably encourage libtrace
         * to halt the receiver threads as well.
         */
        if (!is_halted(libtrace)) {
                trace_interrupt();
        }

        pthread_exit(NULL);
}

static int ndag_start_threads(libtrace_t *libtrace, uint32_t maxthreads)
{
        int ret;
        uint32_t i;
        /* Configure the set of receiver threads */

        if (FORMAT_DATA->receivers == NULL) {
                /* What if the number of threads changes between a pause and
                 * a restart? Can this happen? */
                FORMAT_DATA->receivers = (recvstream_t *)
                                malloc(sizeof(recvstream_t) * maxthreads);
        }

        for (i = 0; i < maxthreads; i++) {
                FORMAT_DATA->receivers[i].sources = NULL;
                FORMAT_DATA->receivers[i].sourcecount = 0;
                FORMAT_DATA->receivers[i].knownmonitors = NULL;
                FORMAT_DATA->receivers[i].monitorcount = 0;
                FORMAT_DATA->receivers[i].threadindex = i;
                FORMAT_DATA->receivers[i].dropped_upstream = 0;
                FORMAT_DATA->receivers[i].received_packets = 0;
                FORMAT_DATA->receivers[i].missing_records = 0;

                libtrace_message_queue_init(&(FORMAT_DATA->receivers[i].mqueue),
                                sizeof(ndag_internal_message_t));
        }

        /* Start the controller thread */
        /* TODO consider affinity of this thread? */

        ret = pthread_create(&(FORMAT_DATA->controlthread), NULL,
                        ndag_controller_run, libtrace);
        if (ret != 0) {
                return -1;
        }
        return maxthreads;
}

static int ndag_start_input(libtrace_t *libtrace) {
        return ndag_start_threads(libtrace, 1);
}

static int ndag_pstart_input(libtrace_t *libtrace) {
        if (ndag_start_threads(libtrace, libtrace->perpkt_thread_count) ==
                        libtrace->perpkt_thread_count)
                return 0;
        return -1;
}

static void halt_ndag_receiver(recvstream_t *receiver) {
        int j, i;
        libtrace_message_queue_destroy(&(receiver->mqueue));

        if (receiver->sources == NULL)
                return;
        for (i = 0; i < receiver->sourcecount; i++) {
                streamsock_t src = receiver->sources[i];
                if (src.saved) {
                        for (j = 0; j < ENCAP_BUFFERS; j++) {
                                if (src.saved[j]) {
                                        free(src.saved[j]);
                                }
                        }
                        free(src.saved);
                }

#if HAVE_RECVMMSG
                for (j = 0; j < RECV_BATCH_SIZE; j++) {
                        if (src.mmsgbufs[j].msg_hdr.msg_iov) {
                                free(src.mmsgbufs[j].msg_hdr.msg_iov);
                        }
                }
#else
		free(src.singlemsg.msg_iov);
#endif

                close(src.sock);
        }
        if (receiver->knownmonitors) {
                free(receiver->knownmonitors);
        }

        if (receiver->sources) {
                free(receiver->sources);
        }
}

static int ndag_pause_input(libtrace_t *libtrace) {
        int i;

        /* Close the existing receiver sockets */
        for (i = 0; i < libtrace->perpkt_thread_count; i++) {
               halt_ndag_receiver(&(FORMAT_DATA->receivers[i]));
        }
        return 0;
}

static int ndag_fin_input(libtrace_t *libtrace) {

        if (FORMAT_DATA->receivers) {
                free(FORMAT_DATA->receivers);
        }
        if (FORMAT_DATA->multicastgroup) {
                free(FORMAT_DATA->multicastgroup);
        }
        if (FORMAT_DATA->portstr) {
                free(FORMAT_DATA->portstr);
        }
        if (FORMAT_DATA->localiface) {
                free(FORMAT_DATA->localiface);
        }

        free(libtrace->format_data);
        return 0;
}

static int ndag_prepare_packet_stream(libtrace_t *libtrace,
                recvstream_t *rt,
                streamsock_t *ssock, libtrace_packet_t *packet,
                uint32_t flags) {

        dag_record_t *erfptr;
        ndag_encap_t *encaphdr;
        uint16_t ndag_reccount = 0;
        int nr;

        if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER) {
                packet->buf_control = TRACE_CTRL_PACKET;
        } else {
                packet->buf_control = TRACE_CTRL_EXTERNAL;
        }

        packet->trace = libtrace;
        packet->buffer = ssock->nextread;
        packet->header = ssock->nextread;
        packet->type = TRACE_RT_DATA_ERF;

        erfptr = (dag_record_t *)packet->header;

        if (erfptr->flags.rxerror == 1) {
                packet->payload = NULL;
                erfptr->rlen = htons(erf_get_framing_length(packet));
        } else {
                packet->payload = (char *)packet->buffer +
                                erf_get_framing_length(packet);
        }

        /* Update upstream drops using lctr */

        if (erfptr->type == TYPE_DSM_COLOR_ETH) {
                /* TODO */
        } else {
                if (rt->received_packets > 0) {
                        rt->dropped_upstream += ntohs(erfptr->lctr);
                }
        }

        rt->received_packets ++;
        ssock->recordcount += 1;

        nr = ssock->nextreadind;
        encaphdr = (ndag_encap_t *)(ssock->saved[nr] +
                        sizeof(ndag_common_t));

        ndag_reccount = ntohs(encaphdr->recordcount);
        if ((ndag_reccount & 0x8000) != 0) {
                /* Record was truncated -- update rlen appropriately */
                erfptr->rlen = htons(ssock->savedsize[nr] -
                                (ssock->nextread - ssock->saved[nr]));
        }
        ssock->nextread += ntohs(erfptr->rlen);

        if (ssock->nextread - ssock->saved[nr] >= ssock->savedsize[nr]) {
                /* Read everything from this buffer, mark as empty and
                 * move on. */
                ssock->savedsize[nr] = 0;
                ssock->bufavail ++;

                assert(ssock->bufavail > 0 && ssock->bufavail <= ENCAP_BUFFERS);
                nr ++;
                if (nr == ENCAP_BUFFERS) {
                        nr = 0;
                }
                ssock->nextread = ssock->saved[nr] + sizeof(ndag_common_t) +
                                sizeof(ndag_encap_t);
                ssock->nextreadind = nr;
        }

        packet->order = erf_get_erf_timestamp(packet);
        packet->error = packet->payload ? ntohs(erfptr->rlen) :
                        erf_get_framing_length(packet);

        return ntohs(erfptr->rlen);
}

static int ndag_prepare_packet(libtrace_t *libtrace UNUSED,
                libtrace_packet_t *packet UNUSED,
                void *buffer UNUSED, libtrace_rt_types_t rt_type UNUSED,
                uint32_t flags UNUSED) {

        assert(0 && "Sending nDAG records over RT doesn't make sense! Please stop.");
        return 0;

}

static ndag_monitor_t *add_new_knownmonitor(recvstream_t *rt, uint16_t monid) {

        ndag_monitor_t *mon;

        if (rt->monitorcount == 0) {
                rt->knownmonitors = (ndag_monitor_t *)
                                malloc(sizeof(ndag_monitor_t) * 5);
        } else {
                rt->knownmonitors = (ndag_monitor_t *)
                            realloc(rt->knownmonitors,
                            sizeof(ndag_monitor_t) * (rt->monitorcount * 5));
        }

        mon = &(rt->knownmonitors[rt->monitorcount]);
        mon->monitorid = monid;
        mon->laststart = 0;

        rt->monitorcount ++;
        return mon;
}

static int add_new_streamsock(recvstream_t *rt, streamsource_t src) {

        streamsock_t *ssock = NULL;
        ndag_monitor_t *mon = NULL;
        int i;

        /* TODO consider replacing this with a list or vector so we can
         * easily remove sources that are no longer in use, rather than
         * just setting the sock to -1 and having to check them every
         * time we want to read a packet.
         */
        if (rt->sourcecount == 0) {
                rt->sources = (streamsock_t *)malloc(sizeof(streamsock_t) * 10);
        } else if ((rt->sourcecount % 10) == 0) {
                rt->sources = (streamsock_t *)realloc(rt->sources,
                        sizeof(streamsock_t) * (rt->sourcecount + 10));
        }

        ssock = &(rt->sources[rt->sourcecount]);

        ssock->sock = join_multicast_group(src.groupaddr, src.localiface,
                        NULL, src.port, &(ssock->srcaddr));

        if (ssock->sock < 0) {
                return -1;
        }

        for (i = 0; i < rt->monitorcount; i++) {
                if (rt->knownmonitors[i].monitorid == src.monitor) {
                        mon = &(rt->knownmonitors[i]);
                        break;
                }
        }

        if (mon == NULL) {
                mon = add_new_knownmonitor(rt, src.monitor);
        }

        ssock->port = src.port;
        ssock->groupaddr = src.groupaddr;
        ssock->expectedseq = 0;
        ssock->monitorptr = mon;
        ssock->saved = (char **)malloc(sizeof(char *) * ENCAP_BUFFERS);
        ssock->bufavail = ENCAP_BUFFERS;
        ssock->startidle = 0;

        for (i = 0; i < ENCAP_BUFFERS; i++) {
                ssock->saved[i] = (char *)malloc(ENCAP_BUFSIZE);
                ssock->savedsize[i] = 0;
        }

#if HAVE_RECVMMSG
        for (i = 0; i < RECV_BATCH_SIZE; i++) {
                ssock->mmsgbufs[i].msg_hdr.msg_iov = (struct iovec *)
                                malloc(sizeof(struct iovec));
                ssock->mmsgbufs[i].msg_hdr.msg_name = ssock->srcaddr->ai_addr;
                ssock->mmsgbufs[i].msg_hdr.msg_namelen = ssock->srcaddr->ai_addrlen;
                ssock->mmsgbufs[i].msg_hdr.msg_control = NULL;
                ssock->mmsgbufs[i].msg_hdr.msg_controllen = 0;
                ssock->mmsgbufs[i].msg_hdr.msg_flags = 0;
                ssock->mmsgbufs[i].msg_len = 0;
        }
#else
	ssock->singlemsg.msg_iov = (struct iovec *) malloc(sizeof(struct iovec));
#endif

        ssock->nextread = NULL;;
        ssock->nextreadind = 0;
        ssock->recordcount = 0;
        rt->sourcecount += 1;

        fprintf(stderr, "Added new stream %s:%u to thread %d\n",
                        ssock->groupaddr, ssock->port, rt->threadindex);

        return ssock->port;
}

static int receiver_read_messages(recvstream_t *rt) {

        ndag_internal_message_t msg;

        while (libtrace_message_queue_try_get(&(rt->mqueue),
                                (void *)&msg) != LIBTRACE_MQ_FAILED) {
                switch(msg.type) {
                        case NDAG_CLIENT_NEWGROUP:
                                if (add_new_streamsock(rt, msg.contents) < 0) {
                                        return -1;
                                }
                                break;
                        case NDAG_CLIENT_HALT:
                                return 0;
                }
        }
        return 1;

}

static inline int readable_data(streamsock_t *ssock) {

        if (ssock->sock == -1) {
                return 0;
        }
        if (ssock->savedsize[ssock->nextreadind] == 0) {
                return 0;
        }
        /*
        if (ssock->nextread - ssock->saved[ssock->nextreadind] >=
                        ssock->savedsize[ssock->nextreadind]) {
                return 0;
        }
        */
        return 1;


}

static inline void reset_expected_seqs(recvstream_t *rt, ndag_monitor_t *mon) {

        int i;
        for (i = 0; i < rt->sourcecount; i++) {
                if (rt->sources[i].monitorptr == mon) {
                        rt->sources[i].expectedseq = 0;
                }
        }

}

static int init_receivers(streamsock_t *ssock, int required) {

        int wind = ssock->nextwriteind;
        int i = 1;

#if HAVE_RECVMMSG
        for (i = 0; i < required; i++) {
                if (i >= RECV_BATCH_SIZE) {
                        break;
                }

                if (wind >= ENCAP_BUFFERS) {
                        wind = 0;
                }

                ssock->mmsgbufs[i].msg_len = 0;
                ssock->mmsgbufs[i].msg_hdr.msg_iov->iov_base = ssock->saved[wind];
                ssock->mmsgbufs[i].msg_hdr.msg_iov->iov_len = ENCAP_BUFSIZE;
                ssock->mmsgbufs[i].msg_hdr.msg_iovlen = 1;

                wind ++;
        }
#else
	assert(required > 0);
	ssock->singlemsg.msg_iov->iov_base = ssock->saved[wind];
	ssock->singlemsg.msg_iov->iov_len = ENCAP_BUFSIZE;
	ssock->singlemsg.msg_iovlen = 1;
#endif
        return i;
}

static int check_ndag_received(streamsock_t *ssock, int index,
                unsigned int msglen, recvstream_t *rt) {

        ndag_encap_t *encaphdr;
        ndag_monitor_t *mon;
        uint8_t rectype;

        /* Check that we have a valid nDAG encap record */
        rectype = check_ndag_header(ssock->saved[index], (uint32_t)msglen);

        if (rectype == NDAG_PKT_KEEPALIVE) {
                /* Keep-alive, reset startidle and carry on. Don't
                 * change nextwrite -- we want to overwrite the
                 * keep-alive with usable content. */
                return 0;
        } else if (rectype != NDAG_PKT_ENCAPERF) {
                fprintf(stderr, "Received invalid record on the channel for %s:%u.\n",
                                ssock->groupaddr, ssock->port);
                close(ssock->sock);
                ssock->sock = -1;
                return -1;
        }

        ssock->savedsize[index] = msglen;
        ssock->nextwriteind ++;
        ssock->bufavail --;

        assert(ssock->bufavail >= 0);

        if (ssock->nextwriteind >= ENCAP_BUFFERS) {
                ssock->nextwriteind = 0;
        }

        /* Get the useful info from the encap header */
        encaphdr=(ndag_encap_t *)(ssock->saved[index] + sizeof(ndag_common_t));

        mon = ssock->monitorptr;

        if (mon->laststart == 0) {
                mon->laststart = bswap_be_to_host64(encaphdr->started);
        } else if (mon->laststart != bswap_be_to_host64(encaphdr->started)) {
                mon->laststart = bswap_be_to_host64(encaphdr->started);
                reset_expected_seqs(rt, mon);

                /* TODO what is a good way to indicate this to clients?
                 * set the loss counter in the ERF header? a bit rude?
                 * use another bit in the ERF header?
                 * add a queryable flag to libtrace_packet_t?
                 */

        }

        if (ssock->expectedseq != 0) {
                rt->missing_records += seq_cmp(
                                ntohl(encaphdr->seqno), ssock->expectedseq);
        }
        ssock->expectedseq = ntohl(encaphdr->seqno) + 1;
        if (ssock->expectedseq == 0) {
                ssock->expectedseq ++;
        }

        if (ssock->nextread == NULL) {
                /* If this is our first read, set up 'nextread'
                 * by skipping past the nDAG headers */
                ssock->nextread = ssock->saved[0] +
                        sizeof(ndag_common_t) + sizeof(ndag_encap_t);
        }
        return 1;

}

static int receive_from_single_socket(streamsock_t *ssock, struct timeval *tv,
                int *gottime, recvstream_t *rt) {

        int ret, ndagstat, avail;
        int toret = 0;

#if HAVE_RECVMMSG
	int i;
#endif

        if (ssock->sock == -1) {
                return 0;
        }

#if HAVE_RECVMMSG
        /* Plenty of full buffers, just use the packets in those */
        if (ssock->bufavail < RECV_BATCH_SIZE / 2) {
                return 1;
        }
#else
	if (ssock->bufavail == 0) {
		return 1;
	}
#endif

        avail = init_receivers(ssock, ssock->bufavail);

#if HAVE_RECVMMSG
        ret = recvmmsg(ssock->sock, ssock->mmsgbufs, avail,
                        MSG_DONTWAIT, NULL);
#else
	ret = recvmsg(ssock->sock, &(ssock->singlemsg), MSG_DONTWAIT);
#endif
        if (ret < 0) {
                /* Nothing to receive right now, but we should still
                 * count as 'ready' if at least one buffer is full */
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (readable_data(ssock)) {
                                toret = 1;
                        }
                        if (!(*gottime)) {
                                gettimeofday(tv, NULL);
                                *gottime = 1;
                        }
                        if (ssock->startidle == 0) {
                                ssock->startidle = tv->tv_sec;
                        } else if (tv->tv_sec - ssock->startidle > NDAG_IDLE_TIMEOUT) {
                                fprintf(stderr,
                                        "Closing channel %s:%u due to inactivity.\n",
                                        ssock->groupaddr,
                                        ssock->port);

                                close(ssock->sock);
                                ssock->sock = -1;
                        }
                } else {

                        fprintf(stderr,
                                "Error receiving encapsulated records from %s:%u -- %s \n",
                                ssock->groupaddr, ssock->port,
                                strerror(errno));
                        close(ssock->sock);
                        ssock->sock = -1;
                }
                return toret;
        }

        ssock->startidle = 0;

#if HAVE_RECVMMSG
        for (i = 0; i < ret; i++) {
                ndagstat = check_ndag_received(ssock, ssock->nextwriteind,
                                ssock->mmsgbufs[i].msg_len, rt);
                if (ndagstat == -1) {
                        break;
                }

                if (ndagstat == 1) {
                        toret = 1;
                }
        }
#else
	ndagstat = check_ndag_received(ssock, ssock->nextwriteind, ret, rt);
	if (ndagstat <= 0) {
		toret = 0;
	} else {
		toret = 1;
	}
#endif

        return toret;
}

static int receive_from_sockets(recvstream_t *rt) {

        int i, readybufs, gottime;
        struct timeval tv;

        readybufs = 0;
        gottime = 0;

        for (i = 0; i < rt->sourcecount; i ++) {
                readybufs += receive_from_single_socket(&(rt->sources[i]),
                                &tv, &gottime, rt);
        }

        return readybufs;

}


static int receive_encap_records_block(libtrace_t *libtrace, recvstream_t *rt,
                libtrace_packet_t *packet) {

        int iserr = 0;

        if (packet->buf_control == TRACE_CTRL_PACKET) {
                free(packet->buffer);
                packet->buffer = NULL;
        }

        do {
                /* Make sure we shouldn't be halting */
                if ((iserr = is_halted(libtrace)) != -1) {
                        return iserr;
                }

                /* Check for any messages from the control thread */
                iserr = receiver_read_messages(rt);

                if (iserr <= 0) {
                        return iserr;
                }

                /* If blocking and no sources, sleep for a bit and then try
                 * checking for messages again.
                 */
                if (rt->sourcecount == 0) {
                        usleep(10000);
                        continue;
                }

                if ((iserr = receive_from_sockets(rt)) < 0) {
                        return iserr;
                } else if (iserr > 0) {
                        /* At least one of our input sockets has available
                         * data, let's go ahead and use what we have. */
                        break;
                }

                /* None of our sources have anything available, we can take
                 * a short break rather than immediately trying again.
                 */
                if (iserr == 0) {
                        usleep(100);
                }

        } while (1);

        return iserr;
}

static int receive_encap_records_nonblock(libtrace_t *libtrace, recvstream_t *rt,
                libtrace_packet_t *packet) {

        int iserr = 0;

        if (packet->buf_control == TRACE_CTRL_PACKET) {
                free(packet->buffer);
                packet->buffer = NULL;
        }

        /* Make sure we shouldn't be halting */
        if ((iserr = is_halted(libtrace)) != -1) {
                return iserr;
        }

        /* If non-blocking and there are no sources, just break */
        if (rt->sourcecount == 0) {
                return 0;
        }

        return receive_from_sockets(rt);
}

static streamsock_t *select_next_packet(recvstream_t *rt) {
        int i;
        streamsock_t *ssock = NULL;
        uint64_t earliest = 0;
        uint64_t currentts = 0;
        dag_record_t *daghdr;

        for (i = 0; i < rt->sourcecount; i ++) {
                if (!readable_data(&(rt->sources[i]))) {
                        continue;
                }

                daghdr = (dag_record_t *)(rt->sources[i].nextread);
                currentts = bswap_le_to_host64(daghdr->ts);

                if (earliest == 0 || earliest > currentts) {
                        earliest = currentts;
                        ssock = &(rt->sources[i]);
                }
                /*
                fprintf(stderr, "%d %d %lu %lu %lu\n", rt->threadindex,
                                i, currentts,
                                rt->sources[i].recordcount, 
                                rt->missing_records);
                */
        }
        return ssock;
}

static int ndag_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {

        int rem;
        streamsock_t *nextavail = NULL;
        rem = receive_encap_records_block(libtrace, &(FORMAT_DATA->receivers[0]),
                        packet);

        if (rem <= 0) {
                return rem;
        }

        nextavail = select_next_packet(&(FORMAT_DATA->receivers[0]));
        if (nextavail == NULL) {
                return 0;
        }

        /* nextread should point at an ERF header, so prepare 'packet' to be
         * a libtrace ERF packet. */

        return ndag_prepare_packet_stream(libtrace,
                        &(FORMAT_DATA->receivers[0]), nextavail,
                        packet, TRACE_PREP_DO_NOT_OWN_BUFFER);
}

static int ndag_pread_packets(libtrace_t *libtrace, libtrace_thread_t *t,
                libtrace_packet_t **packets, size_t nb_packets) {

        recvstream_t *rt;
        int rem;
        size_t read_packets = 0;
        streamsock_t *nextavail = NULL;

        rt = (recvstream_t *)t->format_data;


        do {
                /* Only check for messages once per batch */
                if (read_packets == 0) {
                        rem = receive_encap_records_block(libtrace, rt,
                                packets[read_packets]);
                } else {
                        rem = receive_encap_records_nonblock(libtrace, rt,
                                packets[read_packets]);
                }

                if (rem < 0) {
                        return rem;
                }

                if (rem == 0) {
                        break;
                }

                nextavail = select_next_packet(rt);
                if (nextavail == NULL) {
                        break;
                }

                ndag_prepare_packet_stream(libtrace, rt, nextavail,
                                packets[read_packets],
                                TRACE_PREP_DO_NOT_OWN_BUFFER);

                read_packets  ++;
                if (read_packets >= nb_packets) {
                        break;
                }
        } while (1);

        return read_packets;

}

static libtrace_eventobj_t trace_event_ndag(libtrace_t *libtrace,
                libtrace_packet_t *packet) {


        libtrace_eventobj_t event = {0,0,0.0,0};
        int rem;
        streamsock_t *nextavail = NULL;

        /* Only check for messages once per call */
        rem = receiver_read_messages(&(FORMAT_DATA->receivers[0]));
        if (rem <= 0) {
                event.type = TRACE_EVENT_TERMINATE;
                return event;
        }

        do {
                rem = receive_encap_records_nonblock(libtrace,
                                &(FORMAT_DATA->receivers[0]), packet);

                if (rem < 0) {
                        trace_set_err(libtrace, TRACE_ERR_BAD_PACKET,
                                "Received invalid nDAG records.");
                        event.type = TRACE_EVENT_TERMINATE;
                        break;
                }

                if (rem == 0) {
                        /* Either we've been halted or we've got no packets
                         * right now. */
                        if (is_halted(libtrace) == 0) {
                                event.type = TRACE_EVENT_TERMINATE;
                                break;
                        }
                        event.type = TRACE_EVENT_SLEEP;
                        event.seconds = 0.0001;
                        break;
                }

                nextavail = select_next_packet(&(FORMAT_DATA->receivers[0]));
                if (nextavail == NULL) {
                        event.type = TRACE_EVENT_SLEEP;
                        event.seconds = 0.0001;
                        break;
                }

                event.type = TRACE_EVENT_PACKET;
                ndag_prepare_packet_stream(libtrace,
                                &(FORMAT_DATA->receivers[0]), nextavail,
                                packet, TRACE_PREP_DO_NOT_OWN_BUFFER);
                event.size = trace_get_capture_length(packet) +
                                trace_get_framing_length(packet);

                if (libtrace->filter) {
                        int filtret = trace_apply_filter(libtrace->filter,
                                        packet);
                        if (filtret == -1) {
                                trace_set_err(libtrace,
                                                TRACE_ERR_BAD_FILTER,
                                                "Bad BPF Filter");
                                event.type = TRACE_EVENT_TERMINATE;
                                break;
                        }

                        if (filtret == 0) {
                                /* Didn't match filter, try next one */
                                libtrace->filtered_packets ++;
                                trace_clear_cache(packet);
                                continue;
                        }
                }

                if (libtrace->snaplen > 0) {
                        trace_set_capture_length(packet, libtrace->snaplen);
                }
                libtrace->accepted_packets ++;
                break;
        } while (1);

        return event;
}

static void ndag_get_statistics(libtrace_t *libtrace, libtrace_stat_t *stat) {

        int i;

        stat->dropped_valid = 1;
        stat->dropped = 0;
        stat->received_valid = 1;
        stat->received = 0;
        stat->missing_valid = 1;
        stat->missing = 0;

        /* TODO Is this thread safe? */
        for (i = 0; i < libtrace->perpkt_thread_count; i++) {
                stat->dropped += FORMAT_DATA->receivers[i].dropped_upstream;
                stat->received += FORMAT_DATA->receivers[i].received_packets;
                stat->missing += FORMAT_DATA->receivers[i].missing_records;
        }

}

static void ndag_get_thread_stats(libtrace_t *libtrace, libtrace_thread_t *t,
                libtrace_stat_t *stat) {

        recvstream_t *recvr = (recvstream_t *)t->format_data;

        if (libtrace == NULL)
                return;
        /* TODO Is this thread safe */
        stat->dropped_valid = 1;
        stat->dropped = recvr->dropped_upstream;

        stat->received_valid = 1;
        stat->received = recvr->received_packets;

        stat->missing_valid = 1;
        stat->missing = recvr->missing_records;

}

static int ndag_pregister_thread(libtrace_t *libtrace, libtrace_thread_t *t,
                bool reader) {
        recvstream_t *recvr;

        if (!reader || t->type != THREAD_PERPKT) {
                return 0;
        }

        recvr = &(FORMAT_DATA->receivers[t->perpkt_num]);
        t->format_data = recvr;

        return 0;
}

static struct libtrace_format_t ndag = {

        "ndag",
        "",
        TRACE_FORMAT_NDAG,
        NULL,                   /* probe filename */
        NULL,                   /* probe magic */
        ndag_init_input,        /* init_input */
        NULL,                   /* config_input */
        ndag_start_input,       /* start_input */
        ndag_pause_input,       /* pause_input */
        NULL,                   /* init_output */
        NULL,                   /* config_output */
        NULL,                   /* start_output */
        ndag_fin_input,         /* fin_input */
        NULL,                   /* fin_output */
        ndag_read_packet,       /* read_packet */
        ndag_prepare_packet,    /* prepare_packet */
        NULL,                   /* fin_packet */
        NULL,                   /* write_packet */
        erf_get_link_type,      /* get_link_type */
        erf_get_direction,      /* get_direction */
        erf_set_direction,      /* set_direction */
        erf_get_erf_timestamp,  /* get_erf_timestamp */
        NULL,                   /* get_timeval */
        NULL,                   /* get_seconds */
        NULL,                   /* get_timespec */
        NULL,                   /* seek_erf */
        NULL,                   /* seek_timeval */
        NULL,                   /* seek_seconds */
        erf_get_capture_length, /* get_capture_length */
        erf_get_wire_length,    /* get_wire_length */
        erf_get_framing_length, /* get_framing_length */
        erf_set_capture_length, /* set_capture_length */
        NULL,                   /* get_received_packets */
        NULL,                   /* get_filtered_packets */
        NULL,                   /* get_dropped_packets */
        ndag_get_statistics,    /* get_statistics */
        NULL,                   /* get_fd */
        trace_event_ndag,       /* trace_event */
        NULL,                   /* help */
        NULL,                   /* next pointer */
        {true, 0},              /* live packet capture */
        ndag_pstart_input,      /* parallel start */
        ndag_pread_packets,     /* parallel read */
        ndag_pause_input,       /* parallel pause */
        NULL,
        ndag_pregister_thread,  /* register thread */
        NULL,
        ndag_get_thread_stats   /* per-thread stats */
};

void ndag_constructor(void) {
        register_format(&ndag);
}
