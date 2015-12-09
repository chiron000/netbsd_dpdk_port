/*
 * app_glue.c
 *
 *  Created on: Jul 6, 2014
 *      Author: Vadim Suraev vadim.suraev@gmail.com
 *  Contains API functions for building applications
 *  on the top of Linux TCP/IP ported to userland and integrated with DPDK 1.6
 */
//#include <stdint.h>
//#include <special_includes/sys/types.h>
#include <special_includes/sys/param.h>
#include <special_includes/sys/malloc.h>
#include <lib/libkern/libkern.h>
#include <special_includes/sys/mbuf.h>
#include <special_includes/sys/queue.h>
#include <special_includes/sys/socket.h>
#include <special_includes/sys/socketvar.h>
#include <special_includes/sys/time.h>
#include <special_includes/sys/poll.h>
#include <netbsd/netinet/in.h>

#include <netbsd/net/if.h>
#include <netbsd/net/route.h>
#include <netbsd/net/if_types.h>

#include <netbsd/netinet/in.h>
#include <netbsd/netinet/in_systm.h>
#include <netbsd/netinet/ip.h>
#include <netbsd/netinet/in_pcb.h>
#include <netbsd/netinet/in_var.h>
#include <netbsd/netinet/ip_var.h>
#include <netbsd/netinet/in_offload.h>

#ifdef INET6
#ifndef INET
#include <netbsd/netinet/in.h>
#endif
#include <netbsd/netinet/ip6.h>
#include <netbsd/netinet6/ip6_var.h>
#include <netbsd/netinet6/in6_pcb.h>
#include <netinet6/ip6_var.h>
#include <netinet6/in6_var.h>
#include <netbsd/netinet/icmp6.h>
#include <netbsd/netinet6/nd6.h>
#ifdef TCP_SIGNATURE
#include <netbsd/netinet6/scope6_var.h>
#endif
#endif

#ifndef INET6
/* always need ip6.h for IP6_EXTHDR_GET */
#include <netbsd/netinet/ip6.h>
#endif

#include <netbsd/netinet/tcp.h>
#include <netbsd/netinet/tcp_fsm.h>
#include <netbsd/netinet/tcp_seq.h>
#include <netbsd/netinet/tcp_timer.h>
#include <netbsd/netinet/tcp_var.h>
#include <service_log.h>

TAILQ_HEAD(read_ready_socket_list_head, socket) read_ready_socket_list_head;
uint64_t read_sockets_queue_len = 0;
TAILQ_HEAD(closed_socket_list_head, socket) closed_socket_list_head;
TAILQ_HEAD(write_ready_socket_list_head, socket) write_ready_socket_list_head;
uint64_t write_sockets_queue_len = 0;
TAILQ_HEAD(accept_ready_socket_list_head, socket) accept_ready_socket_list_head;
uint64_t working_cycles_stat = 0;
uint64_t total_cycles_stat = 0;
uint64_t work_prev = 0;
uint64_t total_prev = 0;
/*
 * This callback function is invoked when data arrives to socket.
 * It inserts the socket into a list of readable sockets
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock, len (dummy)
 * Returns: void
 *
 */

static inline void app_glue_sock_readable(struct socket *so)
{
	if(so->so_type == SOCK_STREAM) {
		struct tcpcb *tp = sototcpcb(so);

		if(tp->t_state != TCPS_ESTABLISHED) {
			if(tp->t_state == TCPS_LISTEN) {
				if(so->accept_queue_present) {
					return;
				}
				so->accept_queue_present = 1;
				TAILQ_INSERT_TAIL(&accept_ready_socket_list_head,so,accept_queue_entry);
			}
			return;
		}
	}
	if(so->read_queue_present) {
		return;
	}
	so->read_queue_present = 1;
	TAILQ_INSERT_TAIL(&read_ready_socket_list_head,so,read_queue_entry);
        read_sockets_queue_len++;
}
/*
 * This callback function is invoked when data canbe transmitted on socket.
 * It inserts the socket into a list of writable sockets
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock
 * Returns: void
 *
 */
