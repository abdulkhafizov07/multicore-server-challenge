#include <stdio.h>
#include <liburing.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define QUEUE_DEPTH 256
#define BUFFER_SIZE 4096

enum operation_type {
    OP_ACCEPT,
    OP_READ,
    OP_WRITE
};

struct request {
    enum operation_type type;
    int fd;

    size_t length;
    size_t offset;

    char buffer[BUFFER_SIZE];
};

static struct io_uring ring;
static int server_fd;

static void submit_accept(struct request *req) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        perror("io_uring_get_sqe");
        exit(EXIT_FAILURE);
    }

    io_uring_prep_accept(sqe, server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

    int ret = io_uring_submit(&ring);
    if (ret < 0) {
        fprintf(stderr, "submit accept: %s\n", strerror(-ret));
        exit(EXIT_FAILURE);
    }
}

static void submit_read(struct request *req) {
    req->type = OP_READ;
    req->length = 0;
    req->offset = 0;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        perror("io_uring_get_sqe");
        exit(EXIT_FAILURE);
    }

    io_uring_prep_recv(sqe, req->fd, req->buffer, sizeof(req->buffer), 0);
    io_uring_sqe_set_data(sqe, req);
}

static void submit_write(struct request *req) {
    req->type = OP_WRITE;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        perror("io_uring_get_sqe");
        exit(EXIT_FAILURE);
    }

    io_uring_prep_send(sqe, req->fd, req->buffer + req->offset, req->length - req->offset, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, req);
}

int main(void) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring init: %s\n", strerror(-ret));
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct request *accept_req = calloc(1, sizeof(*accept_req));
    if (!accept_req) {
        perror("calloc");
        io_uring_queue_exit(&ring);
        close(server_fd);
        return EXIT_FAILURE;
    }

    accept_req->type = OP_ACCEPT;
    submit_accept(accept_req);

    printf("Listening on port %d\n", PORT);

    for (;;) {
        struct io_uring_cqe *cqe;

        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;

            fprintf(stderr, "wait: %s\n", strerror(-ret));
            break;
        }

        struct request *req = io_uring_cqe_get_data(cqe);

        int result = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (!req) continue;

        switch (req->type) {
        case OP_ACCEPT:
            {
                if (result >= 0) {
                    int client_fd = result;

                    struct request *client_req = calloc(1, sizeof(*client_req));
                    if (!client_req) {
                        perror("calloc client");
                        close(client_fd);
                    } else {
                        client_req->fd = client_fd;
                        submit_read(client_req);
                    }
                } else if (result != -EAGAIN && result != -EINTR) {
                    fprintf(stderr, "accept: %s\n", strerror(-result));
                }

                submit_accept(req);
                break;
            }
        case OP_READ:
            {
                if (result > 0) {
                    req->length = (size_t)result;
                    req->offset = 0;
                    submit_write(req);
                } else if (result == -EAGAIN || result == -EINTR) {
                    submit_read(req);
                } else {
                    close(req->fd);
                    free(req);
                }
                break;
            }
        case OP_WRITE:
            {
                if (result > 0) {
                    req->offset += (size_t)result;

                    if (req->offset < req->length) {
                        // Handle partial writes
                        submit_write(req);
                    } else {
                        submit_read(req);
                    }
                } else if (result == -EAGAIN || result == -EINTR) {
                    submit_write(req);
                } else {
                    close(req->fd);
                    free(req);
                }
                break;
            }
        }

        ret = io_uring_submit(&ring);
        if (ret < 0) {
            fprintf(stderr, "submit: %s\n", strerror(-ret));
            break;
        }
    }

    // Exit whole program
    free(accept_req);
    io_uring_queue_exit(&ring);
    close(server_fd);

    return EXIT_FAILURE;
}

