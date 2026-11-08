// mmap for the one thing the runtime wants it for: reserving the guest's address space. See win_compat.cpp.
#pragma once
#include <cstddef>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t size, int prot, int flags, int fd, long long at);