static void app_glue_sock_write_space(struct socket *so)
{
	if(so->so_type == SOCK_STREAM) {
		struct tcpcb *tp = sototcpcb(so);

		if(tp->t_state != TCPS_ESTABLISHED) {
			return;
		}
	}
	if (sowritable(so)) {
		if(so->write_queue_present) {
			return;
		}
		so->write_queue_present = 1;
		TAILQ_INSERT_TAIL(&write_ready_socket_list_head,so,write_queue_entry);
                write_sockets_queue_len++;
	}
}
#if 0
/*
 * This callback function is invoked when an error occurs on socket.
 * It inserts the socket into a list of closable sockets
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock
 * Returns: void
 *
 */
static void app_glue_sock_error_report(struct sock *sk)
{
	if(sk->sk_socket) {
		if(sk->sk_socket->closed_queue_present) {
			return;
		}
		sk->sk_socket->closed_queue_present = 1;
		TAILQ_INSERT_TAIL(&closed_socket_list_head,sk->sk_socket,closed_queue_entry);
	}
}
/*
 * This callback function is invoked when a new connection can be accepted on socket.
 * It looks up the parent (listening) socket for the newly established connection
 * and inserts it into the accept queue
 * which is processed in periodic function app_glue_periodic
 * Paramters: a pointer to struct sock
 * Returns: void
 *
 */
static void app_glue_sock_wakeup(struct sock *sk)
{
	struct sock *sock;
        struct tcp_sock *tp;
        tp = tcp_sk(sk);

	sock = __inet_lookup_listener(&init_net/*sk->sk_net*/,
			&tcp_hashinfo,
			sk->sk_daddr,
			sk->sk_dport/*__be16 sport*/,
			sk->sk_rcv_saddr,
			ntohs(tp->inet_conn.icsk_inet.inet_sport),//sk->sk_num/*const unsigned short hnum*/,
			sk->sk_bound_dev_if);
	if(sock) {
		if(sock->sk_socket->accept_queue_present) {
			return;
		}
		sock->sk_socket->accept_queue_present = 1;
		TAILQ_INSERT_TAIL(&accept_ready_socket_list_head,sock->sk_socket,accept_queue_entry);
	}
        else {
              struct tcp_sock *tp;
              tp = tcp_sk(sk);
              printf("%s %d %x %d %x %d %d \n",__FILE__,__LINE__,sk->sk_daddr,sk->sk_dport,sk->sk_rcv_saddr,sk->sk_num,tp->inet_conn.icsk_inet.inet_sport);
              return;
        }
	sock_reset_flag(sk,SOCK_USE_WRITE_QUEUE);
	sk->sk_data_ready = app_glue_sock_readable;
	sk->sk_write_space = app_glue_sock_write_space;
	sk->sk_error_report = app_glue_sock_error_report; 
}
#endif
void app_glue_so_upcall(struct socket *sock, void *arg, int band, int flag)
{
	if(band | POLLIN) {
		app_glue_sock_readable(sock);
	}
	if(band | POLLOUT) {
		app_glue_sock_write_space(sock);
	}
}

void *app_glue_create_socket(int family,int type)
{
	struct timeval tv;
	struct socket *sock = NULL;

	if(socreate(family, &sock, type, 0/*port*/, NULL)) {
		printf("cannot create socket %s %d\n",__FILE__,__LINE__);
		return NULL;
	}	
	tv.tv_sec = -1;
	tv.tv_usec = 0;
	if (app_glue_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, sizeof(tv), (char *)&tv)) {
		service_log(SERVICE_LOG_ERR,"%s %d cannot set notimeout option\n",__FILE__,__LINE__);
	}
	tv.tv_sec = -1;
	tv.tv_usec = 0;
	if (app_glue_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, sizeof(tv), (char *)&tv)) {
		service_log(SERVICE_LOG_ERR,"%s %d cannot set notimeout option\n",__FILE__,__LINE__);
	}	
	sock->so_upcall2 = app_glue_so_upcall;
