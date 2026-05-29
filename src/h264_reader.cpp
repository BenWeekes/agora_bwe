#include "h264_reader.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logging.h"

namespace bwe {

H264Reader::~H264Reader() {
  if (buffer_ && buffer_ != MAP_FAILED) {
    munmap(buffer_, size_);
  }
}

bool H264Reader::open(const std::string& path) {
  path_ = path;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG_ERROR("h264_reader: cannot open %s", path.c_str());
    return false;
  }
  struct stat sb;
  if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
    LOG_ERROR("h264_reader: fstat failed or empty file %s", path.c_str());
    ::close(fd);
    return false;
  }
  void* mapped = mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (mapped == MAP_FAILED) {
    LOG_ERROR("h264_reader: mmap failed for %s", path.c_str());
    return false;
  }
  buffer_ = static_cast<uint8_t*>(mapped);
  size_ = static_cast<size_t>(sb.st_size);
  index_frames();
  if (frames_.empty()) {
    LOG_ERROR("h264_reader: no frames parsed from %s", path.c_str());
    return false;
  }
  LOG_INFO("h264_reader: %s -> %zu frames (%zu keyframes)", path.c_str(), frames_.size(), [&] {
    size_t k = 0;
    for (auto& f : frames_) k += f.is_keyframe;
    return k;
  }());
  return true;
}

namespace {
// Returns the byte length of the Annex-B start code at offset p (3 or 4), or 0 if none.
inline int start_code_len(const uint8_t* b, size_t size, size_t p) {
  if (p + 3 <= size && b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 1) return 3;
  if (p + 4 <= size && b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 0 && b[p + 3] == 1) return 4;
  return 0;
}

// Decode the first unsigned exp-Golomb value (ue(v)) starting at the given bit
// offset of buf. Used to read first_mb_in_slice from a slice header. (The value
// sits at the very start of the slice RBSP, before any byte that could carry an
// emulation-prevention sequence, so we can read raw.)
inline uint32_t read_ue(const uint8_t* buf, size_t size) {
  int total_bits = static_cast<int>(size) * 8;
  int leading_zeros = 0;
  int bit = 0;
  while (bit < total_bits) {
    int byte = bit >> 3, off = 7 - (bit & 7);
    if ((buf[byte] >> off) & 1) break;
    leading_zeros++;
    bit++;
  }
  bit++;  // skip the terminating 1
  uint32_t value = 0;
  for (int i = 0; i < leading_zeros && bit < total_bits; ++i, ++bit) {
    int byte = bit >> 3, off = 7 - (bit & 7);
    value = (value << 1) | ((buf[byte] >> off) & 1);
  }
  return (1u << leading_zeros) - 1 + value;
}
}  // namespace

void H264Reader::index_frames() {
  // Pass 1: locate every NAL unit (start-code offset, payload offset, nal type).
  struct Nal {
    size_t start;         // offset of the start code
    size_t payload;       // offset of the byte after the NAL header
    uint8_t type;
  };
  std::vector<Nal> nals;
  for (size_t i = 0; i + 3 <= size_;) {
    int sc = start_code_len(buffer_, size_, i);
    if (sc == 0) {
      i++;
      continue;
    }
    uint8_t type = buffer_[i + sc] & 0x1f;
    nals.push_back({i, i + sc + 1, type});
    i += sc + 1;
  }

  // Pass 2: group NALs into access units (one coded picture each). Frames may be
  // multi-slice (libx264 sliced-threads), so a VCL NAL alone does NOT end a frame.
  // A new access unit begins when, after we've already collected a VCL slice for
  // the current picture, we see either:
  //   - a VCL slice with first_mb_in_slice == 0 (the first slice of the next
  //     picture), or
  //   - a non-VCL NAL (SPS=7, PPS=8, SEI=6, AUD=9) that introduces the next picture.
  // A picture is a keyframe if any of its slices is an IDR slice (type 5).
  frames_.clear();
  size_t au_start = 0;
  bool au_open = false;
  bool au_has_vcl = false;
  bool au_has_idr = false;

  auto close_au = [&](size_t end) {
    if (au_open && au_has_vcl) frames_.push_back({au_start, end - au_start, au_has_idr});
  };

  for (size_t n = 0; n < nals.size(); ++n) {
    const Nal& nal = nals[n];
    bool is_vcl = (nal.type == 1 || nal.type == 5);

    bool starts_new_au = false;
    if (au_open && au_has_vcl) {
      if (is_vcl) {
        uint32_t first_mb = read_ue(buffer_ + nal.payload, size_ - nal.payload);
        if (first_mb == 0) starts_new_au = true;
      } else {
        starts_new_au = true;  // param sets / SEI / AUD for the next picture
      }
    }

    if (starts_new_au) {
      close_au(nal.start);
      au_open = false;
    }
    if (!au_open) {
      au_start = nal.start;
      au_open = true;
      au_has_vcl = false;
      au_has_idr = false;
    }
    if (is_vcl) au_has_vcl = true;
    if (nal.type == 5) au_has_idr = true;
  }
  close_au(size_);
}

bool H264Reader::frame_at(size_t index, Frame& out) const {
  if (index >= frames_.size()) return false;
  const AccessUnit& au = frames_[index];
  out.data = buffer_ + au.offset;
  out.length = au.length;
  out.is_keyframe = au.is_keyframe;
  return true;
}

}  // namespace bwe
