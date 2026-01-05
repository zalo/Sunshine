/**
 * @file src/webrtc/peer.cpp
 * @brief WebRTC peer connection implementation.
 */

#include "peer.h"

#include <random>

#include "src/logging.h"
#include "video_sender.h"

namespace webrtc {

  std::shared_ptr<Peer>
  Peer::create(const std::string &id, const rtc::Configuration &config) {
    auto peer = std::make_shared<Peer>(id, config);
    peer->setup_peer_connection();  // Must be called after shared_ptr is created for weak_from_this()
    return peer;
  }

  Peer::Peer(const std::string &id, const rtc::Configuration &config)
      : id_(id),
        config_(config) {
    // Generate random SSRC for RTP
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    ssrc_ = dist(gen);

    // NOTE: setup_peer_connection() is called from create() after shared_ptr is constructed
    // This is required for weak_from_this() to work in callbacks

    BOOST_LOG(info) << "WebRTC peer " << id_ << " created";
  }

  Peer::~Peer() {
    close();
    // Explicitly reset pc_ to ensure all callbacks complete before member destruction
    // This prevents use-after-free from libdatachannel callbacks during destruction
    pc_.reset();
    BOOST_LOG(info) << "WebRTC peer " << id_ << " destroyed";
  }

  void
  Peer::setup_peer_connection() {
    pc_ = std::make_unique<rtc::PeerConnection>(config_);

    // Capture weak_ptr to prevent use-after-free when callbacks fire after peer destruction
    std::weak_ptr<Peer> weak_self = weak_from_this();
    std::string peer_id = id_;  // Copy for safe logging

    pc_->onStateChange([weak_self, peer_id](rtc::PeerConnection::State state) {
      auto self = weak_self.lock();
      if (!self) {
        BOOST_LOG(debug) << "Peer " << peer_id << " state change callback ignored (peer destroyed)";
        return;
      }

      PeerState new_state;
      switch (state) {
        case rtc::PeerConnection::State::New:
        case rtc::PeerConnection::State::Connecting:
          new_state = PeerState::CONNECTING;
          break;
        case rtc::PeerConnection::State::Connected:
          new_state = PeerState::CONNECTED;
          break;
        case rtc::PeerConnection::State::Disconnected:
          new_state = PeerState::DISCONNECTED;
          break;
        case rtc::PeerConnection::State::Failed:
        case rtc::PeerConnection::State::Closed:
          new_state = PeerState::FAILED;
          break;
        default:
          new_state = PeerState::CONNECTING;
      }

      self->state_.store(new_state);

      BOOST_LOG(debug) << "Peer " << peer_id << " state changed to " << static_cast<int>(new_state);

      // Start async sender when connected
      if (new_state == PeerState::CONNECTED) {
        self->start_sender();
      }
      else if (new_state == PeerState::DISCONNECTED || new_state == PeerState::FAILED) {
        self->stop_sender();
      }

      if (self->on_state_change_) {
        self->on_state_change_(new_state);
      }
    });

    pc_->onLocalDescription([weak_self, peer_id](rtc::Description description) {
      auto self = weak_self.lock();
      if (!self) {
        BOOST_LOG(debug) << "Peer " << peer_id << " local description callback ignored (peer destroyed)";
        return;
      }

      BOOST_LOG(debug) << "Peer " << peer_id << " local description generated";

      if (self->on_local_description_) {
        self->on_local_description_(std::string(description), description.typeString());
      }
    });

    pc_->onLocalCandidate([weak_self, peer_id](rtc::Candidate candidate) {
      auto self = weak_self.lock();
      if (!self) {
        BOOST_LOG(debug) << "Peer " << peer_id << " local candidate callback ignored (peer destroyed)";
        return;
      }

      BOOST_LOG(debug) << "Peer " << peer_id << " local ICE candidate: " << std::string(candidate);

      if (self->on_local_candidate_) {
        self->on_local_candidate_(std::string(candidate), candidate.mid());
      }
    });

    pc_->onDataChannel([weak_self, peer_id](std::shared_ptr<rtc::DataChannel> channel) {
      auto self = weak_self.lock();
      if (!self) {
        BOOST_LOG(debug) << "Peer " << peer_id << " data channel callback ignored (peer destroyed)";
        return;
      }

      BOOST_LOG(debug) << "Peer " << peer_id << " received data channel: " << channel->label();
      self->handle_data_channel(channel);
    });

    pc_->onTrack([peer_id](std::shared_ptr<rtc::Track> track) {
      // This callback only logs, no need to access Peer state
      BOOST_LOG(debug) << "Peer " << peer_id << " received track: " << track->mid();
    });
  }

