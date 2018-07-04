#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ev.h>
#include "net.h"

#include "linklist.h"
#include "rbtree.h"

#include "server.h"
#include "mptunnel.h"
#include "buffer.h"
#include "client.h"

#define UDP_KEEP_ALIVE 300
#define UDP_MAX_SIZE  65536
#define UDP_INTERACTIVE_TIMEOUT 60

static struct ev_loop *g_ev_reactor = NULL;

static struct list_head g_bridge_list = LIST_HEAD_INIT(g_bridge_list);
static pthread_mutex_t g_bridge_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_listen_fd = -1;
static int g_upstream_fd = -1;

static int g_listen_port = 0;
static char *g_upstream_host = NULL;
static int g_upstream_port = 0;

extern int g_config_encrypt;

static void recv_bridge_callback(struct ev_loop *reactor, ev_io *w, int events)
{
    char *buf;
    int buf_len = UDP_MAX_SIZE, readb;
    struct sockaddr_in baddr;
    static received_t *received = NULL;
 
    if (received == NULL) {
        received = malloc(sizeof(*received));
        received_init(received);
    }
    
    buf = malloc(buf_len);
    memset(buf, 0x00, buf_len);
    
    bridge_t *b = (bridge_t *)malloc(sizeof(bridge_t));
    memset(b, 0, sizeof(*b));
    b->st_time = time(NULL);
    b->addrlen = sizeof(b->addr);
    baddr = *(struct sockaddr_in *)&b->addr;
    
    readb = recvfrom(w->fd, buf, buf_len, 0, &b->addr, &b->addrlen);
    if (readb < 0) {
        LOGW(_("Bridge(fd=%d) may close the connection: %s\n"), w->fd, strerror(errno));
        free(buf);
        free(b);
        return;
    } else if (readb == 0) {
        LOGW(_("Can't received packet from bridge(fd=%d)，bridge may close the connection\n"), w->fd);
        free(buf);
        free(b);
        return;
    } else {
        int found = 0;
        bridge_t *lb;
        struct list_head *l;
 
        pthread_mutex_lock(&g_bridge_list_mutex);
        list_for_each(l, &g_bridge_list) {
            lb = list_entry(l, bridge_t, list);
            if (memcmp(&lb->addr, &b->addr, sizeof(struct sockaddr)) == 0) {
                found = 1;
                free(b);
                b = lb;
                break;
            }
        }
        b->rc_time = time(NULL);
        if (!found) {
            LOGI(_("Got a new client, add it to Client List\n"));
            list_add(&b->list, &g_bridge_list);
        }
        pthread_mutex_unlock(&g_bridge_list_mutex);
    }
       
    mpdecrypt(buf);

    packet_t *p = (packet_t *)buf;
    if (p->type == PKT_TYPE_CTL) {
        LOGD(_("Received control packet from bridge (:%u) of %d bytes, packet ID is %d, drop it\n"),
            htons(baddr.sin_port), readb, p->id);
        free(buf);
        return;
    } else if (p->type != PKT_TYPE_DATA) {
        LOGD(_("Received packet from bridge (:%u) of %d bytes, packet ID is %d, but packet type is unknown, drop it.\n"),
            htons(baddr.sin_port), readb, p->id);
        free(buf);
        return;
    } else {
        LOGD(_("Received packet from bridge(:%u) of %d bytes, packet ID is %d\n"), htons(baddr.sin_port), readb, p->id);
    }
    
    buf_len = p->buflen;
    buf = (char *)buf + sizeof(*p);
    if (received_is_received(received, p->id) == 1) {
        LOGD(_("Received packet from bridge (:%u) of %d bytes which was received, packet ID is %d, drop it\n"),
            htons(baddr.sin_port), readb, p->id);
        free(p);
        
        //received_destroy(received);
        //free(received);
        //received = NULL;
        
        return;
    } else {
        LOGD(_("Received packet from bridge (:%u) of %d bytes, ID is %d, forward it\n"),
            htons(baddr.sin_port), readb, p->id);
        received_add(received, p->id);
    }
 
    if (received != NULL)
        received_try_dropdead(received, 30);
    
    int sendb = send(g_upstream_fd, buf, buf_len, MSG_DONTWAIT);
    if (sendb < 0) {
        LOGW(_("Can't send packet #%d to target server: %s\n"), p->id, strerror(errno));
    } else if (sendb == 0) {
        LOGW(_("Connection to target server seems closed, can't forward packet #%d\n"), p->id);
    } else {
        LOGD(_("Send to target server %d byte：%s\n"), buf_len, buf);
    }
    
    free(p);

    return;
}

static void * listen_thread(void *ptr) 
{
    int port = 3002;
       
    g_listen_fd = net_bind("0.0.0.0", port, SOCK_DGRAM);
    if (g_listen_fd < 0) {
        LOGE(_("Can't listen port %d: %s\n"), port, strerror(errno));
        exit(0);
    }
    
    g_ev_reactor = ev_loop_new(EVFLAG_AUTO);
     
    ev_io *w = (ev_io*)malloc(sizeof(ev_io));
    ev_io_init(w, recv_bridge_callback, g_listen_fd, EV_READ);
    ev_io_start(g_ev_reactor, w);
     
    ev_run(g_ev_reactor, 0);
}

