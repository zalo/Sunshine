/**
 * @file src/webrtc/video_sender.cpp
 * @brief Video sender implementation for WebRTC streaming.
 */

#include "video_sender.h"

#include <cstring>
#include <random>

#include "peer.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/thread_safe.h"
#include "src/video.h"

namespace webrtc {

  VideoSender &
  VideoSender::instance() {
    static VideoSender instance;
    return instance;
  }

  void
  VideoSender::init() {
    // Generate random SSRC
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    ssrc_ = dist(gen);

    BOOST_LOG(info) << "WebRTC video sender initialized (SSRC: " << ssrc_ << ")";
  }

  void
  VideoSender::start() {
    if (running_.load()) {
      return;
    }

    running_.store(true);
    sender_thread_ = std::thread(&VideoSender::sender_loop, this);

    BOOST_LOG(info) << "WebRTC video sender started";
  }

  void
  VideoSender::stop() {
    if (!running_.load()) {
      return;
    }

    running_.store(false);

    if (sender_thread_.joinable()) {
      sender_thread_.join();
    }

    BOOST_LOG(info) << "WebRTC video sender stopped";
  }

  void
  VideoSender::set_codec(VideoCodec codec) {
    codec_ = codec;
    params_.codec = codec;
  }

  VideoSender::VideoParams
  VideoSender::get_params() const {
    return params_;
  }

  VideoSender::Stats
  VideoSender::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  void
  VideoSender::sender_loop() {
    BOOST_LOG(info) << "WebRTC video sender loop started";

    // Subscribe to the video packet queue
    // This is the same queue that the regular streaming uses
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);

    while (running_.load()) {
      // Wait for and get the next video packet
      auto packet = packets->pop();

      if (!packet) {
        // Queue was stopped or empty with timeout
        if (!running_.load()) {
          break;
        }
        continue;
      }

      // Only process if we have connected peers
      if (PeerManager::instance().connected_count() == 0) {
        continue;
      }

      // Process and send the packet via WebRTC
      process_packet(packet);
    }