#if 0
	if(type != SOCK_STREAM) {
		if(sock->sk) {
            		sock_reset_flag(sock->sk,SOCK_USE_WRITE_QUEUE);
            		sock->sk->sk_data_ready = app_glue_sock_readable;
            		sock->sk->sk_write_space = app_glue_sock_write_space;
            		app_glue_sock_write_space(sock->sk);
		}
	}
#endif
	return sock;
}

int app_glue_v4_bind(void *so,unsigned int ipaddr, unsigned short port)
{
	struct socket *sock = (struct socket *)so;
	struct mbuf *m;
	struct sockaddr_in *sin;
		
	m = m_get(M_WAIT, MT_SONAME);
	if (!m) {
		printf("cannot create socket %s %d\n",__FILE__,__LINE__);
		return -1;
	}
	m->m_len = sizeof(struct sockaddr);
	sin = mtod(m, struct sockaddr_in *);
	sin->sin_len = sizeof(struct sockaddr_in);
	sin->sin_family = AF_INET;

	sin->sin_addr.s_addr = ipaddr;
	sin->sin_port = htons(port);

	if(sobind(sock,m)) {
		printf("cannot bind %s %d\n",__FILE__,__LINE__);
		return -1;
	}
	m_freem(m);
	return 0;
}

int app_glue_v4_connect(void *so,unsigned int ipaddr,unsigned short port)
{
	struct socket *sock = (struct socket *)so;
	struct mbuf *m;
	struct sockaddr_in *sin;
	unsigned short my_port;
		
	m = m_get(M_WAIT, MT_SONAME);
	if (!m) {
		printf("cannot create socket %s %d\n",__FILE__,__LINE__);
		return -1;
	}
	m->m_len = sizeof(struct sockaddr);
	sin = mtod(m, struct sockaddr_in *);
	sin->sin_len = sizeof(struct sockaddr_in);
	sin->sin_family = AF_INET;
	while(1) {
		sin->sin_addr.s_addr = 0 /*my_ip_addr*/;
		if(my_port) {
			sin->sin_port = htons(my_port);
		}
		else {
			sin->sin_port = htons(rand() & 0xffff);
		}
		if(sobind(sock,m)) {
			printf("cannot bind %s %d\n",__FILE__,__LINE__);
			if(my_port) {
				break;
			}
			continue;
		}
		break;
	}
	printf("%s %s %d %p\n",__FILE__,__func__,__LINE__,sock);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = ipaddr;
	sin->sin_port = htons(port);
#if 0
	if(client_sock->sk) {
		client_sock->sk->sk_state_change = app_glue_sock_wakeup;
	}
#endif	
	soconnect(sock, m);
	m_freem(m);	
	return 0;
}

int app_glue_v4_listen(void *so)
{
	struct socket *sock = (struct socket *)so;
	sock->so_upcall2 = app_glue_so_upcall;
	if(solisten(sock,32000)) {
		printf("cannot listen %s %d\n",__FILE__,__LINE__);
		return -1;
	}
	return 0;
}

static void *port_2_ifp[10] = { 0 };

static void app_glue_set_port_ifp(int portnum, void *ifp)
{
	if (portnum >= 10)
		return;
	port_2_ifp[portnum] = ifp;
}
/*
 * This function polls the driver for the received packets.Called from app_glue_periodic
 * Paramters: ethernet port number.
 * Returns: None
 *
 */
static inline void app_glue_poll(int port_num)
{
	if (port_num >= 10)
		return;
	poll_rx(port_2_ifp[port_num],port_num,0);
}

/*
 * This function must be called before app_glue_periodic is called for the first time.
 * It initializes the readable, writable and acceptable (listeners) lists
 * Paramters: None
 * Returns: None
 *
 */
