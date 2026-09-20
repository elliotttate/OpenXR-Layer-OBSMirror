#include "frame_ipc.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace linux_mirror {
    uint64_t monotonicNs() {
        timespec time{};
        clock_gettime(CLOCK_MONOTONIC, &time);
        return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
    }
    std::string frameDirectory() {
        const char* overridePath = std::getenv("OPENXR_OBS_MIRROR_DIR");
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        return overridePath && *overridePath ? overridePath
                                             : std::string(runtime && *runtime ? runtime : "/tmp") +
                                                   "/openxr-obsmirror-" + std::to_string(getuid());
    }
    static bool validDirectory() {
        const auto path = frameDirectory();
        mkdir(path.c_str(), 0700);
        struct stat info{};
        return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == getuid() &&
               (info.st_mode & 0077) == 0;
    }
    static bool validHeader(const FrameHeader& h) {
        return h.magic == kMagic && h.version == kVersion && h.width && h.height && h.width <= 16384 &&
               h.height <= 8192 && h.leftWidth && uint64_t(h.leftWidth) + h.rightWidth == h.width &&
               h.stride == uint64_t(h.width) * 4 && uint64_t(h.stride) * h.height <= kMaxFrameBytes;
    }
    FrameWriter::~FrameWriter() {
        if (_fd >= 0) {
            unlink(_path.c_str());
            close(_fd);
        }
    }
    bool FrameWriter::publish(Frame& frame) {
        if (!validHeader(frame.header) || frame.pixels.size() != uint64_t(frame.header.stride) * frame.header.height)
            return false;
        if (_fd < 0) {
            if (!validDirectory())
                return false;
            _path = frameDirectory() + "/" + std::to_string(getpid()) + "-" + std::to_string(monotonicNs()) + ".frame";
            _fd = open(_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (_fd < 0)
                return false;
        }
        if (flock(_fd, LOCK_EX | LOCK_NB) != 0)
            return false;
        const size_t size = sizeof(FrameHeader) + frame.pixels.size();
        struct stat info{};
        // Allocate storage before mapping so a full tmpfs cannot cause SIGBUS
        // in the host game. Resizing and mapping are protected by the same lock.
        bool ok = fstat(_fd, &info) == 0;
        if (ok && info.st_size != static_cast<off_t>(size))
            ok = posix_fallocate(_fd, 0, size) == 0 && ftruncate(_fd, size) == 0;
        void* mapping = ok ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0) : MAP_FAILED;
        if (mapping != MAP_FAILED) {
            frame.header.pid = getpid();
            frame.header.timestamp = monotonicNs();
            frame.header.sequence = ++_sequence;
            std::memcpy(mapping, &frame.header, sizeof(frame.header));
            std::memcpy(
                static_cast<uint8_t*>(mapping) + sizeof(frame.header), frame.pixels.data(), frame.pixels.size());
            munmap(mapping, size);
        }
        flock(_fd, LOCK_UN);
        return mapping != MAP_FAILED;
    }
    bool readFrame(const std::string& path, Frame& frame, uint64_t previousSequence) {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            return false;
        struct stat info{};
        bool locked = flock(fd, LOCK_SH | LOCK_NB) == 0;
        bool ok = locked && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == getuid() &&
                  info.st_size >= static_cast<off_t>(sizeof(FrameHeader)) &&
                  info.st_size <= static_cast<off_t>(sizeof(FrameHeader) + kMaxFrameBytes);
        if (ok) {
            const void* mapping = mmap(nullptr, info.st_size, PROT_READ, MAP_SHARED, fd, 0);
            ok = mapping != MAP_FAILED;
            if (ok) {
                FrameHeader header;
                std::memcpy(&header, mapping, sizeof(header));
                const auto now = monotonicNs();
                ok = validHeader(header) && header.sequence != previousSequence && header.timestamp <= now &&
                     now - header.timestamp < 3000000000ull &&
                     uint64_t(header.stride) * header.height + sizeof(header) == uint64_t(info.st_size);
                if (ok) {
                    frame.header = header;
                    try {
                        const auto* bytes = static_cast<const uint8_t*>(mapping) + sizeof(header);
                        frame.pixels.assign(bytes, bytes + size_t(header.stride) * header.height);
                    } catch (...) {
                        ok = false;
                    }
                }
                munmap(const_cast<void*>(mapping), info.st_size);
            }
        }
        if (locked)
            flock(fd, LOCK_UN);
        close(fd);
        return ok;
    }
    std::string newestFrame(uint32_t pid) {
        if (!validDirectory())
            return {};
        std::error_code error;
        std::string result;
        uint64_t newest = 0;
        const auto now = monotonicNs();
        const std::string prefix = std::to_string(pid) + "-";
        for (const auto& item : std::filesystem::directory_iterator(frameDirectory(), error)) {
            const auto name = item.path().filename().string();
            if (item.path().extension() != ".frame" || (pid && name.rfind(prefix, 0) != 0))
                continue;
            const int fd = open(item.path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0)
                continue;
            FrameHeader header{};
            struct stat info{};
            const bool locked = flock(fd, LOCK_SH | LOCK_NB) == 0;
            const bool valid = locked && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == getuid() &&
                               pread(fd, &header, sizeof(header), 0) == sizeof(header) && validHeader(header) &&
                               header.timestamp <= now && now - header.timestamp < 3000000000ull;
            if (locked)
                flock(fd, LOCK_UN);
            close(fd);
            if (valid && header.timestamp > newest) {
                result = item.path().string();
                newest = header.timestamp;
            }
        }
        return result;
    }
} // namespace linux_mirror
