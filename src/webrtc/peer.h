/**
 * @file src/webrtc/peer.h
 * @brief WebRTC peer connection management.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

namespace webrtc {

  // Forward declarations
  class Room;

  /**
   * @brief Connection state for a peer.
   */
  enum class PeerState {
    CONNECTING,
    CONNECTED,
    DISCONNECTED,
    FAILED
  };

  /**
   * @brief Represents a WebRTC peer connection to a browser client.
   */
  class Peer : public std::enable_shared_from_this<Peer> {
  public:
    using MessageCallback = std::function<void(const std::string &)>;
    using BinaryCallback = std::function<void(const std::vector<std::byte> &)>;
    using StateCallback = std::function<void(PeerState)>;

    /**
     * @brief Create a new peer connection.
     * @param id Unique identifier for this peer.
     * @param config RTC configuration (STUN/TURN servers).
     * @return Shared pointer to the peer.
     */
    static std::shared_ptr<Peer>
    create(const std::string &id, const rtc::Configuration &config);

    Peer(const std::string &id, const rtc::Configuration &config);
    ~Peer();

    // Non-copyable, non-movable
    Peer(const Peer &) = delete;
    Peer &operator=(const Peer &) = delete;

    /**
     * @brief Get the peer ID.
     */
    const std::string &
    id() const {
      return id_;
    }

    /**
     * @brief Get the current connection state.
     */
    PeerState
    state() const {
      return state_.load();
    }

    /**
     * @brief Set the local description and generate an offer/answer.
     * @param type "offer" or "answer".
     * @return The generated SDP, or empty string on failure.
     */
    std::string
    create_description(const std::string &type);

    /**
     * @brief Set the remote description from the browser.
     * @param sdp The SDP string.
     * @param type "offer" or "answer".
     * @return true on success.
     */
    bool
    set_remote_description(const std::string &sdp, const std::string &type);

    /**
     * @brief Add an ICE candidate from the browser.
     * @param candidate The ICE candidate string.
     * @param mid The media ID.
     * @return true on success.
     */
    bool
    add_ice_candidate(const std::string &candidate, const std::string &mid);

    /**
     * @brief Add a video track for sending encoded video.
     * @param codec The codec (e.g., "H264", "HEVC", "AV1").
     * @return true on success.
     */
    bool
    add_video_track(const std::string &codec);

    /**
     * @brief Add an audio track for sending encoded audio.
     * @return true on success.
     */
    bool
    add_audio_track();

    /**
     * @brief Send video data to this peer.
     * @param data The RTP packet data.
     * @param size Size of the data.
     * @param timestamp RTP timestamp.
     * @return true on success.
     */
    bool
    send_video(const std::byte *data, size_t size, uint32_t timestamp);

    /**
     * @brief Send audio data to this peer.
     * @param data The RTP packet data.
     * @param size Size of the data.
     * @param timestamp RTP timestamp.
     * @return true on success.
     */
    bool
    send_audio(const std::byte *data, size_t size, uint32_t timestamp);

    /**
     * @brief Create a data channel for input.
     * @param label The channel label (e.g., "input-gamepad").
     * @return true on success.
     */
    bool
    create_data_channel(const std::string &label);

    /**
     * @brief Send a message on a data channel.
     * @param label The channel label.
     * @param message The message to send.
     * @return true on success.
     */
    bool
    send_data(const std::string &label, const std::string &message);

    /**
     * @brief Send binary data on a data channel.
     * @param label The channel label.
     * @param data The data to send.
     * @return true on success.
     */
    bool
    send_binary(const std::string &label, const std::vector<std::byte> &data);

    /**
     * @brief Close the peer connection.
     */
    void
    close();

    /**
     * @brief Set callback for when a local ICE candidate is generated.
     */
    void
    on_local_candidate(std::function<void(const std::string &candidate, const std::string &mid)> callback);

    /**
     * @brief Set callback for when local description is ready.
     */
    void
    on_local_description(std::function<void(const std::string &sdp, const std::string &type)> callback);

    /**
     * @brief Set callback for connection state changes.
     */
    void
    on_state_change(StateCallback callback);

    /**
     * @brief Set callback for data channel messages.
     */
    void
    on_data_channel_message(const std::string &label, MessageCallback callback);

    /**
     * @brief Set callback for binary data channel messages.
     */
    void
    on_data_channel_binary(const std::string &label, BinaryCallback callback);

    /**
     * @brief Get statistics about this connection.
     */
    struct Stats {
      uint64_t bytes_sent_video = 0;
      uint64_t bytes_sent_audio = 0;
      uint64_t packets_sent_video = 0;
      uint64_t packets_sent_audio = 0;
      uint64_t bytes_received = 0;
      double rtt_ms = 0.0;
    };

    Stats
    get_stats() const;

  private:
    std::string id_;
    std::atomic<PeerState> state_{PeerState::CONNECTING};
    rtc::Configuration config_;

    std::unique_ptr<rtc::PeerConnection> pc_;

    // Media tracks
    std::shared_ptr<rtc::Track> video_track_;
    std::shared_ptr<rtc::Track> audio_track_;

    // Data channels (label -> channel)
    std::mutex channels_mutex_;
    std::unordered_map<std::string, std::shared_ptr<rtc::DataChannel>> data_channels_;

    // Callbacks
    std::function<void(const std::string &, const std::string &)> on_local_candidate_;
    std::function<void(const std::string &, const std::string &)> on_local_description_;
    StateCallback on_state_change_;
    std::unordered_map<std::string, MessageCallback> message_callbacks_;
    std::unordered_map<std::string, BinaryCallback> binary_callbacks_;

    // Stats
    mutable std::mutex stats_mutex_;
    Stats stats_;

    // SSRC for media streams
    uint32_t ssrc_ = 0;

    void
    setup_peer_connection();

    void
    handle_data_channel(std::shared_ptr<rtc::DataChannel> channel);
  };

  /**
   * @brief Manager for all peer connections.
   */
  class PeerManager {
  public:
    static PeerManager &
    instance();

    /**
     * @brief Create a new peer connection.
     * @param id Unique peer ID.
     * @return The peer, or nullptr on failure.
     */
    std::shared_ptr<Peer>
    create_peer(const std::string &id);

    /**
     * @brief Find a peer by ID.
     */
    std::shared_ptr<Peer>
    find_peer(const std::string &id);

    /**
     * @brief Remove a peer.
     */
    void
    remove_peer(const std::string &id);

    /**
     * @brief Get all active peers.
     */
    std::vector<std::shared_ptr<Peer>>
    get_peers();

    /**
     * @brief Send video to all connected peers.
     * @param data RTP packet data.
     * @param size Data size.
     * @param timestamp RTP timestamp.
     */
    void
    broadcast_video(const std::byte *data, size_t size, uint32_t timestamp);

    /**
     * @brief Send audio to all connected peers.
     * @param data RTP packet data.
     * @param size Data size.
     * @param timestamp RTP timestamp.
     */
    void
    broadcast_audio(const std::byte *data, size_t size, uint32_t timestamp);

    /**
     * @brief Get the RTC configuration (STUN/TURN servers).
     */
    rtc::Configuration
    get_rtc_config() const;

    /**
     * @brief Set the RTC configuration.
     */
    void
    set_rtc_config(const rtc::Configuration &config);

    /**
     * @brief Get the count of connected peers.
     */
    size_t
    connected_count() const;

  private:
    PeerManager();
    ~PeerManager() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Peer>> peers_;
    rtc::Configuration rtc_config_;
  };

}  // namespace webrtc