    BOOST_LOG(info) << "WebRTC video sender loop ended";
  }

  void
  VideoSender::process_packet(video::packet_t &packet) {
    if (!packet || !running_.load()) {
      return;
    }

    uint8_t *data = packet->data();
    size_t size = packet->data_size();
    bool is_keyframe = packet->is_idr();
    uint32_t timestamp = static_cast<uint32_t>(packet->frame_index() * 3000);  // 90kHz clock for video

    // Packetize based on codec
    switch (codec_) {
      case VideoCodec::H264:
        packetize_h264(data, size, timestamp, is_keyframe);
        break;
      case VideoCodec::HEVC:
        packetize_hevc(data, size, timestamp, is_keyframe);
        break;
      case VideoCodec::AV1:
        packetize_av1(data, size, timestamp, is_keyframe);
        break;
    }

    // Update stats
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.frames_sent++;
      stats_.bytes_sent += size;
      if (is_keyframe) {
        stats_.key_frames_sent++;
      }
      stats_.avg_frame_size = static_cast<double>(stats_.bytes_sent) / stats_.frames_sent;
    }
  }

  void
  VideoSender::packetize_h264(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe) {
    // H.264 NAL unit packetization for RTP (RFC 6184)
    // We use FU-A fragmentation for NAL units larger than MAX_RTP_PAYLOAD

    size_t offset = 0;

    while (offset < size) {
      // Find NAL unit start (0x00 0x00 0x01 or 0x00 0x00 0x00 0x01)
      size_t nal_start = offset;
      size_t nal_end = size;

      // Skip start code
      if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0) {
        if (data[offset + 2] == 1) {
          nal_start = offset + 3;
        }
        else if (data[offset + 2] == 0 && data[offset + 3] == 1) {
          nal_start = offset + 4;
        }
      }

      // Find next NAL unit or end
      for (size_t i = nal_start; i < size - 3; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1))) {
          nal_end = i;
          break;
        }
      }

      size_t nal_size = nal_end - nal_start;

      if (nal_size <= MAX_RTP_PAYLOAD) {
        // Single NAL unit packet
        std::vector<std::byte> rtp_packet(12 + nal_size);

        // RTP header
        rtp_packet[0] = std::byte{0x80};  // Version 2
        rtp_packet[1] = std::byte{96};     // Payload type (dynamic)
        if (nal_end >= size) {
          rtp_packet[1] = std::byte{96 | 0x80};  // Marker bit for last packet of frame
        }

        // Sequence number (big-endian)
        rtp_packet[2] = std::byte((sequence_number_ >> 8) & 0xFF);
        rtp_packet[3] = std::byte(sequence_number_ & 0xFF);
        sequence_number_++;

        // Timestamp (big-endian)
        rtp_packet[4] = std::byte((timestamp >> 24) & 0xFF);
        rtp_packet[5] = std::byte((timestamp >> 16) & 0xFF);
        rtp_packet[6] = std::byte((timestamp >> 8) & 0xFF);
        rtp_packet[7] = std::byte(timestamp & 0xFF);

        // SSRC (big-endian)
        rtp_packet[8] = std::byte((ssrc_ >> 24) & 0xFF);
        rtp_packet[9] = std::byte((ssrc_ >> 16) & 0xFF);
        rtp_packet[10] = std::byte((ssrc_ >> 8) & 0xFF);
        rtp_packet[11] = std::byte(ssrc_ & 0xFF);

        // NAL unit
        std::memcpy(&rtp_packet[12], &data[nal_start], nal_size);

        broadcast_rtp_packet(rtp_packet.data(), rtp_packet.size(), timestamp);
      }
      else {
        // FU-A fragmentation
        uint8_t nal_header = data[nal_start];
        uint8_t nal_type = nal_header & 0x1F;
        uint8_t nri = nal_header & 0x60;

        size_t frag_offset = 1;  // Skip NAL header
        bool first = true;

        while (frag_offset < nal_size) {
          size_t frag_size = std::min(MAX_RTP_PAYLOAD - 2, nal_size - frag_offset);
          bool last = (frag_offset + frag_size >= nal_size);

          std::vector<std::byte> rtp_packet(12 + 2 + frag_size);

          // RTP header
          rtp_packet[0] = std::byte{0x80};
          rtp_packet[1] = std::byte{96};
          if (last && nal_end >= size) {
            rtp_packet[1] = std::byte{96 | 0x80};  // Marker bit
          }

          rtp_packet[2] = std::byte((sequence_number_ >> 8) & 0xFF);
          rtp_packet[3] = std::byte(sequence_number_ & 0xFF);
          sequence_number_++;

          rtp_packet[4] = std::byte((timestamp >> 24) & 0xFF);
          rtp_packet[5] = std::byte((timestamp >> 16) & 0xFF);
          rtp_packet[6] = std::byte((timestamp >> 8) & 0xFF);
          rtp_packet[7] = std::byte(timestamp & 0xFF);

          rtp_packet[8] = std::byte((ssrc_ >> 24) & 0xFF);
          rtp_packet[9] = std::byte((ssrc_ >> 16) & 0xFF);
          rtp_packet[10] = std::byte((ssrc_ >> 8) & 0xFF);
          rtp_packet[11] = std::byte(ssrc_ & 0xFF);

          // FU indicator (type 28 = FU-A)
          rtp_packet[12] = std::byte(nri | 28);

          // FU header
          uint8_t fu_header = nal_type;
          if (first) fu_header |= 0x80;  // Start bit
          if (last) fu_header |= 0x40;   // End bit
          rtp_packet[13] = std::byte(fu_header);

          // Fragment data
          std::memcpy(&rtp_packet[14], &data[nal_start + frag_offset], frag_size);

          broadcast_rtp_packet(rtp_packet.data(), rtp_packet.size(), timestamp);

          frag_offset += frag_size;
          first = false;
        }
      }

      offset = nal_end;
    }
  }

  void
  VideoSender::packetize_hevc(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe) {
    // HEVC/H.265 packetization for RTP (RFC 7798)
    // Similar structure to H.264 but with different NAL unit format

    size_t offset = 0;

    while (offset < size) {
      size_t nal_start = offset;
      size_t nal_end = size;

      // Skip start code
      if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0) {
        if (data[offset + 2] == 1) {
          nal_start = offset + 3;
        }
        else if (data[offset + 2] == 0 && data[offset + 3] == 1) {
          nal_start = offset + 4;
        }
      }

      // Find next NAL unit
      for (size_t i = nal_start; i < size - 3; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1))) {
          nal_end = i;
          break;
        }
      }

      size_t nal_size = nal_end - nal_start;

      if (nal_size <= MAX_RTP_PAYLOAD) {
        // Single NAL unit packet
        std::vector<std::byte> rtp_packet(12 + nal_size);

        // RTP header
        rtp_packet[0] = std::byte{0x80};
        rtp_packet[1] = std::byte{96};
        if (nal_end >= size) {
          rtp_packet[1] = std::byte{96 | 0x80};
        }

        rtp_packet[2] = std::byte((sequence_number_ >> 8) & 0xFF);
        rtp_packet[3] = std::byte(sequence_number_ & 0xFF);
        sequence_number_++;

        rtp_packet[4] = std::byte((timestamp >> 24) & 0xFF);
        rtp_packet[5] = std::byte((timestamp >> 16) & 0xFF);
        rtp_packet[6] = std::byte((timestamp >> 8) & 0xFF);
        rtp_packet[7] = std::byte(timestamp & 0xFF);

        rtp_packet[8] = std::byte((ssrc_ >> 24) & 0xFF);
        rtp_packet[9] = std::byte((ssrc_ >> 16) & 0xFF);
        rtp_packet[10] = std::byte((ssrc_ >> 8) & 0xFF);
        rtp_packet[11] = std::byte(ssrc_ & 0xFF);

        std::memcpy(&rtp_packet[12], &data[nal_start], nal_size);

        broadcast_rtp_packet(rtp_packet.data(), rtp_packet.size(), timestamp);
      }
      else {
        // Fragmentation Unit (FU) for HEVC
        uint16_t nal_header = (data[nal_start] << 8) | data[nal_start + 1];
        uint8_t nal_type = (nal_header >> 9) & 0x3F;
        uint8_t layer_id = nal_header & 0x1F8;
        uint8_t tid = nal_header & 0x7;

        size_t frag_offset = 2;  // Skip 2-byte NAL header
        bool first = true;

        while (frag_offset < nal_size) {
          size_t frag_size = std::min(MAX_RTP_PAYLOAD - 3, nal_size - frag_offset);
          bool last = (frag_offset + frag_size >= nal_size);

          std::vector<std::byte> rtp_packet(12 + 3 + frag_size);

          // RTP header
          rtp_packet[0] = std::byte{0x80};
          rtp_packet[1] = std::byte{96};
          if (last && nal_end >= size) {
            rtp_packet[1] = std::byte{96 | 0x80};
          }

          rtp_packet[2] = std::byte((sequence_number_ >> 8) & 0xFF);
          rtp_packet[3] = std::byte(sequence_number_ & 0xFF);
          sequence_number_++;

          rtp_packet[4] = std::byte((timestamp >> 24) & 0xFF);
          rtp_packet[5] = std::byte((timestamp >> 16) & 0xFF);
          rtp_packet[6] = std::byte((timestamp >> 8) & 0xFF);
          rtp_packet[7] = std::byte(timestamp & 0xFF);

          rtp_packet[8] = std::byte((ssrc_ >> 24) & 0xFF);
          rtp_packet[9] = std::byte((ssrc_ >> 16) & 0xFF);
          rtp_packet[10] = std::byte((ssrc_ >> 8) & 0xFF);
          rtp_packet[11] = std::byte(ssrc_ & 0xFF);

          // HEVC FU header (type 49)
          rtp_packet[12] = std::byte((49 << 1) | (layer_id >> 5));
          rtp_packet[13] = std::byte(((layer_id & 0x1F) << 3) | tid);

          // FU header
          uint8_t fu_header = nal_type;
          if (first) fu_header |= 0x80;
          if (last) fu_header |= 0x40;
          rtp_packet[14] = std::byte(fu_header);

          std::memcpy(&rtp_packet[15], &data[nal_start + frag_offset], frag_size);

          broadcast_rtp_packet(rtp_packet.data(), rtp_packet.size(), timestamp);

          frag_offset += frag_size;
          first = false;
        }
      }

      offset = nal_end;
    }
  }

  void
  VideoSender::packetize_av1(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe) {
    // AV1 packetization for RTP (RFC 9698 / draft)
    // AV1 uses OBUs (Open Bitstream Units)

    // For simplicity, we send the entire frame as a single RTP packet if it fits,
    // otherwise we fragment it

    if (size <= MAX_RTP_PAYLOAD - 1) {
      // Single OBU element
      std::vector<std::byte> rtp_packet(12 + 1 + size);

      // RTP header
      rtp_packet[0] = std::byte{0x80};
      rtp_packet[1] = std::byte{96 | 0x80};  // Marker bit (end of frame)

      rtp_packet[2] = std::byte((sequence_number_ >> 8) & 0xFF);
      rtp_packet[3] = std::byte(sequence_number_ & 0xFF);
      sequence_number_++;

      rtp_packet[4] = std::byte((timestamp >> 24) & 0xFF);
      rtp_packet[5] = std::byte((timestamp >> 16) & 0xFF);
      rtp_packet[6] = std::byte((timestamp >> 8) & 0xFF);
      rtp_packet[7] = std::byte(timestamp & 0xFF);

      rtp_packet[8] = std::byte((ssrc_ >> 24) & 0xFF);
      rtp_packet[9] = std::byte((ssrc_ >> 16) & 0xFF);
      rtp_packet[10] = std::byte((ssrc_ >> 8) & 0xFF);
      rtp_packet[11] = std::byte(ssrc_ & 0xFF);

      // AV1 aggregation header
      // Z=0 (not continuation), Y=0 (no new coded video sequence), W=1 (one OBU), N=0
      rtp_packet[12] = std::byte{0x10};
      if (is_keyframe) {
        rtp_packet[12] = std::byte{0x18};  // N=1 for new temporal unit
      }

      std::memcpy(&rtp_packet[13], data, size);

      broadcast_rtp_packet(rtp_packet.data(), rtp_packet.size(), timestamp);
    }
    else {
      // Fragmentation required
      size_t offset = 0;
      bool first = true;

      while (offset < size) {
        size_t frag_size = std::min(MAX_RTP_PAYLOAD - 1, size - offset);
        bool last = (offset + frag_size >= size);

        std::vector<std::byte> rtp_packet(12 + 1 + frag_size);

        // RTP header
        rtp_packet[0] = std::byte{0x80};
        rtp_packet[1] = std::byte{96};
        if (last) {
          rtp_packet[1] = std::byte{96 | 0x80};
        }

        rtp_packet[2] = std::byte((sequence_number_ >> 8) & 0xFF);
        rtp_packet[3] = std::byte(sequence_number_ & 0xFF);
        sequence_number_++;

        rtp_packet[4] = std::byte((timestamp >> 24) & 0xFF);
        rtp_packet[5] = std::byte((timestamp >> 16) & 0xFF);
        rtp_packet[6] = std::byte((timestamp >> 8) & 0xFF);
        rtp_packet[7] = std::byte(timestamp & 0xFF);

        rtp_packet[8] = std::byte((ssrc_ >> 24) & 0xFF);
        rtp_packet[9] = std::byte((ssrc_ >> 16) & 0xFF);
        rtp_packet[10] = std::byte((ssrc_ >> 8) & 0xFF);
        rtp_packet[11] = std::byte(ssrc_ & 0xFF);

        // AV1 aggregation header for fragmentation
        uint8_t agg_header = 0;
        if (!first) agg_header |= 0x80;  // Z=1 (continuation)
        if (!last) agg_header |= 0x40;   // Y=1 (not last fragment)
        if (first && is_keyframe) agg_header |= 0x08;  // N=1
        rtp_packet[12] = std::byte{agg_header};

        std::memcpy(&rtp_packet[13], &data[offset], frag_size);

        broadcast_rtp_packet(rtp_packet.data(), rtp_packet.size(), timestamp);

        offset += frag_size;
        first = false;
      }
    }
  }

  void
  VideoSender::broadcast_rtp_packet(const std::byte *rtp_data, size_t size, uint32_t timestamp) {
    PeerManager::instance().broadcast_video(rtp_data, size, timestamp);
  }

}  // namespace webrtc