static int send_to_bridges(char *buf, int len)
{
    struct sockaddr *addr;
    struct sockaddr_in *baddr;
    socklen_t addrlen;
    int sendb;
    char ipstr[128] = {0};
    static int id = 0;
    
    if (len > MAX_PACKET_SIZE) {
        int ret = 0;
        int split = len / 2;
        
        LOGI(_("Packet is %d bytes, which exceeds max packet size limit (%d bytes), "
            "spilt the packet into two smaller packets before send.\n"), buflen, MAX_PACKET_SIZE);
        
        ret += send_to_bridges(buf, split);
        ret += send_to_bridges(buf + split, len - split);
        return ret;
    }
    
    packet_t *pkt = (packet_t *)malloc(sizeof(*pkt) + len);
    pkt->type = PKT_TYPE_DATA;
    pkt->id = ++id;
    pkt->buflen = len;
    memcpy(((char*)pkt) + sizeof(*pkt), buf, len);
    
    packet_t rawpkt = *pkt;
 
    mpencrypt((char *)pkt, len + sizeof(*pkt));
    
    int ts = time(NULL);
    bridge_t *b;
    struct list_head *l, *tmp;
    
    list_for_each_safe(l, tmp, &g_bridge_list) {
        b = list_entry(l, bridge_t, list);
        baddr = (struct sockaddr_in *)&b->addr;
        if (ts - b->rc_time > UDP_KEEP_ALIVE) {
            LOGD(_("No packet received from bridge (%s:%u) for %d seconds, assume the connection is closed, "
                "stop forward packet to it %d\n"), ipstr, ntohs(baddr->sin_port), ts - b->rc_time, pkt->id);
            list_del(l);
            free(l);
            continue;
        }
        if (abs(b->rc_time - b->st_time) > UDP_INTERACTIVE_TIMEOUT) {
            LOGD(_("The time difference between packet received and packet sent of bridge %s:%u is "
                "larger than %d seconds (Actually %d seconds), assume the connection is broken, "
                "stop forward packet to it %d\n"), ipstr, ntohs(baddr->sin_port), 
                UDP_INTERACTIVE_TIMEOUT, b->rc_time - b->st_time, pkt->id);
            list_del(l);
            free(l);
            continue;
        }
        
        b->st_time = time(NULL);
        
        sendb = sendto(g_listen_fd, pkt, len + sizeof(*pkt), 0, &b->addr, b->addrlen);
        if (sendb < 0) {
            LOGW(_("Can't send packet to bridge(%s:%d) of %d bytes, packet ID is %d: %s\n"),
                ipstr, ntohs(baddr->sin_port), buflen, rawp.id, strerror(errno));
        } else if (sendb == 0) {
            LOGW(_("Can't send packet to bridge, bridge may close the connection\n"));
        } else {
            LOGD(_("Forward packet to bridge(port %u) of %d bytes, packet ID is %d\n"), 
                ntohs(baddr->sin_port), sendb, rawp.id);
        }
    }
    
    free(pkt);
    
    return 0;
}

static void * upstream_thread(void *ptr)
{
    int buf_len;
    char *buf;
    
    buf_len = UDP_MAX_SIZE;
    buf = malloc(buf_len);
    if (!buf) {
        LOGE(_("Fail to allocate memory(size: %d).\n"), buf_len);
        return NULL;
    }
    
    g_upstream_fd = net_connect(g_upstream_host, g_upstream_port, SOCK_DGRAM);
    if (g_upstream_fd < 0) {
        LOGE(_("Could not connect to upstream server：%s\n"), strerror(errno));
        return NULL;
    }
    
    while (1) {
        int readb = recv(g_upstream_fd, buf, buf_len, 0);
        if (readb < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
                LOGI(_("Upstream server close the connection: %s\n"), strerror(errno));
                close(g_upstream_fd);
                g_upstream_fd = net_connect(g_upstream_host, g_upstream_port, SOCK_DGRAM);
                if (g_upstream_fd < 0) {
                    LOGE(_("Could not connect to upstream server：%s\n"), strerror(errno));
                    break;
                }
                continue;
            }
        } else if (readb == 0) {
            LOGW(_("Can't received packet from server, server close the connection\n"));
            close(g_upstream_fd);
            g_upstream_fd = net_connect(g_upstream_host, g_upstream_port, SOCK_DGRAM);
            if (g_upstream_fd < 0) {
                LOGE(_("Could not connect to upstream server：%s\n"), strerror(errno));
                break;
            }
            continue;
        } else {
            send_to_bridges(buf, readb);
        }
    }
    
    free(buf);
    
    return NULL;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("mptunnel", "locale");
    textdomain("mptunnel");
    
    if (argc <= 3) {
        fprintf(stderr, _("Usage: <%s> <listen_port> <target_ip> <target_port>\n"), argv[0]);
        fprintf(stderr, _("To disable encryption, set environment variable MPTUNNEL_ENCRYPT=0\n"));
        exit(-1);
    } else {
        g_listen_port = atoi(argv[1]);
        g_upstream_host = strdup(argv[2]);
        g_upstream_port = atoi(argv[3]);
        
        if (g_listen_port <= 0 || g_listen_port >= 65536) {
            LOGE("Invalid listen port `%s'\n", argv[1]);
            exit(-2);
        }
        if (g_upstream_port <= 0 || g_upstream_port >= 65536) {
            LOGE("Invalid target port `%s'\n", argv[3]);
            exit(-3);
        }
        
        if (getenv("MPTUNNEL_ENCRYPT") == NULL) {
            g_config_encrypt = 1;
        } else if(atoi(getenv("MPTUNNEL_ENCRYPT")) == 0) {
            g_config_encrypt = 0;
        } else {
            g_config_encrypt = 1;
        }
    }
    
    pthread_t tid;
    pthread_create(&tid, NULL, listen_thread, NULL);
    pthread_detach(tid);

    pthread_create(&tid, NULL, upstream_thread, NULL);
    pthread_detach(tid);
    
    while (1) {
        sleep(100);
    }

    return 0;
}
