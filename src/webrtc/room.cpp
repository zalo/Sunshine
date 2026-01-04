/**
 * @file src/webrtc/room.cpp
 * @brief Room/session management implementation.
 */

#include "room.h"

#include <algorithm>
#include <random>

#include "peer.h"
#include "src/logging.h"

namespace webrtc {

  // Valid characters for room codes (excluding ambiguous: 0/O, 1/I/l)
  static const char ROOM_CODE_CHARS[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  static constexpr size_t ROOM_CODE_LENGTH = 6;

  std::string
  Room::generate_code() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<size_t> dist(0, sizeof(ROOM_CODE_CHARS) - 2);

    std::string code;
    code.reserve(ROOM_CODE_LENGTH);

    for (size_t i = 0; i < ROOM_CODE_LENGTH; ++i) {
      code += ROOM_CODE_CHARS[dist(gen)];
    }

    return code;
  }

  std::shared_ptr<Room>
  Room::create(std::shared_ptr<Peer> host_peer, const std::string &host_name) {
    std::string code = generate_code();

    // Ensure uniqueness (retry if collision)
    int attempts = 0;
    while (RoomManager::instance().find_room(code) && attempts < 10) {
      code = generate_code();
      ++attempts;
    }

    if (attempts >= 10) {
      BOOST_LOG(error) << "Failed to generate unique room code after 10 attempts";
      return nullptr;
    }

    return std::make_shared<Room>(code, host_peer, host_name);
  }

  Room::Room(const std::string &code, std::shared_ptr<Peer> host_peer, const std::string &host_name)
      : code_(code),
        host_peer_id_(host_peer->id()),
        created_at_(std::chrono::steady_clock::now()) {
    // Add host as Player 1
    PlayerInfo player_info;
    player_info.peer_id = host_peer->id();
    player_info.name = host_name;
    player_info.slot = PlayerSlot::PLAYER_1;
    player_info.is_host = true;
    player_info.is_spectator = false;
    player_info.can_use_keyboard = true;
    player_info.can_use_mouse = true;
    player_info.connected_at = created_at_;

    players_[host_peer->id()] = player_info;
    peers_[host_peer->id()] = host_peer;

    BOOST_LOG(info) << "Room " << code_ << " created by " << host_name;
  }

  Room::~Room() {
    BOOST_LOG(info) << "Room " << code_ << " destroyed";
  }

  bool
  Room::is_host(const std::string &peer_id) const {
    return peer_id == host_peer_id_;
  }

  std::string
  Room::host_peer_id() const {
    return host_peer_id_;
  }

  bool
  Room::add_spectator(std::shared_ptr<Peer> peer, const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if peer already in room
    if (players_.find(peer->id()) != players_.end()) {
      BOOST_LOG(warning) << "Peer " << peer->id() << " already in room " << code_;
      return false;
    }

    // Check capacity (spectators don't count toward player limit, but we limit total connections)
    if (peers_.size() >= 16) {
      BOOST_LOG(warning) << "Room " << code_ << " has too many connections";
      return false;
    }

    PlayerInfo player_info;
    player_info.peer_id = peer->id();
    player_info.name = name;
    player_info.slot = PlayerSlot::NONE;
    player_info.is_host = false;
    player_info.is_spectator = true;
    player_info.can_use_keyboard = false;
    player_info.can_use_mouse = false;
    player_info.connected_at = std::chrono::steady_clock::now();

    players_[peer->id()] = player_info;
    peers_[peer->id()] = peer;

    BOOST_LOG(info) << "Spectator " << name << " joined room " << code_;
    return true;
  }

  PlayerSlot
  Room::promote_to_player(const std::string &peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(peer_id);
    if (it == players_.end()) {
      return PlayerSlot::NONE;
    }

    if (!it->second.is_spectator) {
      // Already a player
      return it->second.slot;
    }

    PlayerSlot slot = next_available_slot();
    if (slot == PlayerSlot::NONE) {
      BOOST_LOG(warning) << "No available player slots in room " << code_;
      return PlayerSlot::NONE;
    }

    it->second.slot = slot;
    it->second.is_spectator = false;

    BOOST_LOG(info) << "Player " << it->second.name << " promoted to slot "
                    << static_cast<int>(slot) << " in room " << code_;

    return slot;
  }

