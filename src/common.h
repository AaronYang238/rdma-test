#ifndef RDMA_DEMO_COMMON_H
#define RDMA_DEMO_COMMON_H

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEMO_DATA_SIZE 1024

struct control_message {
    uint64_t addr;
    uint32_t rkey;
    uint32_t size;
    char note[64];
};

static inline void die_perror(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static inline void die_rdma(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static inline void check_zero(int rc, const char *what)
{
    if (rc) {
        die_rdma(what);
    }
}

#endif
