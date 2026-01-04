/**
 * @file src/webrtc/audio_sender.h
 * @brief Audio packet handling for WebRTC streaming.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace webrtc {

  /**
   * @brief RTP packetizer for Opus audio.
   *
   * Takes encoded Opus audio frames and packetizes them into RTP packets
   * suitable for WebRTC transmission.
   */
  class AudioSender {
  public:
    static AudioSender &
    instance();

    /**
     * @brief Initialize the audio sender.
     * Subscribes to Sunshine's audio packet queue.
     */
    void
    init();

    /**
     * @brief Start the audio sender thread.
     */
    void
    start();

    /**
     * @brief Stop the audio sender.
     */
    void
    stop();

    /**
     * @brief Check if audio is being sent.
     */
    bool
    is_running() const {
      return running_.load();
    }

    /**
     * @brief Get audio parameters.
     */
    struct AudioParams {
      int sample_rate = 48000;
      int channels = 2;
      int bitrate = 128000;
    };

    AudioParams
    get_params() const;

    /**
     * @brief Set audio parameters.
     */
    void
    set_params(const AudioParams &params);

    /**
     * @brief Get statistics.
     */
    struct Stats {
      uint64_t packets_sent = 0;
      uint64_t bytes_sent = 0;
    };

    Stats
    get_stats() const;

  private:
    AudioSender() = default;
    ~AudioSender() = default;

    // Non-copyable
    AudioSender(const AudioSender &) = delete;
    AudioSender &operator=(const AudioSender &) = delete;

    /**
     * @brief Main audio sender loop.
     */
    void
    sender_loop();

    /**
     * @brief Process an audio packet from the encoder.
     * @param data Opus encoded audio data.
     * @param size Data size.
     * @param timestamp Audio timestamp.
     */
    void
    process_packet(const uint8_t *data, size_t size, uint32_t timestamp);

    /**
     * @brief Build and send an RTP packet.
     */
    void
    send_rtp_packet(const uint8_t *opus_data, size_t size, uint32_t timestamp);

    std::atomic<bool> running_{false};
    std::thread sender_thread_;

    AudioParams params_;

    // RTP state
    uint16_t sequence_number_{0};
    uint32_t ssrc_{0};

    // Stats
    mutable std::mutex stats_mutex_;
    Stats stats_;
  };

}  // namespace webrtc