  bool
  Room::remove_peer(const std::string &peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(peer_id);
    if (it == players_.end()) {
      return false;
    }

    std::string name = it->second.name;
    bool was_host = it->second.is_host;

    // Release all gamepads owned by this peer
    auto mapping_it = peer_gamepad_mappings_.find(peer_id);
    if (mapping_it != peer_gamepad_mappings_.end()) {
      for (const auto &[browser_id, server_slot] : mapping_it->second) {
        gamepad_slot_owners_.erase(server_slot);
      }
      peer_gamepad_mappings_.erase(mapping_it);
    }

    players_.erase(it);
    peers_.erase(peer_id);

    BOOST_LOG(info) << "Player " << name << " left room " << code_;

    return was_host;  // Return true if host left (room should be closed)
  }

  int
  Room::claim_gamepad(const std::string &peer_id, int browser_gamepad_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto player_it = players_.find(peer_id);
    if (player_it == players_.end() || player_it->second.is_spectator) {
      BOOST_LOG(warning) << "Spectator " << peer_id << " cannot claim gamepad";
      return -1;
    }

    // Check if this browser gamepad is already claimed by this peer
    auto &peer_mapping = peer_gamepad_mappings_[peer_id];
    auto existing = peer_mapping.find(browser_gamepad_id);
    if (existing != peer_mapping.end()) {
      return existing->second;  // Already claimed, return existing slot
    }

    // Find next available server slot
    int server_slot = next_gamepad_slot_.fetch_add(1);
    if (server_slot >= 16) {
      BOOST_LOG(warning) << "No more gamepad slots available";
      next_gamepad_slot_.fetch_sub(1);
      return -1;
    }

    // Claim the slot
    gamepad_slot_owners_[server_slot] = peer_id;
    peer_mapping[browser_gamepad_id] = server_slot;
    player_it->second.gamepad_ids.push_back(server_slot);

    BOOST_LOG(info) << "Player " << player_it->second.name << " claimed gamepad "
                    << browser_gamepad_id << " -> server slot " << server_slot;

    return server_slot;
  }

  void
  Room::release_gamepad(const std::string &peer_id, int server_gamepad_slot) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Verify ownership
    auto owner_it = gamepad_slot_owners_.find(server_gamepad_slot);
    if (owner_it == gamepad_slot_owners_.end() || owner_it->second != peer_id) {
      BOOST_LOG(warning) << "Peer " << peer_id << " does not own gamepad slot " << server_gamepad_slot;
      return;
    }

    // Remove from mappings
    gamepad_slot_owners_.erase(owner_it);

