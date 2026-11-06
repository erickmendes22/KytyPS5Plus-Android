#ifndef KYTY_KERNEL_SPARSE_BACKING_READ_H_
#define KYTY_KERNEL_SPARSE_BACKING_READ_H_

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace Libs::LibKernel::Memory::SparseBackingRead {

inline bool ReadAt(int fd, uint8_t* output, uint64_t offset, uint64_t bytes) {
    while (bytes != 0) {
        const auto count = pread(fd, output,
            static_cast<size_t>(std::min<uint64_t>(bytes, 1024 * 1024)),
            static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        output += count;
        offset += static_cast<uint64_t>(count);
        bytes -= static_cast<uint64_t>(count);
    }
    return true;
}

// fd belongs to the fixed-size shared backing store. All data transfers use explicit
// offsets, so concurrent SEEK_DATA/SEEK_HOLE calls never consume another reader's position.
inline bool Read(int fd, uint8_t* output, uint64_t offset, uint64_t bytes, uint64_t file_size) {
    if (output == nullptr || fd < 0 || offset > file_size || bytes > file_size - offset)
        return false;
    const auto end = offset + bytes;
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    while (offset < end) {
        off_t data;
        do { data = lseek(fd, static_cast<off_t>(offset), SEEK_DATA); }
        while (data < 0 && errno == EINTR);
        if (data < 0) {
            if (errno == ENXIO) {
                std::memset(output, 0, end - offset);
                return true;
            }
            if (errno == EINVAL || errno == ENOTSUP || errno == ENOSYS)
                return ReadAt(fd, output, offset, end - offset);
            return false;
        }
        const auto begin = std::min<uint64_t>(static_cast<uint64_t>(data), end);
        if (begin < offset) return false;
        if (begin != offset) {
            std::memset(output, 0, begin - offset);
            output += begin - offset;
            offset = begin;
        }
        if (offset == end) break;
        off_t hole;
        do { hole = lseek(fd, data, SEEK_HOLE); } while (hole < 0 && errno == EINTR);
        if (hole < 0) {
            if (errno == EINVAL || errno == ENOTSUP || errno == ENOSYS)
                return ReadAt(fd, output, offset, end - offset);
            return false;
        }
        const auto finish = std::min<uint64_t>(static_cast<uint64_t>(hole), end);
        if (finish <= offset || !ReadAt(fd, output, offset, finish - offset)) return false;
        output += finish - offset;
        offset = finish;
    }
    return true;
#else
    return ReadAt(fd, output, offset, bytes);
#endif
}

} // namespace Libs::LibKernel::Memory::SparseBackingRead
#endif