void app_glue_init()
{
	TAILQ_INIT(&read_ready_socket_list_head);
	TAILQ_INIT(&write_ready_socket_list_head);
	TAILQ_INIT(&accept_ready_socket_list_head);
	TAILQ_INIT(&closed_socket_list_head);
}
/*
 * This function walks on closable, acceptable and readable lists and calls.
 * the application's (user's) function. Called from app_glue_periodic
 * Paramters: None
 * Returns: None
 *
 */
static inline void process_rx_ready_sockets()
{
	struct socket *sock;
        uint64_t idx,limit;

	while(!TAILQ_EMPTY(&closed_socket_list_head)) {
		sock = TAILQ_FIRST(&closed_socket_list_head);
//		user_on_socket_fatal(sock);
		sock->closed_queue_present = 0;
		TAILQ_REMOVE(&closed_socket_list_head,sock,closed_queue_entry);
		soclose(sock);
	}
	while(!TAILQ_EMPTY(&accept_ready_socket_list_head)) {

		sock = TAILQ_FIRST(&accept_ready_socket_list_head);
		user_on_accept(sock);
		sock->accept_queue_present = 0;
		TAILQ_REMOVE(&accept_ready_socket_list_head,sock,accept_queue_entry);
	}
        idx = 0;
        limit = read_sockets_queue_len;
	while((idx < limit)&&(!TAILQ_EMPTY(&read_ready_socket_list_head))) {
		sock = TAILQ_FIRST(&read_ready_socket_list_head);
                sock->read_queue_present = 0;
		TAILQ_REMOVE(&read_ready_socket_list_head,sock,read_queue_entry);
                user_data_available_cbk(sock, sock->glueing_block);
                read_sockets_queue_len--;
                idx++;	
	}
}
/*
 * This function walks on writable lists and calls.
 * the application's (user's) function. Called from app_glue_periodic
 * Paramters: None
 * Returns: None
 *
 */
static inline void process_tx_ready_sockets()
{
	struct socket *sock;
        uint64_t idx,limit;
 
        idx = 0;
        limit = write_sockets_queue_len;
	while((idx < limit)&&(!TAILQ_EMPTY(&write_ready_socket_list_head))) {
		sock = TAILQ_FIRST(&write_ready_socket_list_head);
		TAILQ_REMOVE(&write_ready_socket_list_head,sock,write_queue_entry);
                sock->write_queue_present = 0;
		service_on_transmission_opportunity(sock, sock->glueing_block);
//                set_bit(SOCK_NOSPACE, &sock->flags);
                write_sockets_queue_len--;
	        idx++;
	}
}
/* These are in translation of micros to cycles */
static uint64_t app_glue_drv_poll_interval = 0;
static uint64_t app_glue_timer_poll_interval = 0;
static uint64_t app_glue_tx_ready_sockets_poll_interval = 0;
static uint64_t app_glue_rx_ready_sockets_poll_interval = 0;

static uint64_t app_glue_drv_last_poll_ts = 0;
static uint64_t app_glue_timer_last_poll_ts = 0;
static uint64_t app_glue_tx_ready_sockets_last_poll_ts = 0;
static uint64_t app_glue_rx_ready_sockets_last_poll_ts = 0;

/*
 * This function must be called by application to initialize.
 * the rate of polling for driver, timer, readable & writable socket lists
 * Paramters: drv_poll_interval,timer_poll_interval,tx_ready_sockets_poll_interval,
 * rx_ready_sockets_poll_interval - all in micros
 * Returns: None
 *
 */
