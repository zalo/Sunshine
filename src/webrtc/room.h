/**
 * @file src/webrtc/room.h
 * @brief Room/session management for WebRTC multiplayer streaming.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace webrtc {

  // Forward declarations
  class Peer;

  /**
   * @brief Player slot assignment (1-4).
   */
  enum class PlayerSlot : int {
    NONE = 0,
    PLAYER_1 = 1,
    PLAYER_2 = 2,
    PLAYER_3 = 3,
    PLAYER_4 = 4
  };

  /**
   * @brief Information about a connected player.
   */
  struct PlayerInfo {
    std::string peer_id;
    std::string name;
    PlayerSlot slot;
    bool is_host;
    bool is_spectator;            // True until they click "Join as Player"
    std::vector<int> gamepad_ids; // Gamepads claimed by this player
    bool can_use_keyboard;        // Host can toggle this for guests
    bool can_use_mouse;           // Host can toggle this for guests
    std::chrono::steady_clock::time_point connected_at;
  };

  /**
   * @brief Represents a streaming room/session.
   */
  class Room {
  public:
    /**
     * @brief Create a new room with a generated code.
     * @param host_peer The peer creating the room (becomes Player 1).
     * @param host_name Display name of the host.
     * @return Shared pointer to the new room, or nullptr on failure.
     */
    static std::shared_ptr<Room>
    create(std::shared_ptr<Peer> host_peer, const std::string &host_name);

    /**
     * @brief Generate a random 6-character room code.
     * Uses alphanumeric characters excluding ambiguous ones (0/O, 1/I/l).
     * @return The generated room code.
     */
    static std::string
    generate_code();

    Room(const std::string &code, std::shared_ptr<Peer> host_peer, const std::string &host_name);
    ~Room();

    // Non-copyable, non-movable
    Room(const Room &) = delete;
    Room &operator=(const Room &) = delete;

    /**
     * @brief Get the room code.
     */
    const std::string &
    code() const {
      return code_;
    }

    /**
     * @brief Check if a peer is the host.
     */
    bool
    is_host(const std::string &peer_id) const;

    /**
     * @brief Get the host peer ID.
     */
    std::string
    host_peer_id() const;

    /**
     * @brief Check if the room has an active host.
     */
    bool
    has_active_host() const;

    /**
     * @brief Promote a peer to host (used when previous host left).
     * @param peer_id The peer to promote.
     * @return true if promotion successful.
     */
    bool
    promote_to_host(const std::string &peer_id);

    /**
     * @brief Add a peer to the room as a spectator.
     * @param peer The peer to add.
     * @param name Display name.
     * @return true if added, false if room is full or peer already in room.
     */
    bool
    add_spectator(std::shared_ptr<Peer> peer, const std::string &name);

    /**
     * @brief Promote a spectator to a player.
     * @param peer_id The peer to promote.
     * @return The assigned player slot, or NONE if no slots available.
     */
    PlayerSlot
    promote_to_player(const std::string &peer_id);

    /**
     * @brief Remove a peer from the room.
     * @param peer_id The peer to remove.
     * @return true if the room should be closed (host left).
     */
    bool
    remove_peer(const std::string &peer_id);

    /**
     * @brief Claim a gamepad for a player.
     * @param peer_id The peer claiming the gamepad.
     * @param browser_gamepad_id The gamepad index in the browser (0-3).
     * @return The server-side gamepad slot assigned, or -1 on failure.
     */
    int
    claim_gamepad(const std::string &peer_id, int browser_gamepad_id);

    /**
     * @brief Release a gamepad.
     * @param peer_id The peer releasing the gamepad.
     * @param server_gamepad_slot The server-side slot to release.
     */
    void
    release_gamepad(const std::string &peer_id, int server_gamepad_slot);

    /**
     * @brief Get the server gamepad slot for a player's browser gamepad.
     * @param peer_id The peer.
     * @param browser_gamepad_id The browser gamepad index.
     * @return Server-side gamepad slot, or -1 if not claimed.
     */
    int
    get_gamepad_slot(const std::string &peer_id, int browser_gamepad_id) const;

    /**
     * @brief Toggle keyboard access for a guest.
     * @param peer_id The guest peer ID.
     * @param enabled Whether to enable keyboard access.
     * @return true if successful.
     */
    bool
    set_keyboard_access(const std::string &peer_id, bool enabled);

    /**
     * @brief Toggle mouse access for a guest.
     * @param peer_id The guest peer ID.
     * @param enabled Whether to enable mouse access.
     * @return true if successful.
     */
    bool
    set_mouse_access(const std::string &peer_id, bool enabled);

    /**
     * @brief Check if a peer can use keyboard input.
     */
    bool
    can_use_keyboard(const std::string &peer_id) const;

    /**
     * @brief Check if a peer can use mouse input.
     */
    bool
    can_use_mouse(const std::string &peer_id) const;

    /**
     * @brief Set default keyboard access for new guests.
     */
    void
    set_default_keyboard_access(bool enabled);

    /**
     * @brief Set default mouse access for new guests.
     */
    void
    set_default_mouse_access(bool enabled);

    /**
     * @brief Get default keyboard access setting.
     */
    bool
    get_default_keyboard_access() const;

    /**
     * @brief Get default mouse access setting.
     */
    bool
    get_default_mouse_access() const;

    /**
     * @brief Get information about all players in the room.
     */
    std::vector<PlayerInfo>
    get_players() const;

    /**
     * @brief Get information about a specific player.
     * @param peer_id The peer ID.
     * @return Player info if found.
     */
    std::optional<PlayerInfo>
    get_player(const std::string &peer_id) const;

    /**
     * @brief Get all peer shared_ptrs in the room.
     */
    std::vector<std::shared_ptr<Peer>>
    get_peers() const;

    /**
     * @brief Update the peer connection for a player (used for reconnect).
     * @param peer_id The peer ID.
     * @param new_peer The new peer connection.
     */
    void
    update_peer(const std::string &peer_id, std::shared_ptr<Peer> new_peer);

    /**
     * @brief Get the peer count (including spectators).
     */
    size_t
    peer_count() const;

    /**
     * @brief Get the player count (excluding spectators).
     */
    size_t
    player_count() const;

    /**
     * @brief Check if the room is full (4 players).
     */
    bool
    is_full() const;

    /**
     * @brief Get room creation time.
     */
    std::chrono::steady_clock::time_point
    created_at() const {
      return created_at_;
    }

  private:
    std::string code_;
    std::string host_peer_id_;
    std::chrono::steady_clock::time_point created_at_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PlayerInfo> players_;
    std::unordered_map<std::string, std::shared_ptr<Peer>> peers_;

    // Gamepad slot management
    // Maps server gamepad slot (0-15) to peer_id that owns it
    std::unordered_map<int, std::string> gamepad_slot_owners_;
    // Maps peer_id -> (browser_gamepad_id -> server_slot)
    std::unordered_map<std::string, std::unordered_map<int, int>> peer_gamepad_mappings_;
    std::atomic<int> next_gamepad_slot_{0};

    // Default permissions for new guests (set by host via toggles)
    bool default_keyboard_access_{true};
    bool default_mouse_access_{true};

    PlayerSlot
    next_available_slot() const;
  };

  /**
   * @brief Manager for all active rooms.
   */
  class RoomManager {
  public:
    static RoomManager &
    instance();

    /**
     * @brief Create a new room.
     * @param host_peer The host peer.
     * @param host_name The host's display name.
     * @return The room, or nullptr on failure.
     */
    std::shared_ptr<Room>
    create_room(std::shared_ptr<Peer> host_peer, const std::string &host_name);

    /**
     * @brief Add an existing room to the manager.
     * Used for single-session mode where room is created externally.
     * @param room The room to add.
     */
    void
    add_room(std::shared_ptr<Room> room);

    /**
     * @brief Register a peer with a room.
     * Used when adding peers to existing rooms.
     * @param peer_id The peer ID.
     * @param room_code The room code.
     */
    void
    register_peer(const std::string &peer_id, const std::string &room_code);

    /**
     * @brief Find a room by code.
     * @param code The 6-character room code.
     * @return The room, or nullptr if not found.
     */
    std::shared_ptr<Room>
    find_room(const std::string &code);

    /**
     * @brief Find the room a peer is in.
     * @param peer_id The peer ID.
     * @return The room, or nullptr if not in any room.
     */
    std::shared_ptr<Room>
    find_room_by_peer(const std::string &peer_id);

    /**
     * @brief Remove a room.
     * @param code The room code.
     */
    void
    remove_room(const std::string &code);

    /**
     * @brief Get all active rooms.
     */
    std::vector<std::shared_ptr<Room>>
    get_rooms();

    /**
     * @brief Get the number of active rooms.
     */
    size_t
    room_count() const;

  private:
    RoomManager() = default;
    ~RoomManager() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Room>> rooms_;
    std::unordered_map<std::string, std::string> peer_to_room_;  // peer_id -> room_code
  };

}  // namespace webrtc
