FLAGS=-Ofast 
#FLAGS=-g
gcc $FLAGS -I/usr/include/netbsddpdk -c tcp_listener_with_select.c -o tcp_listener_with_select.o
gcc tcp_listener_with_select.o -L/usr/lib/netbsddpdk -lrt -ldl -lnetbsddpdkapi -lrte_eal -lrte_ring -lrte_timer -lrte_mempool -lrte_malloc -lrte_mbuf -lrt -ldl -pthread -lnetbsddpdklog -o tcp_with_select.bin