void app_glue_init_poll_intervals(int drv_poll_interval,
		                          int timer_poll_interval,
		                          int tx_ready_sockets_poll_interval,
		                          int rx_ready_sockets_poll_interval)
{
#if 0
	printf("%s %d %d %d %d %d\n",__func__,__LINE__,
			drv_poll_interval,timer_poll_interval,tx_ready_sockets_poll_interval,
			rx_ready_sockets_poll_interval);
	float cycles_in_micro = rte_get_tsc_hz()/1000000;
	app_glue_drv_poll_interval = cycles_in_micro*(float)drv_poll_interval;
	app_glue_timer_poll_interval = cycles_in_micro*(float)timer_poll_interval;
	app_glue_tx_ready_sockets_poll_interval = cycles_in_micro*(float)tx_ready_sockets_poll_interval;
	app_glue_rx_ready_sockets_poll_interval = cycles_in_micro*(float)rx_ready_sockets_poll_interval;
	printf("%s %d %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64"\n",__func__,__LINE__,
			app_glue_drv_poll_interval,app_glue_timer_poll_interval,
			app_glue_tx_ready_sockets_poll_interval,app_glue_rx_ready_sockets_poll_interval);
#endif
}
uint64_t app_glue_periodic_called = 0;
uint64_t app_glue_tx_queues_process = 0;
uint64_t app_glue_rx_queues_process = 0;
/*
 * This function must be called by application periodically.
 * This is the heart of the system, it performs all the driver/IP stack work
 * and timers
 * Paramters: call_flush_queues - if non-zero, the readable, closable and writable queues
 * are processed and user's functions are called.
 * Alternatively, call_flush_queues can be 0 and the application may call
 * app_glue_get_next* functions to get readable, acceptable, closable and writable sockets
 * Returns: None
 *
 */
void app_glue_periodic(int call_flush_queues,uint8_t *ports_to_poll,int ports_to_poll_count)
{
	uint64_t ts,ts2,ts3,ts4;
    uint8_t port_idx;

	app_glue_periodic_called++;
//	ts = rte_rdtsc();	
	if((ts - app_glue_drv_last_poll_ts) >= app_glue_drv_poll_interval) {
//		ts4 = rte_rdtsc();
		for(port_idx = 0;port_idx < ports_to_poll_count;port_idx++)
		    app_glue_poll(ports_to_poll[port_idx]);
		app_glue_drv_last_poll_ts = ts;
//		working_cycles_stat += rte_rdtsc() - ts4;
	}
	ts = (app_glue_timer_last_poll_ts + app_glue_timer_poll_interval) + 1;
	if((ts - app_glue_timer_last_poll_ts) >= app_glue_timer_poll_interval) {
//		ts3 = rte_rdtsc();
		rte_timer_manage();
		app_glue_timer_last_poll_ts = ts;
//		working_cycles_stat += rte_rdtsc() - ts3;
	}
	if(call_flush_queues) {
		ts =  (app_glue_tx_ready_sockets_last_poll_ts + app_glue_tx_ready_sockets_poll_interval) + 1;
		if((ts - app_glue_tx_ready_sockets_last_poll_ts) >= app_glue_tx_ready_sockets_poll_interval) {
//			ts2 = rte_rdtsc();
			app_glue_tx_queues_process++;
			process_tx_ready_sockets();
//			working_cycles_stat += rte_rdtsc() - ts2;
			app_glue_tx_ready_sockets_last_poll_ts = ts;
		}
		ts =  (app_glue_rx_ready_sockets_last_poll_ts + app_glue_rx_ready_sockets_poll_interval) + 1;
		if((ts - app_glue_rx_ready_sockets_last_poll_ts) >= app_glue_rx_ready_sockets_poll_interval) {
//			ts2 = rte_rdtsc();
			app_glue_rx_queues_process++;
			process_rx_ready_sockets();
//			working_cycles_stat += rte_rdtsc() - ts2;
//			app_glue_rx_ready_sockets_last_poll_ts = ts;
		}
	}
	else {
		app_glue_tx_ready_sockets_last_poll_ts = ts;
		app_glue_rx_ready_sockets_last_poll_ts = ts;
	}
//	total_cycles_stat += rte_rdtsc() - ts;
	softint_run();
}
/*
 * This function may be called to attach user's data to the socket.
 * Paramters: a pointer  to socket (returned, for example, by create_*_socket)
 * a pointer to data to be attached to the socket
 * Returns: None
 *
 */
