/**
 * @file src/webrtc/audio_sender.cpp
 * @brief Audio sender implementation for WebRTC streaming.
 */

#include "audio_sender.h"

#include <cstring>
#include <random>

#include "peer.h"
#include "src/audio.h"
#include "src/globals.h"
#include "src/logging.h"

namespace webrtc {

  AudioSender &
  AudioSender::instance() {
    static AudioSender instance;
    return instance;
  }

  void
  AudioSender::init() {
    // Generate random SSRC (different from video)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    ssrc_ = dist(gen);

    // Default Opus parameters
    params_.sample_rate = 48000;
    params_.channels = 2;
    params_.bitrate = 128000;

    BOOST_LOG(info) << "WebRTC audio sender initialized (SSRC: " << ssrc_ << ")";
  }

  void
  AudioSender::start() {
    if (running_.load()) {
      return;
    }

    running_.store(true);
    sender_thread_ = std::thread(&AudioSender::sender_loop, this);

    BOOST_LOG(info) << "WebRTC audio sender started";
  }

  void
  AudioSender::stop() {
    if (!running_.load()) {
      return;
    }

    running_.store(false);

    if (sender_thread_.joinable()) {
      sender_thread_.join();
    }

    BOOST_LOG(info) << "WebRTC audio sender stopped";
  }

  AudioSender::AudioParams
  AudioSender::get_params() const {
    return params_;
  }

  void
  AudioSender::set_params(const AudioParams &params) {
    params_ = params;
  }

  AudioSender::Stats
  AudioSender::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  void
  AudioSender::sender_loop() {
    BOOST_LOG(info) << "WebRTC audio sender loop started";

    // Subscribe to the audio packet queue
    auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    // Audio timestamp tracking (48kHz for Opus)
    uint32_t timestamp = 0;
    const uint32_t samples_per_packet = 480;  // 10ms at 48kHz

    while (running_.load()) {
      // Wait for next audio packet with timeout so we can check running_ periodically
      auto packet = packets->pop(std::chrono::milliseconds(100));

      if (!packet) {
        // Queue was stopped, empty, or timeout - check if we should continue
        if (!running_.load()) {
          break;
        }
        continue;
      }

      // Only process if we have connected peers
      if (PeerManager::instance().connected_count() == 0) {
        continue;
      }

      // Extract the Opus data from the packet
      // packet_t is std::pair<void*, buffer_t>
      const auto &buffer = packet->second;
      if (buffer.size() > 0) {
        process_packet(buffer.begin(), buffer.size(), timestamp);
        timestamp += samples_per_packet;
      }
    }

    BOOST_LOG(info) << "WebRTC audio sender loop ended";
  }

  void
  AudioSender::process_packet(const uint8_t *data, size_t size, uint32_t timestamp) {
    if (!data || size == 0 || !running_.load()) {
      return;
    }

    send_rtp_packet(data, size, timestamp);

    // Update stats
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.packets_sent++;
      stats_.bytes_sent += size;
    }
  }

  void
  AudioSender::send_rtp_packet(const uint8_t *opus_data, size_t size, uint32_t timestamp) {
    // Build RTP packet for Opus audio
    // Opus uses a simple RTP payload format (RFC 7587)

    std::vector<std::byte> rtp_packet(12 + size);

    // RTP header
    rtp_packet[0] = std::byte{0x80};          // Version 2
    rtp_packet[1] = std::byte{111 | 0x80};    // Payload type 111 (Opus) with marker bit

    // Sequence number (big-endian)
    rtp_packet[2] = std::byte((sequence_number_ >> 8) & 0xFF);
    rtp_packet[3] = std::byte(sequence_number_ & 0xFF);
    sequence_number_++;

    // Timestamp (big-endian) - 48kHz clock for Opus
    rtp_packet[4] = std::byte((timestamp >> 24) & 0xFF);
    rtp_packet[5] = std::byte((timestamp >> 16) & 0xFF);
    rtp_packet[6] = std::byte((timestamp >> 8) & 0xFF);
    rtp_packet[7] = std::byte(timestamp & 0xFF);

    // SSRC (big-endian)
    rtp_packet[8] = std::byte((ssrc_ >> 24) & 0xFF);
    rtp_packet[9] = std::byte((ssrc_ >> 16) & 0xFF);
    rtp_packet[10] = std::byte((ssrc_ >> 8) & 0xFF);
    rtp_packet[11] = std::byte(ssrc_ & 0xFF);

    // Opus payload (no additional header needed for basic Opus RTP)
    std::memcpy(&rtp_packet[12], opus_data, size);

    // Broadcast to all peers
    PeerManager::instance().broadcast_audio(rtp_packet.data(), rtp_packet.size(), timestamp);
  }

}  // namespace webrtc
