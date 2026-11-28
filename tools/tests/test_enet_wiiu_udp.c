/* Exercise the Wii U UDP adapter against real localhost sockets on the host. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#ifdef ENET_TEST_WRAP_RECV
ssize_t __real_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t __wrap_recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *from, socklen_t *length) {
    assert(((uintptr_t)data & 63) == 0);
    assert((size & 63) == 0);
    return __real_recvfrom(fd, data, size, flags, from, length);
}
#endif
#include "enet/enet.h"

int main(void)
{
    ENetSocket receiver = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetSocket sender = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    assert(receiver >= 0 && sender >= 0);
    ENetAddress local = {ENET_HOST_ANY, 0}, source;
    assert(enet_socket_bind(receiver, &local) == 0);
    assert(enet_socket_get_address(receiver, &local) == 0);
    assert(enet_address_set_host_ip(&local, "127.0.0.1") == 0);
    char a[] = "hello", b[] = " world", result[32] = {0};
    ENetBuffer send_buffers[] = {{a, 5}, {b, 6}};
    ENetBuffer receive_buffers[] = {{result, 3}, {result + 3, sizeof(result) - 3}};
    assert(enet_socket_send(sender, &local, send_buffers, 2) == 11);
    assert(enet_socket_receive(receiver, &source, receive_buffers, 2) == 11);
    assert(memcmp(result, "hello world", 11) == 0);
    assert(source.port != 0);
    /* Oversize datagrams must be discarded, never accepted as valid ENet data. */
    assert(enet_socket_send(sender, &local, send_buffers, 2) == 11);
    ENetBuffer small = {result, 4};
    assert(enet_socket_receive(receiver, &source, &small, 1) == -2);
    /* ENet drains the socket before sending queued room messages. Never block
       here, even if the platform did not apply the socket-wide option. */
    assert(enet_socket_receive(receiver, &source, receive_buffers, 2) == 0);
    assert(enet_socket_set_option(receiver, ENET_SOCKOPT_NONBLOCK, 1) == 0);
    assert(enet_socket_receive(receiver, &source, receive_buffers, 2) == 0);
    ENetBuffer oversized = {a, 65508};
    assert(enet_socket_send(sender, &local, &oversized, 1) == -1);
    enet_socket_destroy(sender);
    enet_socket_destroy(receiver);
    return 0;
}