  std::string
  Peer::create_description(const std::string &type) {
    if (!pc_) {
      BOOST_LOG(error) << "Peer " << id_ << " has no peer connection";
      return "";
    }

    try {
      if (type == "offer") {
        pc_->setLocalDescription(rtc::Description::Type::Offer);
      }
      else {
        pc_->setLocalDescription(rtc::Description::Type::Answer);
      }

      auto desc = pc_->localDescription();
      if (desc) {
        return std::string(*desc);
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Peer " << id_ << " failed to create description: " << e.what();
    }

    return "";
  }

  bool
  Peer::set_remote_description(const std::string &sdp, const std::string &type) {
    if (!pc_) {
      BOOST_LOG(error) << "Peer " << id_ << " has no peer connection";
      return false;
    }

    try {
      rtc::Description::Type desc_type =
        (type == "offer") ? rtc::Description::Type::Offer : rtc::Description::Type::Answer;

      pc_->setRemoteDescription(rtc::Description(sdp, desc_type));
      BOOST_LOG(debug) << "Peer " << id_ << " set remote description (" << type << ")";
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Peer " << id_ << " failed to set remote description: " << e.what();
      return false;
    }
  }

  bool
  Peer::add_ice_candidate(const std::string &candidate, const std::string &mid) {
    if (!pc_) {
      BOOST_LOG(error) << "Peer " << id_ << " has no peer connection";
      return false;
    }

    try {
      pc_->addRemoteCandidate(rtc::Candidate(candidate, mid));
      BOOST_LOG(debug) << "Peer " << id_ << " added ICE candidate";
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Peer " << id_ << " failed to add ICE candidate: " << e.what();
      return false;
    }
  }

  bool
  Peer::add_video_track(const std::string &codec) {
    if (!pc_) {
      BOOST_LOG(error) << "Peer " << id_ << " has no peer connection";
      return false;
    }

    try {
      rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);

      // Add appropriate codec
      int payload_type = 96;  // Dynamic payload type for video

      if (codec == "H264") {
        // H.264 Constrained Baseline Profile, Level 4.0
        video.addH264Codec(payload_type);
      }
      else if (codec == "HEVC" || codec == "H265") {
        // H.265/HEVC
        video.addH265Codec(payload_type);
      }
      else if (codec == "AV1") {
        // AV1
        video.addAV1Codec(payload_type);
      }
      else {
        // Default to H.264
        video.addH264Codec(payload_type);
      }

      // Use VideoSender's SSRC so RTP packets match what's negotiated in SDP
      uint32_t video_ssrc = VideoSender::instance().video_ssrc();
      video.addSSRC(video_ssrc, "video-stream");

      auto track = pc_->addTrack(video);

      // Copy id for safe callbacks
      std::string peer_id = id_;

      track->onOpen([peer_id]() {
        BOOST_LOG(info) << "Peer " << peer_id << " video track opened";
      });

      track->onClosed([peer_id]() {
        BOOST_LOG(info) << "Peer " << peer_id << " video track closed";
      });

      // Store track with mutex protection
      {
        std::lock_guard<std::mutex> lock(track_mutex_);
        video_track_ = track;
      }

      BOOST_LOG(info) << "Peer " << id_ << " added video track (" << codec << ", SSRC: " << video_ssrc << ")";
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Peer " << id_ << " failed to add video track: " << e.what();
      return false;
    }
  }