void app_glue_set_glueing_block(void *socket,void *data)
{
	struct socket *sock = socket;

	if (sock == NULL)
		return;

	sock->glueing_block = data;
}
/*
 * This function may be called to get attached to the socket user's data .
 * Paramters: a pointer  to socket (returned, for example, by create_*_socket,)
 * Returns: pointer to data to be attached to the socket
 *
 */
void *app_glue_get_glueing_block(void *socket)
{
	struct socket *sock = socket;

	if (sock == NULL)
		return NULL;

	return sock->glueing_block;
}

/*
 * This function may be called to close socket .
 * Paramters: a pointer to socket structure
 * Returns: None
 *
 */
void app_glue_close_socket(void *sk)
{
	struct socket *sock = (struct socket *)sk;
	
	if(sock->read_queue_present) {
		TAILQ_REMOVE(&read_ready_socket_list_head,sock,read_queue_entry);
		sock->read_queue_present = 0;
	}
	if(sock->write_queue_present) {
		TAILQ_REMOVE(&write_ready_socket_list_head,sock,write_queue_entry);
		sock->write_queue_present = 0;
	}
	if(sock->accept_queue_present) {
                struct socket *newsock = NULL;
#if 0
	        while(kernel_accept(sock, &newsock, 0) == 0) {
                    soclose(newsock);
                }
#endif
		TAILQ_REMOVE(&accept_ready_socket_list_head,sock,accept_queue_entry);
		sock->accept_queue_present = 0;
	}
	if(sock->closed_queue_present) {
		TAILQ_REMOVE(&closed_socket_list_head,sock,closed_queue_entry);
		sock->closed_queue_present = 0;
	}
#if 0
	if(sock->sk)
		sock->sk->sk_user_data = NULL;
#endif
	soclose(sock);
}
/*
 * This function may be called to estimate amount of data can be sent .
 * Paramters: a pointer to socket structure
 * Returns: number of bytes the application can send
 *
 */
int app_glue_calc_size_of_data_to_send(void *sock)
{
#if 0
	int bufs_count1,bufs_count2,bufs_count3,stream_space,bufs_min;
	struct sock *sk = ((struct socket *)sock)->sk;
	if(!sk_stream_is_writeable(sk)) {
		return 0;
	}
	bufs_count1 = kmem_cache_get_free(get_fclone_cache());
	bufs_count2 = kmem_cache_get_free(get_header_cache());
	bufs_count3 = get_buffer_count();
	if(bufs_count1 > 2) {
		bufs_count1 -= 2;
	}
	if(bufs_count2 > 2) {
		bufs_count2 -= 2;
	}
	bufs_min = min(bufs_count1,bufs_count2);
	bufs_min = min(bufs_min,bufs_count3);
	if(bufs_min <= 0) {
		return 0;
	}
	stream_space = sk_stream_wspace(((struct socket *)sock)->sk);
	return min(bufs_min << 10,stream_space);
#else
	return 0;
#endif
}
/*
 * This function may be called to allocate rte_mbuf from existing pool.
 * Paramters: None
 * Returns: a pointer to rte_mbuf, if succeeded, NULL if failed
 *
 */
#if 0
struct rte_mbuf *app_glue_get_buffer()
{
	return get_buffer();
}
#endif
void app_glue_print_stats()
{
#if 0
	float ratio;

	ratio = (float)(total_cycles_stat - total_prev)/(float)(working_cycles_stat - work_prev);
	total_prev = total_cycles_stat;
	work_prev = working_cycles_stat;
	printf("total %"PRIu64" work %"PRIu64" ratio %f\n",total_cycles_stat,working_cycles_stat,ratio);
	printf("app_glue_periodic_called %"PRIu64"\n",app_glue_periodic_called);
	printf("app_glue_tx_queues_process %"PRIu64"\n",app_glue_tx_queues_process);
	printf("app_glue_rx_queues_process %"PRIu64"\n",app_glue_rx_queues_process);
#endif
}

