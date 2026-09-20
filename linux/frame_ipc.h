#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace linux_mirror {
    constexpr uint32_t kMagic = 0x4f58524d;
    constexpr uint32_t kVersion = 1;
    constexpr uint64_t kMaxFrameBytes = 128ull * 1024 * 1024;
    struct FrameHeader {
        uint32_t magic = kMagic, version = kVersion;
        uint32_t width = 0, height = 0, leftWidth = 0, rightWidth = 0;
        uint32_t stride = 0, pid = 0;
        uint64_t sequence = 0, timestamp = 0;
    };
    struct Frame {
        FrameHeader header;
        std::vector<uint8_t> pixels;
    };
    uint64_t monotonicNs();
    std::string frameDirectory();
    // Nonblocking file locks prevent torn frames without stalling the game.
    class FrameWriter {
      public:
        FrameWriter() = default;
        FrameWriter(const FrameWriter&) = delete;
        FrameWriter& operator=(const FrameWriter&) = delete;
        ~FrameWriter();
        bool publish(Frame& frame);
        const std::string& path() const {
            return _path;
        }

      private:
        int _fd = -1;
        std::string _path;
        uint64_t _sequence = 0;
    };
    bool readFrame(const std::string& path, Frame& frame, uint64_t previousSequence = 0);
    std::string newestFrame(uint32_t pid = 0);
} // namespace linux_mirror
