/**
 * @file src/webrtc/video_sender.h
 * @brief Video packet handling for WebRTC streaming.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace video {
  struct packet_raw_t;
  using packet_t = std::unique_ptr<packet_raw_t>;
}  // namespace video

namespace webrtc {

  /**
   * @brief Video codec types supported.
   */
  enum class VideoCodec {
    H264,
    HEVC,
    AV1
  };

  /**
   * @brief RTP packetizer for H.264/HEVC/AV1 video.
   *
   * Takes encoded video frames and packetizes them into RTP packets
   * suitable for WebRTC transmission.
   */
  class VideoSender {
  public:
    static VideoSender &
    instance();

    /**
     * @brief Initialize the video sender.
     * Subscribes to Sunshine's video packet queue.
     */
    void
    init();

    /**
     * @brief Start the video sender thread.
     */
    void
    start();

    /**
     * @brief Stop the video sender.
     */
    void
    stop();

    /**
     * @brief Check if video is being sent.
     */
    bool
    is_running() const {
      return running_.load();
    }

    /**
     * @brief Set the video codec.
     * Must be called before starting.
     */
    void
    set_codec(VideoCodec codec);

    /**
     * @brief Get the current codec.
     */
    VideoCodec
    codec() const {
      return codec_;
    }

    /**
     * @brief Get the current video parameters.
     */
    struct VideoParams {
      int width = 0;
      int height = 0;
      int framerate = 0;
      int bitrate = 0;
      VideoCodec codec = VideoCodec::H264;
    };

    VideoParams
    get_params() const;

    /**
     * @brief Get statistics.
     */
    struct Stats {
      uint64_t frames_sent = 0;
      uint64_t bytes_sent = 0;
      uint64_t key_frames_sent = 0;
      double avg_frame_size = 0.0;
    };

    Stats
    get_stats() const;

    /**
     * @brief Get the video SSRC.
     * Peers must use this SSRC when creating their video track.
     */
    uint32_t
    video_ssrc() const {
      return ssrc_;
    }

  private:
    VideoSender() = default;
    ~VideoSender() = default;

    // Non-copyable
    VideoSender(const VideoSender &) = delete;
    VideoSender &operator=(const VideoSender &) = delete;

    /**
     * @brief Main video sender loop.
     * Receives video packets and broadcasts to peers.
     */
    void
    sender_loop();

    /**
     * @brief Process a video packet from the encoder.
     * @param packet The encoded video packet.
     */
    void
    process_packet(video::packet_t &packet);

    /**
     * @brief Packetize H.264 NAL units into RTP.
     * @param data NAL unit data.
     * @param size Data size.
     * @param timestamp RTP timestamp.
     * @param is_keyframe True if this is a keyframe.
     */
    void
    packetize_h264(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe);

    /**
     * @brief Packetize HEVC NAL units into RTP.
     */
    void
    packetize_hevc(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe);

    /**
     * @brief Packetize AV1 OBUs into RTP.
     */
    void
    packetize_av1(const uint8_t *data, size_t size, uint32_t timestamp, bool is_keyframe);

    /**
     * @brief Send an RTP packet to all peers.
     * @param rtp_data The complete RTP packet.
     * @param size Packet size.
     * @param timestamp RTP timestamp.
     */
    void
    broadcast_rtp_packet(const std::byte *rtp_data, size_t size, uint32_t timestamp);

    std::atomic<bool> running_{false};
    std::thread sender_thread_;

    VideoCodec codec_{VideoCodec::H264};
    VideoParams params_;

    // RTP state
    uint16_t sequence_number_{0};
    uint32_t ssrc_{0};

    // Stats
    mutable std::mutex stats_mutex_;
    Stats stats_;

    // Maximum RTP payload size (MTU - headers)
    static constexpr size_t MAX_RTP_PAYLOAD = 1200;
  };

}  // namespace webrtc