int app_glue_get_socket_type(struct socket *so)
{
	return so->so_type;
}

int app_glue_sendto(struct socket *so, void *data,int len, void *desc)
{
    struct mbuf *addr,*top;
    char *p;
    int rc;
    struct sockaddr_in *p_addr;

    addr = m_get(M_WAIT, MT_SONAME);
    if (!addr) {
	printf("cannot create socket %s %d\n",__FILE__,__LINE__);
	return NULL;
    }
    
    p = (char *)data;
    p -= sizeof(struct sockaddr_in);
    p_addr = (struct sockaddr_in *)p;
    addr->m_len = sizeof(struct sockaddr_in);
    p = mtod(addr, char *);

    memcpy(p, p_addr, sizeof(*p_addr));
    top = m_devget(data, len, 0, NULL, desc);
    if(!top) {
	m_freem(addr);
        return -1;
    }
    rc = sosend(so, addr, top, NULL, 0);
    m_freem(addr);
    return rc;
}

int app_glue_receivefrom(struct socket *so, void **buf)
{
    struct mbuf *paddr = NULL,*mp0 = NULL,*controlp = NULL;
    int flags = 0,rc;
    char *p;

    rc = soreceive( so, &paddr,&mp0, &controlp, &flags);
    if(!rc) {
	*buf = mp0->m_paddr;
	p = mtod(mp0, char *);
	mp0->m_paddr = NULL;
	memcpy(p - sizeof(struct sockaddr_in), mtod(paddr, char *), sizeof(struct sockaddr_in));
	m_freem(mp0);
	if(paddr) {
		m_freem(paddr);
	}
    }
    return rc;
}

int app_glue_send(struct socket *so, void *data,int len, void *desc)
{
    struct mbuf *top;
    int rc;

    top = m_devget(data, len, 0, NULL, desc);
    if(!top) {
        return -1;
    }
    rc = sosend(so, NULL, top,NULL, 0);
    return rc;
}

int app_glue_receive(struct socket *so,void **buf)
{
    struct mbuf *mp0 = NULL,*controlp = NULL;
    int flags = 0,rc;

    rc = soreceive( so, NULL,&mp0, &controlp, &flags);
    if(!rc) {
	*buf = mp0->m_paddr;
	mp0->m_paddr = NULL;	
	m_freem(mp0);
    }
    return rc;
}

int app_glue_setsockopt(void *so, int level, int name, size_t size, void *data)
{
	struct socket *sock = (struct socket *)so;
	struct sockopt sockoption;

	sockoption.sopt_level = level;
	sockoption.sopt_name = name;
	sockoption.sopt_size = size;
	sockoption.sopt_data = data;
	return sosetopt(so, &sockoption);
}

TAILQ_HEAD(buffers_available_notification_socket_list_head, socket) buffers_available_notification_socket_list_head;
void app_glue_process_tx_empty(void *so)
{
	struct socket *sock = (struct socket *)so;
	if(sock) {
               if(!sock->buffers_available_notification_queue_present) {
                   TAILQ_INSERT_TAIL(&buffers_available_notification_socket_list_head, sock, buffers_available_notification_queue_entry);
                   sock->buffers_available_notification_queue_present = 1;
		   if(sock->so_type == SOCK_DGRAM)
//		   	user_set_socket_tx_space(&g_service_sockets[socket_satelite_data[cmd->ringset_idx].ringset_idx].tx_space,sk_stream_wspace(socket_satelite_data[cmd->ringset_idx].socket->sk));
			;
               }
           }
}

int app_glue_is_buffers_available_waiters_empty()
{
	return TAILQ_EMPTY(&buffers_available_notification_socket_list_head);
}

void *app_glue_get_first_buffers_available_waiter()
{
	struct socket *sock = TAILQ_FIRST(&buffers_available_notification_socket_list_head);
	return sock->glueing_block;
}