    auto mapping_it = peer_gamepad_mappings_.find(peer_id);
    if (mapping_it != peer_gamepad_mappings_.end()) {
      auto &mapping = mapping_it->second;
      for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        if (it->second == server_gamepad_slot) {
          mapping.erase(it);
          break;
        }
      }
    }

    // Remove from player's gamepad list
    auto player_it = players_.find(peer_id);
    if (player_it != players_.end()) {
      auto &ids = player_it->second.gamepad_ids;
      ids.erase(std::remove(ids.begin(), ids.end(), server_gamepad_slot), ids.end());
    }

    BOOST_LOG(info) << "Peer " << peer_id << " released gamepad slot " << server_gamepad_slot;
  }

  int
  Room::get_gamepad_slot(const std::string &peer_id, int browser_gamepad_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto mapping_it = peer_gamepad_mappings_.find(peer_id);
    if (mapping_it == peer_gamepad_mappings_.end()) {
      return -1;
    }

    auto slot_it = mapping_it->second.find(browser_gamepad_id);
    if (slot_it == mapping_it->second.end()) {
      return -1;
    }

    return slot_it->second;
  }

  bool
  Room::set_keyboard_access(const std::string &peer_id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(peer_id);
    if (it == players_.end()) {
      return false;
    }

    // Host always has access
    if (it->second.is_host) {
      return true;
    }

    it->second.can_use_keyboard = enabled;
    BOOST_LOG(info) << "Keyboard access for " << it->second.name << " set to " << enabled;
    return true;
  }

  bool
  Room::set_mouse_access(const std::string &peer_id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(peer_id);
    if (it == players_.end()) {
      return false;
    }

    // Host always has access
    if (it->second.is_host) {
      return true;
    }

    it->second.can_use_mouse = enabled;
    BOOST_LOG(info) << "Mouse access for " << it->second.name << " set to " << enabled;
    return true;
  }

  bool
  Room::can_use_keyboard(const std::string &peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(peer_id);
    if (it == players_.end()) {
      return false;
    }

    return it->second.can_use_keyboard;
  }

  bool
  Room::can_use_mouse(const std::string &peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(peer_id);
    if (it == players_.end()) {
      return false;
    }

    return it->second.can_use_mouse;
  }

  std::vector<PlayerInfo>
  Room::get_players() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PlayerInfo> result;
    result.reserve(players_.size());

    for (const auto &[id, info] : players_) {
      result.push_back(info);
    }

    return result;
  }

  std::optional<PlayerInfo>
  Room::get_player(const std::string &peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = players_.find(peer_id);
    if (it == players_.end()) {
      return std::nullopt;
    }

    return it->second;
  }

  std::vector<std::shared_ptr<Peer>>
  Room::get_peers() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<Peer>> result;
    result.reserve(peers_.size());

    for (const auto &[id, peer] : peers_) {
      result.push_back(peer);
    }

    return result;
  }

  size_t
  Room::peer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
  }

  size_t
  Room::player_count() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = 0;
    for (const auto &[id, info] : players_) {
      if (!info.is_spectator) {
        ++count;
      }
    }
    return count;
  }

  bool
  Room::is_full() const {
    return player_count() >= 4;
  }

  PlayerSlot
  Room::next_available_slot() const {
    // Must be called with mutex_ held

    std::set<PlayerSlot> used_slots;
    for (const auto &[id, info] : players_) {
      if (!info.is_spectator) {
        used_slots.insert(info.slot);
      }
    }

    for (int i = 1; i <= 4; ++i) {
      PlayerSlot slot = static_cast<PlayerSlot>(i);
      if (used_slots.find(slot) == used_slots.end()) {
        return slot;
      }
    }

    return PlayerSlot::NONE;
  }

  // RoomManager implementation

  RoomManager &
  RoomManager::instance() {
    static RoomManager instance;
    return instance;
  }

  std::shared_ptr<Room>
  RoomManager::create_room(std::shared_ptr<Peer> host_peer, const std::string &host_name) {
    auto room = Room::create(host_peer, host_name);
    if (!room) {
      return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    rooms_[room->code()] = room;
    peer_to_room_[host_peer->id()] = room->code();

    return room;
  }

  std::shared_ptr<Room>
  RoomManager::find_room(const std::string &code) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(code);
    if (it == rooms_.end()) {
      return nullptr;
    }

    return it->second;
  }

  std::shared_ptr<Room>
  RoomManager::find_room_by_peer(const std::string &peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peer_to_room_.find(peer_id);
    if (it == peer_to_room_.end()) {
      return nullptr;
    }

    auto room_it = rooms_.find(it->second);
    if (room_it == rooms_.end()) {
      return nullptr;
    }

    return room_it->second;
  }

  void
  RoomManager::remove_room(const std::string &code) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(code);
    if (it == rooms_.end()) {
      return;
    }

    // Remove all peer mappings for this room
    for (auto peer_it = peer_to_room_.begin(); peer_it != peer_to_room_.end();) {
      if (peer_it->second == code) {
        peer_it = peer_to_room_.erase(peer_it);
      }
      else {
        ++peer_it;
      }
    }

    rooms_.erase(it);
    BOOST_LOG(info) << "Room " << code << " removed from manager";
  }

  std::vector<std::shared_ptr<Room>>
  RoomManager::get_rooms() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<Room>> result;
    result.reserve(rooms_.size());

    for (const auto &[code, room] : rooms_) {
      result.push_back(room);
    }

    return result;
  }

  size_t
  RoomManager::room_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rooms_.size();
  }

}  // namespace webrtc