  bool
  Peer::add_audio_track() {
    if (!pc_) {
      BOOST_LOG(error) << "Peer " << id_ << " has no peer connection";
      return false;
    }

    try {
      rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);

      // Opus codec (payload type 111 is common for Opus)
      audio.addOpusCodec(111);

      audio.addSSRC(ssrc_ + 1, "audio-stream");

      auto track = pc_->addTrack(audio);

      // Copy id for safe callbacks
      std::string peer_id = id_;

      track->onOpen([peer_id]() {
        BOOST_LOG(info) << "Peer " << peer_id << " audio track opened";
      });

      track->onClosed([peer_id]() {
        BOOST_LOG(info) << "Peer " << peer_id << " audio track closed";
      });

      // Store track with mutex protection
      {
        std::lock_guard<std::mutex> lock(track_mutex_);
        audio_track_ = track;
      }

      BOOST_LOG(info) << "Peer " << id_ << " added audio track (Opus)";
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Peer " << id_ << " failed to add audio track: " << e.what();
      return false;
    }
  }

  bool
  Peer::send_video(const std::byte *data, size_t size, uint32_t timestamp) {
    // Quick state check
    if (state_.load() != PeerState::CONNECTED || !sender_running_.load()) {
      return false;
    }

    // Enqueue the packet for async sending
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);

      // If queue is full, drop oldest packets to make room (prioritize recent data)
      while (packet_queue_.size() >= MAX_QUEUE_SIZE) {
        packet_queue_.erase(packet_queue_.begin());
      }

      MediaPacket pkt;
      pkt.is_video = true;
      pkt.data.assign(data, data + size);
      pkt.timestamp = timestamp;
      packet_queue_.push_back(std::move(pkt));
    }
    queue_cv_.notify_one();

    return true;
  }

  bool
  Peer::send_audio(const std::byte *data, size_t size, uint32_t timestamp) {
    // Quick state check
    if (state_.load() != PeerState::CONNECTED || !sender_running_.load()) {
      return false;
    }

    // Enqueue the packet for async sending
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);

      // If queue is full, drop oldest packets to make room
      while (packet_queue_.size() >= MAX_QUEUE_SIZE) {
        packet_queue_.erase(packet_queue_.begin());
      }

      MediaPacket pkt;
      pkt.is_video = false;
      pkt.data.assign(data, data + size);
      pkt.timestamp = timestamp;
      packet_queue_.push_back(std::move(pkt));
    }
    queue_cv_.notify_one();

    return true;
  }

  bool
  Peer::send_video_direct(const std::byte *data, size_t size, uint32_t timestamp) {
    // Hold track mutex to prevent race with close()
    std::shared_ptr<rtc::Track> track;
    {
      std::lock_guard<std::mutex> lock(track_mutex_);
      track = video_track_;
    }

    if (!track) {
      return false;
    }

    if (!track->isOpen()) {
      static uint64_t not_open_count = 0;
      if (not_open_count++ % 60 == 0) {
        BOOST_LOG(debug) << "Peer " << id_ << " video track not open (count: " << not_open_count << ")";
      }
      return false;
    }

    try {
      bool sent = track->send(data, size);

      // Update stats
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.bytes_sent_video += size;
      stats_.packets_sent_video++;

      if (stats_.packets_sent_video % 60 == 1) {
        BOOST_LOG(debug) << "Peer " << id_ << " sent video packet " << stats_.packets_sent_video
                         << " (" << size << " bytes, result: " << sent << ")";
      }

      return sent;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Peer " << id_ << " failed to send video: " << e.what();
      return false;
    }
  }

  bool
  Peer::send_audio_direct(const std::byte *data, size_t size, uint32_t timestamp) {
    // Hold track mutex to prevent race with close()
    std::shared_ptr<rtc::Track> track;
    {
      std::lock_guard<std::mutex> lock(track_mutex_);
      track = audio_track_;
    }

    if (!track || !track->isOpen()) {
      return false;
    }

    try {
      track->send(data, size);

      // Update stats
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.bytes_sent_audio += size;
      stats_.packets_sent_audio++;

      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Peer " << id_ << " failed to send audio: " << e.what();
      return false;
    }
  }

  bool
  Peer::create_data_channel(const std::string &label) {
    if (!pc_) {
      BOOST_LOG(error) << "Peer " << id_ << " has no peer connection";
      return false;
    }

    try {
      // Configure data channel options
      rtc::DataChannelInit init;

      if (label == "input") {
        // Input channel uses unreliable/unordered for lowest latency
        // Old packets are automatically discarded since we only care about latest state
        init.reliability.maxRetransmits = 0;  // No retransmissions (unreliable)
        init.reliability.unordered = true;  // Unordered delivery
        BOOST_LOG(debug) << "Peer " << id_ << " creating unreliable/unordered data channel: " << label;
      }

      auto channel = pc_->createDataChannel(label, init);
      handle_data_channel(channel);
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Peer " << id_ << " failed to create data channel: " << e.what();
      return false;
    }
  }

  void
  Peer::handle_data_channel(std::shared_ptr<rtc::DataChannel> channel) {
    std::string label = channel->label();

    // Capture weak_ptr and copy id for safe callbacks
    std::weak_ptr<Peer> weak_self = weak_from_this();
    std::string peer_id = id_;

    channel->onOpen([peer_id, label]() {
      BOOST_LOG(info) << "Peer " << peer_id << " data channel '" << label << "' opened";
    });

    channel->onClosed([peer_id, label]() {
      BOOST_LOG(info) << "Peer " << peer_id << " data channel '" << label << "' closed";
    });

    channel->onMessage([weak_self, peer_id, label](rtc::message_variant data) {
      auto self = weak_self.lock();
      if (!self) {
        BOOST_LOG(debug) << "Peer " << peer_id << " data channel message ignored (peer destroyed)";
        return;
      }

      if (std::holds_alternative<std::string>(data)) {
        // Text message
        const auto &msg = std::get<std::string>(data);
        auto it = self->message_callbacks_.find(label);
        if (it != self->message_callbacks_.end() && it->second) {
          it->second(msg);
        }
      }
      else {
        // Binary message
        const auto &binary = std::get<rtc::binary>(data);
        auto it = self->binary_callbacks_.find(label);
        if (it != self->binary_callbacks_.end() && it->second) {
          it->second(binary);
        }
      }
    });

    {
      std::lock_guard<std::mutex> lock(channels_mutex_);
      data_channels_[label] = channel;
    }
  }

  bool
  Peer::send_data(const std::string &label, const std::string &message) {
    std::lock_guard<std::mutex> lock(channels_mutex_);

    auto it = data_channels_.find(label);
    if (it == data_channels_.end() || !it->second->isOpen()) {
      return false;
    }

    try {
      it->second->send(message);
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Peer " << id_ << " failed to send data on '" << label << "': " << e.what();
      return false;
    }
  }

  bool
  Peer::send_binary(const std::string &label, const std::vector<std::byte> &data) {
    std::lock_guard<std::mutex> lock(channels_mutex_);

    auto it = data_channels_.find(label);
    if (it == data_channels_.end() || !it->second->isOpen()) {
      return false;
    }

    try {
      it->second->send(data);
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "Peer " << id_ << " failed to send binary on '" << label << "': " << e.what();
      return false;
    }
  }

  void
  Peer::close() {
    // Prevent double-close
    PeerState expected = PeerState::CONNECTED;
    if (!state_.compare_exchange_strong(expected, PeerState::DISCONNECTED)) {
      expected = PeerState::CONNECTING;
      if (!state_.compare_exchange_strong(expected, PeerState::DISCONNECTED)) {
        BOOST_LOG(debug) << "Peer " << id_ << " already closed or closing";
        return;
      }
    }

    BOOST_LOG(debug) << "Peer " << id_ << " closing...";

    // Stop the async sender first
    stop_sender();

    // Close the peer connection to stop callbacks
    if (pc_) {
      try {
        pc_->close();
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Peer " << id_ << " error closing peer connection: " << e.what();
      }
    }

    // Then clear the tracks
    {
      std::lock_guard<std::mutex> lock(track_mutex_);
      video_track_.reset();
      audio_track_.reset();
    }

    // Clear data channels
    {
      std::lock_guard<std::mutex> lock(channels_mutex_);
      data_channels_.clear();
    }

    BOOST_LOG(debug) << "Peer " << id_ << " closed";
  }

  void
  Peer::on_local_candidate(std::function<void(const std::string &, const std::string &)> callback) {
    on_local_candidate_ = callback;
  }

  void
  Peer::on_local_description(std::function<void(const std::string &, const std::string &)> callback) {
    on_local_description_ = callback;
  }

  void
  Peer::on_state_change(StateCallback callback) {
    on_state_change_ = callback;
  }

  void
  Peer::on_data_channel_message(const std::string &label, MessageCallback callback) {
    message_callbacks_[label] = callback;
  }

  void
  Peer::on_data_channel_binary(const std::string &label, BinaryCallback callback) {
    binary_callbacks_[label] = callback;
  }

  Peer::Stats
  Peer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  void
  Peer::start_sender() {
    // Prevent double-start
    bool expected = false;
    if (!sender_running_.compare_exchange_strong(expected, true)) {
      return;
    }

    BOOST_LOG(info) << "Peer " << id_ << " starting async sender";

    sender_thread_ = std::thread(&Peer::sender_loop, this);
  }

  void
  Peer::stop_sender() {
    // Signal the sender to stop
    bool expected = true;
    if (!sender_running_.compare_exchange_strong(expected, false)) {
      return;  // Already stopped
    }

    BOOST_LOG(debug) << "Peer " << id_ << " stopping async sender";

    // Wake up the sender thread if it's waiting
    queue_cv_.notify_all();

    // Wait for the sender thread to finish
    if (sender_thread_.joinable()) {
      sender_thread_.join();
    }

    // Clear any remaining packets
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      packet_queue_.clear();
    }

    BOOST_LOG(debug) << "Peer " << id_ << " async sender stopped";
  }

  void
  Peer::sender_loop() {
    BOOST_LOG(debug) << "Peer " << id_ << " sender loop started";

    while (sender_running_.load()) {
      MediaPacket pkt;
      bool got_packet = false;

      // Wait for a packet or stop signal
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // Wait with timeout so we can check sender_running_ periodically
        queue_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
          return !packet_queue_.empty() || !sender_running_.load();
        });

        if (!sender_running_.load()) {
          break;
        }

        if (!packet_queue_.empty()) {
          pkt = std::move(packet_queue_.front());
          packet_queue_.erase(packet_queue_.begin());
          got_packet = true;
        }
      }

      // Send the packet outside of the lock
      if (got_packet) {
        if (pkt.is_video) {
          send_video_direct(pkt.data.data(), pkt.data.size(), pkt.timestamp);
        }
        else {
          send_audio_direct(pkt.data.data(), pkt.data.size(), pkt.timestamp);
        }
      }
    }

    BOOST_LOG(debug) << "Peer " << id_ << " sender loop ended";
  }

  // PeerManager implementation

  PeerManager::PeerManager() {
    // Default configuration with Google STUN
    rtc_config_.iceServers.emplace_back("stun:stun.l.google.com:19302");
  }

  PeerManager &
  PeerManager::instance() {
    static PeerManager instance;
    return instance;
  }

  std::shared_ptr<Peer>
  PeerManager::create_peer(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if peer already exists
    auto it = peers_.find(id);
    if (it != peers_.end()) {
      BOOST_LOG(warning) << "Peer " << id << " already exists";
      return it->second;
    }

    auto peer = Peer::create(id, rtc_config_);
    peers_[id] = peer;

    return peer;
  }

  std::shared_ptr<Peer>
  PeerManager::find_peer(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(id);
    if (it == peers_.end()) {
      return nullptr;
    }

    return it->second;
  }

  void
  PeerManager::remove_peer(const std::string &id) {
    // Extract peer from map while holding lock, then close outside lock
    // This prevents crashes from callbacks firing during destruction
    std::shared_ptr<Peer> peer;
    {
      std::lock_guard<std::mutex> lock(mutex_);

      auto it = peers_.find(id);
      if (it != peers_.end()) {
        peer = std::move(it->second);  // Move ownership out
        peers_.erase(it);
      }
    }

    // Close the peer outside the lock to prevent deadlock and
    // allow the peer to fully clean up before destruction
    if (peer) {
      peer->close();
      BOOST_LOG(info) << "Peer " << id << " removed and closed";
      // peer goes out of scope here, destructor runs without lock held
    }
  }

  std::vector<std::shared_ptr<Peer>>
  PeerManager::get_peers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<Peer>> result;
    result.reserve(peers_.size());

    for (const auto &[id, peer] : peers_) {
      result.push_back(peer);
    }

    return result;
  }

  void
  PeerManager::broadcast_video(const std::byte *data, size_t size, uint32_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto &[id, peer] : peers_) {
      if (peer->state() == PeerState::CONNECTED) {
        peer->send_video(data, size, timestamp);
      }
    }
  }

  void
  PeerManager::broadcast_audio(const std::byte *data, size_t size, uint32_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto &[id, peer] : peers_) {
      if (peer->state() == PeerState::CONNECTED) {
        peer->send_audio(data, size, timestamp);
      }
    }
  }

  rtc::Configuration
  PeerManager::get_rtc_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rtc_config_;
  }

  void
  PeerManager::set_rtc_config(const rtc::Configuration &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtc_config_ = config;
  }

  size_t
  PeerManager::connected_count() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = 0;
    for (const auto &[id, peer] : peers_) {
      if (peer->state() == PeerState::CONNECTED) {
        count++;
      }
    }
    return count;
  }

}  // namespace webrtc
