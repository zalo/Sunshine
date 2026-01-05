/**
 * @file src/webrtc/signaling.h
 * @brief WebSocket signaling for WebRTC connections.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ws_server.h"

namespace webrtc {

  /**
   * @brief Handles WebSocket signaling for WebRTC connections.
   *
   * Signaling protocol (JSON over WebSocket):
   *
   * Client -> Server:
   *   { "type": "create_room", "player_name": "Alice" }
   *   { "type": "join_room", "room_code": "A3K9B2", "player_name": "Bob" }
   *   { "type": "leave_room" }
   *   { "type": "join_as_player" }  // Spectator -> Player
   *   { "type": "claim_gamepad", "gamepad_id": 0 }
   *   { "type": "release_gamepad", "gamepad_slot": 0 }
   *   { "type": "sdp", "sdp": "...", "sdp_type": "offer"|"answer" }
   *   { "type": "ice", "candidate": "...", "mid": "..." }
   *   { "type": "set_guest_keyboard", "peer_id": "...", "enabled": true }
   *   { "type": "set_guest_mouse", "peer_id": "...", "enabled": true }
   *   { "type": "set_quality", "bitrate": 10000, "framerate": 60, "width": 1920, "height": 1080 }
   *
   * Server -> Client:
   *   { "type": "room_created", "room_code": "A3K9B2", "player_slot": 1 }
   *   { "type": "room_joined", "room_code": "A3K9B2", "player_slot": 0, "is_spectator": true }
   *   { "type": "promoted_to_player", "player_slot": 2 }
   *   { "type": "room_updated", "players": [...] }
   *   { "type": "player_joined", "player": {...} }
   *   { "type": "player_left", "peer_id": "...", "slot": 2 }
   *   { "type": "room_closed", "reason": "host_left" }
   *   { "type": "gamepad_claimed", "gamepad_id": 0, "server_slot": 1 }
   *   { "type": "gamepad_released", "server_slot": 1 }
   *   { "type": "sdp", "sdp": "...", "sdp_type": "offer"|"answer" }
   *   { "type": "ice", "candidate": "...", "mid": "..." }
   *   { "type": "error", "message": "...", "code": "..." }
   *   { "type": "stream_ready" }  // Video/audio tracks ready
   *   { "type": "quality_updated", "success": true, "bitrate": 10000, ... }
   */
  class SignalingServer {
  public:
    static SignalingServer &
    instance();

    /**
     * @brief Initialize the signaling server.
     * This sets up WebSocket handlers on the existing HTTPS server.
     */
    void
    init();

    /**
     * @brief Start accepting WebSocket connections.
     */
    void
    start();

    /**
     * @brief Stop the signaling server.
     */
    void
    stop();

    /**
     * @brief Send a message to a specific peer.
     * @param peer_id The peer to send to.
     * @param message JSON message string.
     * @return true if sent successfully.
     */
    bool
    send_to_peer(const std::string &peer_id, const std::string &message);

    /**
     * @brief Broadcast a message to all peers in a room.
     * @param room_code The room code.
     * @param message JSON message string.
     * @param exclude_peer_id Optional peer to exclude from broadcast.
     */
    void
    broadcast_to_room(const std::string &room_code,
      const std::string &message,
      const std::string &exclude_peer_id = "");

    /**
     * @brief Get the number of connected WebSocket clients.
     */
    size_t
    connection_count() const;

  private:
    SignalingServer() = default;
    ~SignalingServer() = default;

    // Non-copyable
    SignalingServer(const SignalingServer &) = delete;
    SignalingServer &operator=(const SignalingServer &) = delete;

    /**
     * @brief Handle a new WebSocket connection.
     */
    void
    on_open(const std::string &peer_id);

    /**
     * @brief Handle a WebSocket disconnection.
     */
    void
    on_close(const std::string &peer_id);

    /**
     * @brief Handle an incoming WebSocket message.
     */
    void
    on_message(const std::string &peer_id, const std::string &message);

    // Message handlers
    void
    handle_join(const std::string &peer_id, const std::string &player_name);

    void
    handle_create_room(const std::string &peer_id, const std::string &player_name);

    void
    handle_join_room(const std::string &peer_id, const std::string &room_code, const std::string &player_name);

    void
    handle_leave_room(const std::string &peer_id);

    void
    handle_join_as_player(const std::string &peer_id);

    void
    handle_claim_gamepad(const std::string &peer_id, int gamepad_id);

    void
    handle_release_gamepad(const std::string &peer_id, int server_slot);

    void
    handle_sdp(const std::string &peer_id, const std::string &sdp, const std::string &type);

    void
    handle_ice(const std::string &peer_id, const std::string &candidate, const std::string &mid);

    void
    handle_set_guest_keyboard(const std::string &peer_id, const std::string &target_peer_id, bool enabled);

    void
    handle_set_guest_mouse(const std::string &peer_id, const std::string &target_peer_id, bool enabled);

    void
    handle_set_quality(const std::string &peer_id, int bitrate, int framerate, int width, int height);

    // Helper to send error response
    void
    send_error(const std::string &peer_id, const std::string &message, const std::string &code);

    // Helper to build player list JSON
    std::string
    build_players_json(const std::string &room_code);

    // WebSocket server callbacks
    void
    on_ws_connect(ws_connection_id conn_id);

    void
    on_ws_disconnect(ws_connection_id conn_id);

    void
    on_ws_message(ws_connection_id conn_id, const std::string &message);

    std::atomic<bool> running_{false};
    mutable std::mutex connections_mutex_;

    // WebSocket connection ID -> peer_id mapping
    std::unordered_map<ws_connection_id, std::string> ws_to_peer_;
    std::unordered_map<std::string, ws_connection_id> peer_to_ws_;
  };

}  // namespace webrtc