void app_glue_remove_first_buffer_available_waiter(void *so)
{
	struct socket *sock = (struct socket *)so;
	sock->buffers_available_notification_queue_present = 0;
        TAILQ_REMOVE(&buffers_available_notification_socket_list_head,sock,buffers_available_notification_queue_entry);
}

void app_glue_init_buffers_available_waiters()
{
	TAILQ_INIT(&buffers_available_notification_socket_list_head);
}

struct socket *sender_so = NULL;
struct socket *receiver_so = NULL;

void user_on_accept(struct socket *so)
{
    while(so->so_qlen) {
    	struct socket *so2 = TAILQ_FIRST(&so->so_q);
	struct mbuf *addr = m_get(M_WAIT, MT_SONAME);
	
    	if (soqremque(so2, 1) == 0) {
		printf("user_on_accept\n");
		exit(1);
	}
    	soaccept(so2,addr);
	receiver_so = so2;
	so2->so_upcall2 = app_glue_so_upcall;
	user_data_available_cbk(so2);
    }
}

#define COHERENCY_UNIT 64
unsigned long hz=0;
unsigned long tick=0;
size_t  coherency_unit = COHERENCY_UNIT;
void *createInterface(int instance);
void *create_udp_socket(const char *ip_addr,unsigned short port);
void *create_client_socket(const char *my_ip_addr,unsigned short my_port,
		                   const char *peer_ip_addr,unsigned short port);
void *create_server_socket(const char *my_ip_addr,unsigned short port);
int init_device(int portid, int queue_count);
void poll_rx(void *ifp, int portid, int queue_id);
int main(int argc,char **argv)
{
    int ret;
    void *ifp;

    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        //rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
        printf("cannot initialize EAL\n");
        exit(0);
    }
    init_device(0, 1);
    softint_init();
    callout_startup(); 
    printf("%s %d\n",__FILE__,__LINE__);
    domaininit(1);
    printf("%s %d\n",__FILE__,__LINE__);
    bpf_setops();
    rt_init();
    soinit();
    mbinit();
    app_glue_init();
    rte_timer_subsystem_init();
    ifp = createInterface(0);
    app_glue_set_port_ifp(0, ifp);
    printf("%s %d %p\n",__FILE__,__LINE__,ifp);
    configure_if_addr(ifp, inet_addr("192.168.150.63"), inet_addr("255.255.255.0"));
    printf("%s %d\n",__FILE__,__LINE__);
    void *socket1,*socket2;
#if 0
    createLoopbackInterface();
    unsigned i = 0,iterations_count = 100000;

    sender_so = create_udp_socket("127.0.0.1",7777);
    printf("%s %d\n",__FILE__,__LINE__);
    receiver_so = create_udp_socket("127.0.0.1",7778);
    user_on_transmission_opportunity(sender_so); 
    while(i < iterations_count) {
	    user_on_transmission_opportunity(sender_so);
	    softint_run();
	    app_glue_periodic(1,NULL,0);
	    i++;
    }
printf("%s %d\n",__FILE__,__LINE__);
    if(sender_so) {
        app_glue_close_socket(sender_so);
    }
    if(receiver_so) {
        app_glue_close_socket(receiver_so);
    }

printf("%s %d\n",__FILE__,__LINE__);
    receiver_so = create_server_socket("127.0.0.1",7777);
     if(!receiver_so) {
        printf("cannot open server socket\n");
        return -1;
    }
    sender_so = create_client_socket("127.0.0.1",11111,"127.0.0.1",7777);
    if(!sender_so) {
        printf("cannot open client socket\n");
        return -1;
    }
#endif
#if 0
    softint_run();
    softint_run();
    softint_run();
    i = 0;
    while(i < iterations_count) {
            user_on_transmission_opportunity(sender_so);
            softint_run();
            softint_run();
            softint_run();
            app_glue_periodic(1,NULL,0);
            i++;
    }
#else
    while(1) { 
	    service_main_loop();
    }
#endif
    app_glue_close_socket(socket1);
    app_glue_close_socket(socket2);
    printf("The END\n");
    return 0;
}
