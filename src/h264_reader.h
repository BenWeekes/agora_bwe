#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bwe {

// Indexes one Annex-B H.264 elementary stream (a single bitrate level).
//
// On open() the whole file is mmap'd and split into access units (one decodable
// picture each). Because every level is encoded from the same source at the same
// fps and GOP (and with repeat-headers=1), frame index N in level A corresponds to
// the same source-timeline position as frame index N in level B, and keyframes land
// at identical indices across all levels. That lets the player switch levels simply
// by continuing to the next keyframe index in the new level's reader.
class H264Reader {
 public:
  struct Frame {
    const uint8_t* data;  // points into the mmap'd buffer (valid for reader lifetime)
    size_t length;        // includes start codes + SPS/PPS/SEI for keyframes
    bool is_keyframe;
  };

  H264Reader() = default;
  ~H264Reader();

  H264Reader(const H264Reader&) = delete;
  H264Reader& operator=(const H264Reader&) = delete;

  // mmap the file and index access units. Returns false on failure.
  bool open(const std::string& path);

  size_t frame_count() const { return frames_.size(); }
  bool is_keyframe(size_t index) const {
    return index < frames_.size() && frames_[index].is_keyframe;
  }

  // Access a frame by index. Returns false if out of range.
  bool frame_at(size_t index, Frame& out) const;

  const std::string& path() const { return path_; }

 private:
  struct AccessUnit {
    size_t offset;  // byte offset into buffer_
    size_t length;
    bool is_keyframe;
  };

  void index_frames();

  std::string path_;
  uint8_t* buffer_ = nullptr;
  size_t size_ = 0;
  std::vector<AccessUnit> frames_;
};

}  // namespace bwe
