/*
* Emulate pipe()
*/
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
// TODO: an include for libpglite
#endif

typedef int (*pglite_pipe_t)(int pipefd[2]);
pglite_pipe_t pglite_pipe = NULL;

pglite_pipe_t EMSCRIPTEN_KEEPALIVE pgl_set_pipe_fn(pglite_pipe_t pipe_fn) {
    pglite_pipe_t prev = pglite_pipe;
    pglite_pipe = pipe_fn;
    return prev;
}

typedef struct pipe_node {
    int fd_read;
    int fd_write;
    int *pipedes;
    void *data;
    bool listening;
    bool accepted;
    struct pipe_node *next;
} pipe_node_t;

static pipe_node_t *pipe_list = NULL;

static void pipe_list_add(int fd_read, int fd_write, int *pipedes, void *data) {
    pipe_node_t *node = (pipe_node_t *)malloc(sizeof(pipe_node_t));
    node->fd_read = fd_read;
    node->fd_write = fd_write;
    node->pipedes = pipedes;
    node->data = data;
    node->listening = false;
    node->accepted = false;
    node->next = pipe_list;
    pipe_list = node;
}

pipe_node_t * EMSCRIPTEN_KEEPALIVE pipe_node_lookup(int fd) {
    for (pipe_node_t *n = pipe_list; n; n = n->next) {
        if (n->fd_read == fd || n->fd_write == fd)
            return n;
    }
    return NULL;
}

void EMSCRIPTEN_KEEPALIVE pipe_remove(int fd) {
    pipe_node_t **pp = &pipe_list;
    while (*pp) {
        if ((*pp)->fd_read == fd || (*pp)->fd_write == fd) {
            pipe_node_t *tmp = *pp;
            *pp = tmp->next;
            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

__attribute__((weak)) int	selfpipe_readfd = -1;
__attribute__((weak)) int	selfpipe_writefd = -1;
__attribute__((weak)) int	postmaster_alive_fds[2] = {-1, -1};

__attribute__((weak)) int	NumListenSockets = 0;
__attribute__((weak)) int *ListenSockets = NULL;

int EMSCRIPTEN_KEEPALIVE hlp_pipe_init_pipes() {
    int __pipedes[2];
    int res = pipe(__pipedes);
    selfpipe_readfd = __pipedes[0];
    selfpipe_writefd = __pipedes[1];
    res = pipe(postmaster_alive_fds);

    NumListenSockets = 0;
	ListenSockets = NULL;

    return res;
}

int EMSCRIPTEN_KEEPALIVE hlp_pipe_replace(int prevFd, int newFd) {
    if (prevFd == selfpipe_readfd) {
        selfpipe_readfd = newFd;
    }
    if (prevFd == selfpipe_writefd) {
        selfpipe_writefd = newFd;
    }
    if (postmaster_alive_fds[0] == prevFd) {
        postmaster_alive_fds[0] = newFd;
    }
    if (postmaster_alive_fds[1] == prevFd) {
        postmaster_alive_fds[1] = newFd;
    }
    // for (pipe_node_t *n = pipe_list; n; n = n->next) {
    //     if (n->fd_read == prevFd) {
    //         *(&n->pipedes[0]) = newFd;
    //         n->fd_read = newFd;
    //         return 0;
    //     } if (n->fd_write == prevFd) {
    //         *(&n->pipedes[1]) = newFd;
    //         n->fd_write = newFd;
    //         return 0;
    //     }
    // }
    return 1;
}

int EMSCRIPTEN_KEEPALIVE pgl_pipe(int __pipedes[2]) {
    if (pglite_pipe) {
        return pglite_pipe(__pipedes);
    }
    int res = pipe(__pipedes);
    if (res == 0) {
        pipe_list_add(__pipedes[0], __pipedes[1], __pipedes, NULL);
    }
    return res;
}

int EMSCRIPTEN_KEEPALIVE pgl_socket(int domain, int type, int protocol) {
    int p[2];
    int res = pgl_pipe(p);
    if (res == 0) {
        return p[0];
    }
    return res;
}

int EMSCRIPTEN_KEEPALIVE pgl_bind(int socket, const struct sockaddr *address, socklen_t address_len) {
    char *maddress = malloc(strlen(address->sa_data) + 1);
    strcpy(maddress, address->sa_data);
    pipe_node_t *node = pipe_node_lookup(socket);
    if (node) {
        node->data = maddress;
    }
}

int EMSCRIPTEN_KEEPALIVE pgl_listen(int socket, int backlog) {
    pipe_node_t *node = pipe_node_lookup(socket);
    if (node) {
        node->listening = true;
    } else {
        // error
        exit(69);
    }
	return 0;
}

int EMSCRIPTEN_KEEPALIVE pgl_accept(int socket, struct sockaddr *address, socklen_t *address_len) {
    int p[2];
    int res = pgl_pipe(p);
    if (res == 0) {
        pipe_node_t *node = pipe_node_lookup(p[0]);
        if (node) {
            char *data = (char *) malloc(strlen("accepted") + 1);
            memcpy(data, "accepted", strlen(data));
            node->data = data;
            node->accepted = true;
        }
        return p[0];
    }
    return res;
}

int EMSCRIPTEN_KEEPALIVE hlp_trigger_new_connection() {
    for (pipe_node_t *n = pipe_list; n; n = n->next) {
        if (n->listening) {
            char byte = 0;
            return write(n->fd_write, &byte, 1);  // wakes up poll()
        }
    }
    return -2;
}