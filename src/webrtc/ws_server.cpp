/**
 * @file src/webrtc/ws_server.cpp
 * @brief WebSocket server implementation using Boost.Beast
 */

#include "ws_server.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "src/logging.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace webrtc {

  // Forward declaration of Impl
  class WebSocketServerImpl;

  /**
   * @brief Plain WebSocket session
   */
  class Session : public std::enable_shared_from_this<Session> {
  public:
    Session(tcp::socket socket, std::shared_ptr<WebSocketServerImpl> server);

    void run();
    void send(const std::string &message);
    void close();

  private:
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_send(const std::string &message);

    websocket::stream<beast::tcp_stream> ws_;
    std::shared_ptr<WebSocketServerImpl> server_;
    beast::flat_buffer buffer_;
    ws_connection_id conn_id_;
  };

  /**
   * @brief SSL WebSocket session
   */
  class SSLSession : public std::enable_shared_from_this<SSLSession> {
  public:
    SSLSession(tcp::socket socket, ssl::context &ctx, std::shared_ptr<WebSocketServerImpl> server);

    void run();
    void send(const std::string &message);
    void close();

  private:
    void on_ssl_handshake(beast::error_code ec);
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_send(const std::string &message);

    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    std::shared_ptr<WebSocketServerImpl> server_;
    beast::flat_buffer buffer_;
    ws_connection_id conn_id_;
  };

  /**
   * @brief WebSocket server implementation
   */
  class WebSocketServerImpl : public std::enable_shared_from_this<WebSocketServerImpl> {
  public:
    WebSocketServerImpl() :
        ioc_(1),
        acceptor_(ioc_),
        ssl_ctx_(ssl::context::tlsv12_server),
        running_(false),
        next_conn_id_(1) {}

    ~WebSocketServerImpl() {
      stop();
    }

    bool start(uint16_t port, bool use_ssl,
               const std::string &cert_path,
               const std::string &key_path) {
      if (running_) {
        return false;
      }

      use_ssl_ = use_ssl;
      port_ = port;

      // Configure SSL if needed
      if (use_ssl) {
        try {
          ssl_ctx_.use_certificate_chain_file(cert_path);
          ssl_ctx_.use_private_key_file(key_path, ssl::context::pem);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "WebSocket SSL setup failed: " << e.what();
          return false;
        }
      }

      // Open the acceptor
      beast::error_code ec;
      tcp::endpoint endpoint(tcp::v6(), port);

      acceptor_.open(endpoint.protocol(), ec);
      if (ec) {
        // Try IPv4 if IPv6 fails
        endpoint = tcp::endpoint(tcp::v4(), port);
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
          BOOST_LOG(error) << "WebSocket acceptor open failed: " << ec.message();
          return false;
        }
      }

      acceptor_.set_option(net::socket_base::reuse_address(true), ec);
      acceptor_.bind(endpoint, ec);
      if (ec) {
        BOOST_LOG(error) << "WebSocket acceptor bind failed: " << ec.message();
        return false;
      }

      acceptor_.listen(net::socket_base::max_listen_connections, ec);
      if (ec) {
        BOOST_LOG(error) << "WebSocket acceptor listen failed: " << ec.message();
        return false;
      }

      running_ = true;

      // Start accepting connections
      do_accept();

      // Start the I/O context in a thread
      io_thread_ = std::thread([this]() {
        BOOST_LOG(info) << "WebSocket signaling server started on port " << port_;
        ioc_.run();
        BOOST_LOG(info) << "WebSocket signaling server stopped";
      });

      return true;
    }

    void stop() {
      if (!running_) {
        return;
      }

      running_ = false;

      // Close all connections
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
        ssl_sessions_.clear();
      }

      // Stop the I/O context
      beast::error_code ec;
      acceptor_.close(ec);
      ioc_.stop();

      if (io_thread_.joinable()) {
        io_thread_.join();
      }
    }

    bool is_running() const {
      return running_;
    }

    bool send(ws_connection_id conn_id, const std::string &message) {
      std::lock_guard<std::mutex> lock(sessions_mutex_);

      if (use_ssl_) {
        auto it = ssl_sessions_.find(conn_id);
        if (it != ssl_sessions_.end()) {
          it->second->send(message);
          return true;
        }
      } else {
        auto it = sessions_.find(conn_id);
        if (it != sessions_.end()) {
          it->second->send(message);
          return true;
        }
      }

      return false;
    }

    void broadcast(const std::string &message) {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      if (use_ssl_) {
        for (auto &[id, session] : ssl_sessions_) {
          session->send(message);
        }
      } else {
        for (auto &[id, session] : sessions_) {
          session->send(message);
        }
      }
    }

    void close_connection(ws_connection_id conn_id) {
      std::lock_guard<std::mutex> lock(sessions_mutex_);

      if (use_ssl_) {
        auto it = ssl_sessions_.find(conn_id);
        if (it != ssl_sessions_.end()) {
          it->second->close();
          ssl_sessions_.erase(it);
        }
      } else {
        auto it = sessions_.find(conn_id);
        if (it != sessions_.end()) {
          it->second->close();
          sessions_.erase(it);
        }
      }
    }

    void set_message_callback(message_callback_t callback) {
      message_callback_ = std::move(callback);
    }

    void set_connect_callback(connection_callback_t callback) {
      connect_callback_ = std::move(callback);
    }

    void set_disconnect_callback(connection_callback_t callback) {
      disconnect_callback_ = std::move(callback);
    }

    size_t connection_count() const {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      return use_ssl_ ? ssl_sessions_.size() : sessions_.size();
    }

    // Called by sessions
    void on_message(ws_connection_id conn_id, const std::string &message) {
      if (message_callback_) {
        message_callback_(conn_id, message);
      }
    }

    void on_connect(ws_connection_id conn_id) {
      if (connect_callback_) {
        connect_callback_(conn_id);
      }
    }

    void on_disconnect(ws_connection_id conn_id) {
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(conn_id);
        ssl_sessions_.erase(conn_id);
      }
      if (disconnect_callback_) {
        disconnect_callback_(conn_id);
      }
    }

    ws_connection_id register_session(std::shared_ptr<Session> session) {
      ws_connection_id id = next_conn_id_++;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[id] = std::move(session);
      }
      return id;
    }

    ws_connection_id register_ssl_session(std::shared_ptr<SSLSession> session) {
      ws_connection_id id = next_conn_id_++;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        ssl_sessions_[id] = std::move(session);
      }
      return id;
    }

  private:
    void do_accept() {
      acceptor_.async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(&WebSocketServerImpl::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
      if (ec) {
        if (running_) {
          BOOST_LOG(error) << "WebSocket accept error: " << ec.message();
        }
      } else {
        if (use_ssl_) {
          auto session = std::make_shared<SSLSession>(
            std::move(socket), ssl_ctx_, shared_from_this());
          session->run();
        } else {
          auto session = std::make_shared<Session>(
            std::move(socket), shared_from_this());
          session->run();
        }
      }

      // Accept next connection
      if (running_) {
        do_accept();
      }
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    ssl::context ssl_ctx_;
    std::thread io_thread_;
    std::atomic<bool> running_;
    bool use_ssl_ = false;
    uint16_t port_ = 0;

    mutable std::mutex sessions_mutex_;
    std::unordered_map<ws_connection_id, std::shared_ptr<Session>> sessions_;
    std::unordered_map<ws_connection_id, std::shared_ptr<SSLSession>> ssl_sessions_;
    std::atomic<ws_connection_id> next_conn_id_;

    message_callback_t message_callback_;
    connection_callback_t connect_callback_;
    connection_callback_t disconnect_callback_;
  };

  // Session implementation

  Session::Session(tcp::socket socket, std::shared_ptr<WebSocketServerImpl> server) :
      ws_(std::move(socket)),
      server_(std::move(server)),
      conn_id_(0) {}

  void Session::run() {
    // Set suggested timeout settings for the websocket
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    // Set a decorator to change the Server header
    ws_.set_option(websocket::stream_base::decorator([](websocket::response_type &res) {
      res.set(http::field::server, "Sunshine WebRTC");
    }));

    // Accept the websocket handshake
    ws_.async_accept(beast::bind_front_handler(&Session::on_accept, shared_from_this()));
  }

  void Session::send(const std::string &message) {
    net::post(ws_.get_executor(), beast::bind_front_handler(&Session::do_send, shared_from_this(), message));
  }

  void Session::close() {
    net::post(ws_.get_executor(), [self = shared_from_this()]() {
      beast::error_code ec;
      self->ws_.close(websocket::close_code::normal, ec);
    });
  }

  void Session::on_accept(beast::error_code ec) {
    if (ec) {
      BOOST_LOG(error) << "WebSocket accept error: " << ec.message();
      return;
    }

    // Register with server and get connection ID
    conn_id_ = server_->register_session(shared_from_this());
    server_->on_connect(conn_id_);

    // Start reading
    do_read();
  }

  void Session::do_read() {
    ws_.async_read(buffer_, beast::bind_front_handler(&Session::on_read, shared_from_this()));
  }

  void Session::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec == websocket::error::closed) {
      server_->on_disconnect(conn_id_);
      return;
    }

    if (ec) {
      BOOST_LOG(error) << "WebSocket read error: " << ec.message();
      server_->on_disconnect(conn_id_);
      return;
    }

    // Handle the message
    std::string message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    server_->on_message(conn_id_, message);

    // Continue reading
    do_read();
  }

  void Session::do_send(const std::string &message) {
    // Make a copy for the async operation
    auto msg = std::make_shared<std::string>(message);
    ws_.async_write(net::buffer(*msg),
      [self = shared_from_this(), msg](beast::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(error) << "WebSocket write error: " << ec.message();
        }
      });
  }

  // SSLSession implementation

  SSLSession::SSLSession(tcp::socket socket, ssl::context &ctx, std::shared_ptr<WebSocketServerImpl> server) :
      ws_(std::move(socket), ctx),
      server_(std::move(server)),
      conn_id_(0) {}

  void SSLSession::run() {
    // Perform the SSL handshake
    ws_.next_layer().async_handshake(
      ssl::stream_base::server,
      beast::bind_front_handler(&SSLSession::on_ssl_handshake, shared_from_this()));
  }

  void SSLSession::send(const std::string &message) {
    net::post(ws_.get_executor(), beast::bind_front_handler(&SSLSession::do_send, shared_from_this(), message));
  }

  void SSLSession::close() {
    net::post(ws_.get_executor(), [self = shared_from_this()]() {
      beast::error_code ec;
      self->ws_.close(websocket::close_code::normal, ec);
    });
  }

  void SSLSession::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
      BOOST_LOG(error) << "WebSocket SSL handshake error: " << ec.message();
      return;
    }

    // Set suggested timeout settings for the websocket
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    // Set a decorator to change the Server header
    ws_.set_option(websocket::stream_base::decorator([](websocket::response_type &res) {
      res.set(http::field::server, "Sunshine WebRTC");
    }));

    // Accept the websocket handshake
    ws_.async_accept(beast::bind_front_handler(&SSLSession::on_accept, shared_from_this()));
  }

  void SSLSession::on_accept(beast::error_code ec) {
    if (ec) {
      BOOST_LOG(error) << "WebSocket accept error: " << ec.message();
      return;
    }

    // Register with server and get connection ID
    conn_id_ = server_->register_ssl_session(shared_from_this());
    server_->on_connect(conn_id_);

    // Start reading
    do_read();
  }

  void SSLSession::do_read() {
    ws_.async_read(buffer_, beast::bind_front_handler(&SSLSession::on_read, shared_from_this()));
  }

  void SSLSession::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec == websocket::error::closed) {
      server_->on_disconnect(conn_id_);
      return;
    }

    if (ec) {
      BOOST_LOG(error) << "WebSocket read error: " << ec.message();
      server_->on_disconnect(conn_id_);
      return;
    }

    // Handle the message
    std::string message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    server_->on_message(conn_id_, message);

    // Continue reading
    do_read();
  }

  void SSLSession::do_send(const std::string &message) {
    // Make a copy for the async operation
    auto msg = std::make_shared<std::string>(message);
    ws_.async_write(net::buffer(*msg),
      [self = shared_from_this(), msg](beast::error_code ec, std::size_t) {
        if (ec) {
          BOOST_LOG(error) << "WebSocket write error: " << ec.message();
        }
      });
  }

  // WebSocketServer PIMPL wrapper - stores shared_ptr to Impl

  class WebSocketServer::Impl {
  public:
    std::shared_ptr<WebSocketServerImpl> server;

    Impl() : server(std::make_shared<WebSocketServerImpl>()) {}
  };

  WebSocketServer::WebSocketServer() :
      impl_(std::make_unique<Impl>()) {}

  WebSocketServer::~WebSocketServer() = default;

  bool WebSocketServer::start(uint16_t port, bool use_ssl,
                              const std::string &cert_path,
                              const std::string &key_path) {
    return impl_->server->start(port, use_ssl, cert_path, key_path);
  }

  void WebSocketServer::stop() {
    impl_->server->stop();
  }

  bool WebSocketServer::is_running() const {
    return impl_->server->is_running();
  }

  bool WebSocketServer::send(ws_connection_id conn_id, const std::string &message) {
    return impl_->server->send(conn_id, message);
  }

  void WebSocketServer::broadcast(const std::string &message) {
    impl_->server->broadcast(message);
  }

  void WebSocketServer::close_connection(ws_connection_id conn_id) {
    impl_->server->close_connection(conn_id);
  }

  void WebSocketServer::set_message_callback(message_callback_t callback) {
    impl_->server->set_message_callback(std::move(callback));
  }

  void WebSocketServer::set_connect_callback(connection_callback_t callback) {
    impl_->server->set_connect_callback(std::move(callback));
  }

  void WebSocketServer::set_disconnect_callback(connection_callback_t callback) {
    impl_->server->set_disconnect_callback(std::move(callback));
  }

  size_t WebSocketServer::connection_count() const {
    return impl_->server->connection_count();
  }

  // Global instance
  WebSocketServer &ws_server() {
    static WebSocketServer instance;
    return instance;
  }

}  // namespace webrtc
