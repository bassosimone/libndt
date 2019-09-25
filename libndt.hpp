// Part of Measurement Kit <https://measurement-kit.github.io/>.
// Measurement Kit is free software under the BSD license. See AUTHORS
// and LICENSE for more information on the copying conditions.
#ifndef MEASUREMENT_KIT_LIBNDT_HPP
#define MEASUREMENT_KIT_LIBNDT_HPP

// TODO(bassosimone): run through cppcheck and attempt to minimize warnings.

/// \file libndt.hpp
///
/// \brief Public header of measurement-kit/libndt. The basic usage is a simple
/// as creating a `libndt::Client c` instance and then calling `c.run()`. More
/// advanced usage may require you to create a subclass of `libndt::Client` and
/// override specific virtual methods to customize the behaviour.
///
/// This implementation provides the C2S and S2C NDT subtests. We implement
/// NDT over TLS and NDT over websocket. For more information on the NDT
/// protocol, \see https://github.com/ndt-project/ndt/wiki/NDTProtocol.
///
/// The NDT protocol described above is version 5 (aka ndt5). The code in this
/// library also implements the ndt7 specification, which is described at
/// \see https://github.com/m-lab/ndt-server/blob/master/spec/ndt7-protocol.md.
///
/// Throughout this file, we'll use NDT to indicate ndt5 and ndt7 explicitly
/// to indicate version 7 of the protocol. Please, use ndt7 in newer code and
/// stick to ndt5 only if backwards compatibility is necessary.
///
/// \remark As a general rule, what is not documented using Doxygen comments
/// inside of this file is considered either internal or experimental. We
/// recommend you to only use documented interfaces.
///
/// Usage example follows. We assume that you have downloaded the single include
/// headers of nlohmann/json >= 3.0.0 and of libndt.
///
/// ```
/// #include "json.hpp"
/// #include "libndt.hpp"
/// measurement_kit::libndt::Client client;
/// client.run();
/// ```
///
/// \warning Not including nlohmann/json before including libndt will cause
/// the build to fail, because libndt uses nlohmann/json symbols.

// Check dependencies
// ``````````````````
#ifndef NLOHMANN_JSON_VERSION_MAJOR
#error "Libndt depends on nlohmann/json. Include nlohmann/json before including libndt."
#endif  // !NLOHMANN_JSON_VERSION_MAJOR
#if NLOHMANN_JSON_VERSION_MAJOR < 3
#error "Libndt requires nlohmann/json >= 3"
#endif

#ifndef _WIN32
#include <sys/socket.h>
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <assert.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <curl/curl.h>

namespace measurement_kit {
namespace libndt {

// Versioning
// ``````````

/// Type containing a version number.
using Version = unsigned int;

/// Major API version number of measurement-kit/libndt.
constexpr Version version_major = Version{0};

/// Minor API version number of measurement-kit/libndt.
constexpr Version version_minor = Version{27};

/// Patch API version number of measurement-kit/libndt.
constexpr Version version_patch = Version{0};

// Flags for selecting subtests
// ````````````````````````````

/// Flags that indicate what subtests to run.
using NettestFlags = unsigned char;

constexpr NettestFlags nettest_flag_middlebox = NettestFlags{1U << 0};

/// Run the upload subtest.
constexpr NettestFlags nettest_flag_upload = NettestFlags{1U << 1};

/// Run the download subtest.
constexpr NettestFlags nettest_flag_download = NettestFlags{1U << 2};

constexpr NettestFlags nettest_flag_simple_firewall = NettestFlags{1U << 3};

constexpr NettestFlags nettest_flag_status = NettestFlags{1U << 4};

constexpr NettestFlags nettest_flag_meta = NettestFlags{1U << 5};

constexpr NettestFlags nettest_flag_upload_ext = NettestFlags{1U << 6};

/// Run the multi-stream download subtest.
constexpr NettestFlags nettest_flag_download_ext = NettestFlags{1U << 7};

// Verbosity levels
// ````````````````

/// Library's logging verbosity.
using Verbosity = unsigned int;

/// Do not emit any log message.
constexpr Verbosity verbosity_quiet = Verbosity{0};

/// Emit only warning messages.
constexpr Verbosity verbosity_warning = Verbosity{1};

/// Emit warning and informational messages.
constexpr Verbosity verbosity_info = Verbosity{2};

/// Emit all log messages.
constexpr Verbosity verbosity_debug = Verbosity{3};

// Portable types
// ``````````````

using Size = uint64_t;

constexpr Size SizeMax = UINT64_MAX;

using Ssize = int64_t;

#ifdef _WIN32
using Socket = SOCKET;
#else
using Socket = int;
#endif

constexpr bool is_socket_valid(Socket s) noexcept {
#ifdef _WIN32
  return s != INVALID_SOCKET;
#else
  return s >= 0;
#endif
}

// Flags for selecting what NDT protocol features to use
// `````````````````````````````````````````````````````

/// Flags to select what protocol should be used.
using ProtocolFlags = unsigned int;

/// When this flag is set we use JSON messages. This specifically means that
/// we send and receive JSON messages (as opposed to raw strings).
constexpr ProtocolFlags protocol_flag_json = ProtocolFlags{1 << 0};

/// When this flag is set we use TLS. This specifically means that we will
/// use TLS channels for the control and the measurement connections.
constexpr ProtocolFlags protocol_flag_tls = ProtocolFlags{1 << 1};

/// When this flag is set we use WebSocket. This specifically means that
/// we use the WebSocket framing to encapsulate NDT messages.
constexpr ProtocolFlags protocol_flag_websocket = ProtocolFlags{1 << 2};

/// When this flag is set, we use ndt7 rather than ndt5. This specifically
/// means that a totally different protocol is used. You can read more on ndt7
/// at https://github.com/m-lab/ndt-server/blob/master/spec/ndt7-protocol.md
constexpr ProtocolFlags protocol_flag_ndt7 = ProtocolFlags{1 << 3};

// Policy for auto-selecting a NDT server
// ``````````````````````````````````````

/// Flags modifying the behavior of mlab-ns. Mlab-ns is the web service used
/// to automatically discover NDT's (and other experiments') servers.
using MlabnsPolicy = unsigned short;

/// Request just the closest NDT server.
constexpr MlabnsPolicy mlabns_policy_closest = MlabnsPolicy{0};

/// Request for a random NDT server.
constexpr MlabnsPolicy mlabns_policy_random = MlabnsPolicy{1};

/// Return a list of nearby NDT servers. When more than one server is returned
/// all the available servers will be tried in case some of them are down.
constexpr MlabnsPolicy mlabns_policy_geo_options = MlabnsPolicy{2};

// NDT message types
// `````````````````
// See <https://github.com/ndt-project/ndt/wiki/NDTProtocol#message-types>.

using MsgType = unsigned char;
constexpr MsgType msg_comm_failure = MsgType{0};
constexpr MsgType msg_srv_queue = MsgType{1};
constexpr MsgType msg_login = MsgType{2};
constexpr MsgType msg_test_prepare = MsgType{3};
constexpr MsgType msg_test_start = MsgType{4};
constexpr MsgType msg_test_msg = MsgType{5};
constexpr MsgType msg_test_finalize = MsgType{6};
constexpr MsgType msg_error = MsgType{7};
constexpr MsgType msg_results = MsgType{8};
constexpr MsgType msg_logout = MsgType{9};
constexpr MsgType msg_waiting = MsgType{10};
constexpr MsgType msg_extended_login = MsgType{11};

// Settings
// ````````

/// Timeout expressed in seconds.
using Timeout = unsigned int;

constexpr const char *ndt_version_compat = "v3.7.0";

/// NDT client settings. If you do not customize the settings when creating
/// a Client, the defaults listed below will be used instead.
class Settings {
 public:
  /// Base URL to be used to query the mlab-ns service. If you specify an
  /// explicit hostname, mlab-ns won't be used. Note that the URL specified
  /// here MUST NOT end with a final slash.
  std::string mlabns_base_url = "https://locate.measurementlab.net";

  /// Flags that modify the behavior of mlabn-ns. By default we use the
  /// geo_options policy that is the most robust to random server failures.
  MlabnsPolicy mlabns_policy = mlabns_policy_geo_options;

  /// Timeout used for I/O operations.
  Timeout timeout = Timeout{7} /* seconds */;

  /// Host name of the NDT server to use. If this is left blank (the default),
  /// we will use mlab-ns to discover a nearby server.
  std::string hostname;

  /// Port of the NDT server to use. If this is not specified, we will use
  /// the most correct port depending on the configuration.
  std::string port;

  /// The tests you want to run with the NDT server. By default we run
  /// a download test, because that is probably the typical usage.
  NettestFlags nettest_flags = nettest_flag_download;

  /// Verbosity of the client. By default no message is emitted. Set to other
  /// values to get more messages (useful when debugging).
  Verbosity verbosity = verbosity_quiet;

  /// Metadata to include in the server side logs. By default we just identify
  /// the NDT version and the application.
  std::map<std::string, std::string> metadata{
      {"client.version", ndt_version_compat},
      {"client.application", "measurement-kit/libndt"},
  };

  /// Type of NDT protocol that you want to use. Selecting the protocol may
  /// cause libndt to use different default settings for the port or for
  /// mlab-ns. Clear text NDT uses port 3001, NDT-over-TLS uses 3010. There
  /// will most likely be servers listening on port 443 in the future, but
  /// they will only support the TLS+WebSocket protocol.
  ProtocolFlags protocol_flags = ProtocolFlags{0};

  /// Maximum time for which a nettest (i.e. download) is allowed to run. After
  /// this time has elapsed, the code will stop downloading (or uploading). It
  /// is meant as a safeguard to prevent the test for running for much more time
  /// than anticipated, due to buffering and/or changing network conditions.
  Timeout max_runtime = Timeout{14} /* seconds */;

  /// SOCKSv5h port to use for tunnelling traffic using, e.g., Tor. If non
  /// empty, all DNS and TCP traffic should be tunnelled over such port.
  std::string socks5h_port;

  /// CA bundle path to be used to verify TLS connections. If you do not
  /// set this variable and you're on Unix, we'll attempt to use some reasonable
  /// default value. Otherwise, the test will fail (unless you set the
  /// tls_verify_peer setting to false, indicating that you do not care about
  /// verifying the peer -- insecure, not recommended).
  std::string ca_bundle_path;

  /// Whether to use the CA bundle and OpenSSL's builtin hostname validation to
  /// make sure we are talking to the correct host. Enabled by default, but it
  /// may be useful sometimes to disable it for testing purposes. You should
  /// not disable this option in general, since doing that is insecure.
  bool tls_verify_peer = true;
};

// Error codes
// ```````````

enum class Err {
  none,
  //
  // Error codes that map directly to errno values. Here we use the naming used
  // by the C++ library <https://en.cppreference.com/w/cpp/error/errc>.
  //
  broken_pipe,
  connection_aborted,
  connection_refused,
  connection_reset,
  function_not_supported,
  host_unreachable,
  interrupted,
  invalid_argument,
  io_error,
  message_size,
  network_down,
  network_reset,
  network_unreachable,
  operation_in_progress,
  operation_would_block,
  timed_out,
  value_too_large,
  //
  // Getaddrinfo() error codes. See <http://man.openbsd.org/gai_strerror>.
  //
  ai_generic,
  ai_again,
  ai_fail,
  ai_noname,
  //
  // SSL error codes. See <http://man.openbsd.org/SSL_get_error>.
  //
  ssl_generic,
  ssl_want_read,
  ssl_want_write,
  ssl_syscall,
  //
  // Libndt misc error codes.
  //
  eof,       // We got an unexpected EOF
  socks5h,   // SOCKSv5 protocol error
  ws_proto,  // WebSocket protocol error
};

// Client
// ``````

/// NDT client. In the typical usage, you just need to construct a Client,
/// optionally providing settings, and to call the run() method. More advanced
/// usage may require you to override methods in a subclass to customize the
/// default behavior. For instance, you may probably want to override the
/// on_result() method that is called when processing NDT results to either
/// show such results to a user or store them on the disk.
class Client {
 public:
  /// Constructs a Client with default settings.
  Client() noexcept;

  /// Deleted copy constructor.
  Client(const Client &) noexcept = delete;

  /// Deleted copy assignment.
  Client &operator=(const Client &) noexcept = delete;

  /// Deleted move constructor.
  Client(Client &&) noexcept = delete;

  /// Deleted move assignment.
  Client &operator=(Client &&) noexcept = delete;

  /// Constructs a Client with the specified @p settings.
  explicit Client(Settings settings) noexcept;

  /// Destroys a Client.
  virtual ~Client() noexcept;

  /// Runs a NDT test using the configured (or default) settings.
  bool run() noexcept;

  // Implementation note: currently SWIG does not propagate `noexcept` even
  // though that is implemented in master [1], hence we have removed this
  // qualifiers from the functions that SWIG needs to wrap.
  //
  // .. [1] https://github.com/swig/swig/issues/526

  /// Called when a warning message is emitted. The default behavior is to write
  /// the warning onto the `std::clog` standard stream. \warning This method
  /// could be called from a different thread context.
  virtual void on_warning(const std::string &s) const;

  /// Called when an informational message is emitted. The default behavior is
  /// to write the message onto the `std::clog` standard stream. \warning This method
  /// could be called from a different thread context.
  virtual void on_info(const std::string &s) const;

  /// Called when a debug message is emitted. The default behavior is
  /// to write the message onto the `std::clog` standard stream. \warning This method
  /// could be called from a different thread context.
  virtual void on_debug(const std::string &s) const;

  /// Called to inform you about the measured speed. The default behavior is
  /// to write the provided information as an info message. @param tid is either
  /// nettest_flag_download or nettest_flag_upload. @param nflows is the number
  /// of used flows. @param measured_bytes is the number of bytes received
  /// or sent since the beginning of the measurement. @param elapsed
  /// is the number of seconds elapsed since the beginning of the nettest.
  /// @param max_runtime is the maximum runtime of this nettest, as copied from
  /// the Settings. @remark By dividing @p elapsed by @p max_runtime, you can
  /// get the percentage of completion of the current nettest. @remark We
  /// provide you with @p tid, so you know whether the nettest is downloading
  /// bytes from the server or uploading bytes to the server. \warning This
  /// method could be called from another thread context.
  virtual void on_performance(NettestFlags tid, uint8_t nflows,
                              double measured_bytes, double elapsed,
                              double max_runtime);

  /// Called to provide you with NDT results. The default behavior is
  /// to write the provided information as an info message. @param scope is
  /// "web100", when we're passing you Web 100 variables, "tcp_info" when
  /// we're passing you TCP info variables, "summary" when we're passing you
  /// summary variables, or "ndt7" when we're passing you results returned
  /// by a ndt7 server. @param name is the name of the variable; if @p scope
  /// is "ndt7", then @p name should be "download". @param value is the
  /// variable value; variables are typically int, float, or string when
  /// running ndt5 tests, instead they are serialized JSON returned by the
  /// server when running a ndt7 test. \warning This method could be called
  /// from another thread context.
  virtual void on_result(std::string scope, std::string name,
                         std::string value);

  /// Called when the server is busy. The default behavior is to write a
  /// warning message. @param msg is the reason why the server is busy, encoded
  /// according to the NDT protocol. @remark when Settings::hostname is empty,
  /// we will autodiscover one or more servers, depending on the configured
  /// policy; in the event in which we autodiscover more than one server, we
  /// will attempt to use each of them, hence, this method may be called more
  /// than once if some of these servers happen to be busy. \warning This
  /// method could be called from another thread context.
  virtual void on_server_busy(std::string msg);

  /*
               _        __             _    _ _                _
   ___ _ _  __| |  ___ / _|  _ __ _  _| |__| (_)__   __ _ _ __(_)
  / -_) ' \/ _` | / _ \  _| | '_ \ || | '_ \ | / _| / _` | '_ \ |
  \___|_||_\__,_| \___/_|   | .__/\_,_|_.__/_|_\__| \__,_| .__/_|
                            |_|                          |_|
  */
  // If you're just interested to use measurement-kit/libndt, you can stop
  // reading right here. All the remainder of this file is not documented on
  // purpose and contains functionality that you'll typically don't care about
  // unless you're looking into heavily customizing this library.
  //
  // SWIG should not see anything below this point otherwise it will attempt
  // to create wrappers for that. TODO(bassosimone): it should be evaluated in
  // the future whether it makes sense enforcing `protected` here. This is
  // certainly feasible but would require some refactoring.
#ifdef SWIG
 private:
#endif

  // High-level API
  virtual bool query_mlabns(std::vector<std::string> *) noexcept;
  virtual bool connect() noexcept;
  virtual bool send_login() noexcept;
  virtual bool recv_kickoff() noexcept;
  virtual bool wait_in_queue() noexcept;
  virtual bool recv_version() noexcept;
  virtual bool recv_tests_ids() noexcept;
  virtual bool run_tests() noexcept;
  virtual bool recv_results_and_logout() noexcept;
  virtual bool wait_close() noexcept;

  // Mid-level API
  virtual bool run_download() noexcept;
  virtual bool run_meta() noexcept;
  virtual bool run_upload() noexcept;

  // ndt7 protocol API
  // `````````````````
  //
  // This API allows you to perform ndt7 tests. The plan is to increasingly
  // use ndt7 code and eventually deprecate and remove NDT.
  //
  // Note that we cannot have ndt7 without OpenSSL.

  // ndt7_download performs a ndt7 download. Returns true if the download
  // succeeds and false in case of failure.
  bool ndt7_download() noexcept;

  // ndt7_upload is like ndt7_download but performs an upload.
  bool ndt7_upload() noexcept;

  // ndt7_connect connects to @p url_path.
  bool ndt7_connect(std::string url_path) noexcept;

  // NDT protocol API
  // ````````````````
  //
  // This API allows to send and receive NDT messages. At the bottom of the
  // abstraction layer lie functions to send and receive NDT's binary protocol
  // which here is called "legacy". It's called like this because it's still
  // the original protocol, AFAIK, even though several additions were layered
  // on top of it over the years (i.e. websocket, JSON, and TLS).

  bool msg_write_login(const std::string &version) noexcept;

  virtual bool msg_write(MsgType code, std::string &&msg) noexcept;

  virtual bool msg_write_legacy(MsgType code, std::string &&msg) noexcept;

  virtual bool msg_expect_test_prepare(  //
      std::string *pport, uint8_t *pnflows) noexcept;

  virtual bool msg_expect_empty(MsgType code) noexcept;

  virtual bool msg_expect(MsgType code, std::string *msg) noexcept;

  virtual bool msg_read(MsgType *code, std::string *msg) noexcept;

  virtual bool msg_read_legacy(MsgType *code, std::string *msg) noexcept;

  // WebSocket
  // `````````
  //
  // This section contain a WebSocket implementation.

  // Send @p line over @p fd.
  virtual Err ws_sendln(Socket fd, std::string line) noexcept;

  // Receive shorter-than @p maxlen @p *line over @p fd.
  virtual Err ws_recvln(Socket fd, std::string *line, size_t maxlen) noexcept;

  // Perform websocket handshake. @param fd is the socket to use. @param
  // ws_flags specifies what headers to send and to expect (for more information
  // see the ws_f_xxx constants defined below). @param ws_protocol specifies
  // what protocol to specify as Sec-WebSocket-Protocol in the upgrade request.
  // @param port is used to construct the Host header. @param url_path is the
  // URL path to use for performing the websocket upgrade.
  virtual Err ws_handshake(Socket fd, std::string port, uint64_t ws_flags,
                           std::string ws_protocol,
                           std::string url_path) noexcept;

  // Prepare and return a WebSocket frame containing @p first_byte and
  // the content of @p base and @p count as payload. If @p base is nullptr
  // then we'll just not include a body in the prepared frame.
  virtual std::string ws_prepare_frame(uint8_t first_byte, uint8_t *base,
                                       Size count) const noexcept;

  // Send @p count bytes from @p base over @p sock as a frame whose first byte
  // @p first_byte should contain the opcode and possibly the FIN flag.
  virtual Err ws_send_frame(Socket sock, uint8_t first_byte, uint8_t *base,
                            Size count) const noexcept;

  // Receive a frame from @p sock. Puts the opcode in @p *opcode. Puts whether
  // there is a FIN flag in @p *fin. The buffer starts at @p base and it
  // contains @p total bytes. Puts in @p *count the actual number of bytes
  // in the message. @return The error that occurred or Err::none.
  Err ws_recv_any_frame(Socket sock, uint8_t *opcode, bool *fin, uint8_t *base,
                        Size total, Size *count) const noexcept;

  // Receive a frame. Automatically and transparently responds to PING, ignores
  // PONG, and handles CLOSE frames. Arguments like ws_recv_any_frame().
  Err ws_recv_frame(Socket sock, uint8_t *opcode, bool *fin, uint8_t *base,
                    Size total, Size *count) const noexcept;

  // Receive a message consisting of one or more frames. Transparently handles
  // PING and PONG frames. Handles CLOSE frames. @param sock is the socket to
  // use. @param opcode is where the opcode is returned. @param base is the
  // beginning of the buffer. @param total is the size of the buffer. @param
  // count contains the actual message size. @return An error on failure or
  // Err::none in case of success.
  Err ws_recvmsg(Socket sock, uint8_t *opcode, uint8_t *base, Size total,
                 Size *count) const noexcept;

  // Networking layer
  // ````````````````
  //
  // This section contains network functionality used by NDT. The functionality
  // to connect to a remote host is layered to comply with the websocket spec
  // as follows:
  //
  // - netx_maybews_dial() calls netx_maybessl_dial() and, if that succeeds, it
  //   then attempts to negotiate a websocket channel (if enabled);
  //
  // - netx_maybessl_dial() calls netx_maybesocks5h_dial() and, if that
  //   suceeds, it then attempts to establish a TLS connection (if enabled);
  //
  // - netx_maybesocks5h_dial() possibly creates the connection through a
  //   SOCKSv5h proxy (if the proxy is enabled).
  //
  // By default with TLS we use a CA and we perform SNI validation. That can be
  // disabled for debug reasons. Doing that breaks compliancy with the websocket
  // spec. See <https://tools.ietf.org/html/rfc6455#section-4.1>.

  // Connect to @p hostname and @p port possibly using WebSocket,
  // SSL, and SOCKSv5. This depends on the Settings. See the documentation
  // of ws_handshake() for more info on @p ws_flags, @p ws_protocol, and
  // @p url_path.
  virtual Err netx_maybews_dial(const std::string &hostname,
                                const std::string &port, uint64_t ws_flags,
                                std::string ws_protocol, std::string url_path,
                                Socket *sock) noexcept;

  // Connect to @p hostname and @p port possibly using SSL and SOCKSv5. This
  // depends on the Settings you configured.
  virtual Err netx_maybessl_dial(const std::string &hostname,
                                 const std::string &port,
                                 Socket *sock) noexcept;

  // Connect to @p hostname and @port possibly using SOCKSv5. This depends
  // on the Settings you configured.
  virtual Err netx_maybesocks5h_dial(const std::string &hostname,
                                     const std::string &port,
                                     Socket *sock) noexcept;

  // Map errno code into a Err value.
  static Err netx_map_errno(int ec) noexcept;

  // Map getaddrinfo return value into a Err value.
  Err netx_map_eai(int ec) noexcept;

  // Connect to @p hostname and @p port.
  virtual Err netx_dial(const std::string &hostname, const std::string &port,
                        Socket *sock) noexcept;

  // Receive from the network.
  virtual Err netx_recv(Socket fd, void *base, Size count,
                        Size *actual) const noexcept;

  // Receive from the network without blocking.
  virtual Err netx_recv_nonblocking(Socket fd, void *base, Size count,
                                    Size *actual) const noexcept;

  // Receive exactly N bytes from the network.
  virtual Err netx_recvn(Socket fd, void *base, Size count) const noexcept;

  // Send data to the network.
  virtual Err netx_send(Socket fd, const void *base, Size count,
                        Size *actual) const noexcept;

  // Send to the network without blocking.
  virtual Err netx_send_nonblocking(Socket fd, const void *base, Size count,
                                    Size *actual) const noexcept;

  // Send exactly N bytes to the network.
  virtual Err netx_sendn(
    Socket fd, const void *base, Size count) const noexcept;

  // Resolve hostname into a list of IP addresses.
  virtual Err netx_resolve(const std::string &hostname,
                           std::vector<std::string> *addrs) noexcept;

  // Set socket non blocking.
  virtual Err netx_setnonblocking(Socket fd, bool enable) noexcept;

  // Pauses until the socket becomes readable.
  virtual Err netx_wait_readable(Socket, Timeout timeout) const noexcept;

  // Pauses until the socket becomes writeable.
  virtual Err netx_wait_writeable(Socket, Timeout timeout) const noexcept;

  // Main function for dealing with I/O patterned after poll(2).
  virtual Err netx_poll(
    std::vector<pollfd> *fds, int timeout_msec) const noexcept;

  // Shutdown both ends of a socket.
  virtual Err netx_shutdown_both(Socket fd) noexcept;

  // Close a socket.
  virtual Err netx_closesocket(Socket fd) noexcept;

  // Dependencies (cURL)
  // ```````````````````
  //
  // This section contains functionality used by cURL.
  class CurlDeleter {
   public:
    void operator()(CURL *handle) noexcept;
  };
  using UniqueCurl = std::unique_ptr<CURL, CurlDeleter>;

  bool curlx_get_maybe_socks5(const std::string &proxy_port,
                              const std::string &url, long timeout,
                              std::string *body) noexcept;

  bool curlx_get(UniqueCurl &, const std::string &url, long timeout,
                 std::string *body) noexcept;

  virtual CURLcode curlx_setopt_url(
                UniqueCurl &, const std::string &url) noexcept;

  virtual CURLcode curlx_setopt_proxy(UniqueCurl &,
                const std::string &url) noexcept;

  virtual CURLcode curlx_setopt_writefunction(UniqueCurl &, size_t (*callback)(
      char *ptr, size_t size, size_t nmemb, void *userdata)) noexcept;

  virtual CURLcode curlx_setopt_writedata(UniqueCurl &, void *pointer) noexcept;

  virtual CURLcode curlx_setopt_timeout(UniqueCurl &, long timeout) noexcept;

  virtual CURLcode curlx_setopt_failonerror(UniqueCurl &) noexcept;

  virtual CURLcode curlx_perform(UniqueCurl &) noexcept;

  virtual UniqueCurl curlx_easy_init() noexcept;

  virtual CURLcode curlx_getinfo_response_code(
    UniqueCurl &handle, long *response_code) noexcept;

  virtual bool query_mlabns_curl(const std::string &url, long timeout,
                                 std::string *body) noexcept;

  // Other helpers

  Verbosity get_verbosity() const noexcept;

  // Dependencies (system)
  // `````````````````````
  //
  // This section contains wrappers for system calls used in regress tests.

  // Access the value of errno in a portable way.
  virtual int sys_get_last_error() const noexcept;

  // Set the value of errno in a portable way.
  virtual void sys_set_last_error(int err) const noexcept;

  // getaddrinfo() wrapper that can be mocked in tests.
  virtual int sys_getaddrinfo(const char *domain, const char *port,
                              const addrinfo *hints, addrinfo **res) noexcept;

  // getnameinfo() wrapper that can be mocked in tests.
  virtual int sys_getnameinfo(const sockaddr *sa, socklen_t salen, char *host,
                              socklen_t hostlen, char *serv, socklen_t servlen,
                              int flags) noexcept;

  // freeaddrinfo() wrapper that can be mocked in tests.
  virtual void sys_freeaddrinfo(addrinfo *aip) noexcept;

  // socket() wrapper that can be mocked in tests.
  virtual Socket sys_socket(int domain, int type, int protocol) noexcept;

  // connect() wrapper that can be mocked in tests.
  virtual int sys_connect(Socket fd, const sockaddr *sa, socklen_t n) noexcept;

  // recv() wrapper that can be mocked in tests.
  virtual Ssize sys_recv(Socket fd, void *base, Size count) const noexcept;

  // send() wrapper that can be mocked in tests.
  virtual Ssize sys_send(
    Socket fd, const void *base, Size count) const noexcept;

  // shutdown() wrapper that can be mocked in tests.
  virtual int sys_shutdown(Socket fd, int shutdown_how) noexcept;

  // Portable wrapper for closing a socket descriptor.
  virtual int sys_closesocket(Socket fd) noexcept;

  // poll() wrapper that can be mocked in tests.
#ifdef _WIN32
  virtual int sys_poll(LPWSAPOLLFD fds, ULONG nfds, INT timeout) const noexcept;
#else
  virtual int sys_poll(pollfd *fds, nfds_t nfds, int timeout) const noexcept;
#endif

  // If strtonum() is available, wrapper that can be mocked in tests, else
  // implementation of strtonum() borrowed from OpenBSD.
  virtual long long sys_strtonum(const char *s, long long minval,
                                 long long maxval, const char **err) noexcept;

#ifdef _WIN32
  // ioctlsocket() wrapper that can be mocked in tests.
  virtual int sys_ioctlsocket(Socket s, long cmd, u_long *argp) noexcept;
#else
  // Wrapper for fcntl() taking just two arguments. Good for getting the
  // currently value of the socket flags.
  virtual int sys_fcntl(Socket s, int cmd) noexcept;

  // Wrapper for fcntl() taking three arguments with the third argument
  // being an integer value. Good for setting O_NONBLOCK on a socket.
  virtual int sys_fcntl(Socket s, int cmd, int arg) noexcept;
#endif

  // getsockopt() wrapper that can be mocked in tests.
  virtual int sys_getsockopt(Socket socket, int level, int name, void *value,
                             socklen_t *len) noexcept;

 private:
  class Winsock {
   public:
    Winsock() noexcept;
    Winsock(const Winsock &) = delete;
    Winsock &operator=(const Winsock &) = delete;
    Winsock(Winsock &&) = delete;
    Winsock &operator=(Winsock &&) = delete;
    ~Winsock() noexcept;
  };

  Socket sock_ = (Socket)-1;
  std::vector<NettestFlags> granted_suite_;
  Settings settings_;
  std::map<Socket, SSL *> fd_to_ssl_;
#ifdef _WIN32
  Winsock winsock_;
#endif
};

// Implementation section
// ``````````````````````
// This is a single header library. In some use cases you may want to split
// the interface and implementation using LIBNDT_NO_INLINE_IMPL.
#ifndef LIBNDT_NO_INLINE_IMPL

#ifdef __linux__
#include <linux/tcp.h>
#define NDT7_ENUM_TCP_INFO \
  XX(tcpi_state, TcpiState) \
  XX(tcpi_ca_state, TcpiCaState) \
  XX(tcpi_retransmits, TcpiRetransmits) \
  XX(tcpi_probes, TcpiProbes) \
  XX(tcpi_backoff, TcpiBackoff) \
  XX(tcpi_options, TcpiOptions) \
  XX(tcpi_snd_wscale, TcpiSndWscale) \
  XX(tcpi_rcv_wscale, TcpiRcvWscale) \
  XX(tcpi_delivery_rate_app_limited, TcpiDeliveryRateAppLimited) \
  XX(tcpi_rto, TcpiRto) \
  XX(tcpi_ato, TcpiAto) \
  XX(tcpi_snd_mss, TcpiSndMss) \
  XX(tcpi_rcv_mss, TcpiRcvMss) \
  XX(tcpi_unacked, TcpiUnacked) \
  XX(tcpi_sacked, TcpiSacked) \
  XX(tcpi_lost, TcpiLost) \
  XX(tcpi_retrans, TcpiRetrans) \
  XX(tcpi_fackets, TcpiFackets) \
  XX(tcpi_last_data_sent, TcpiLastDataSent) \
  XX(tcpi_last_ack_sent, TcpiLastAckSent) \
  XX(tcpi_last_data_recv, TcpiLastDataRecv) \
  XX(tcpi_last_ack_recv, TcpiLastAckRecv) \
  XX(tcpi_pmtu, TcpiPmtu) \
  XX(tcpi_rcv_ssthresh, TcpiRcvSsthresh) \
  XX(tcpi_rtt, TcpiRtt) \
  XX(tcpi_rttvar, TcpiRttvar) \
  XX(tcpi_snd_ssthresh, TcpiSndSsthresh) \
  XX(tcpi_snd_cwnd, TcpiSndCwnd) \
  XX(tcpi_advmss, TcpiAdvmss) \
  XX(tcpi_reordering, TcpiReordering) \
  XX(tcpi_rcv_rtt, TcpiRcvRtt) \
  XX(tcpi_rcv_space, TcpiRcvSpace) \
  XX(tcpi_total_retrans, TcpiTotalRetrans) \
  XX(tcpi_pacing_rate, TcpiPacingRate) \
  XX(tcpi_max_pacing_rate, TcpiMaxPacingRate) \
  XX(tcpi_bytes_acked, TcpiBytesAcked) \
  XX(tcpi_bytes_received, TcpiBytesReceived) \
  XX(tcpi_segs_out, TcpiSegsOut) \
  XX(tcpi_segs_in, TcpiSegsIn) \
  XX(tcpi_notsent_bytes, TcpiNotsentBytes) \
  XX(tcpi_min_rtt, TcpiMinRtt) \
  XX(tcpi_data_segs_in, TcpiDataSegsIn) \
  XX(tcpi_data_segs_out, TcpiDataSegsOut) \
  XX(tcpi_delivery_rate, TcpiDeliveryRate) \
  XX(tcpi_busy_time, TcpiBusyTime) \
  XX(tcpi_rwnd_limited, TcpiRwndLimited) \
  XX(tcpi_sndbuf_limited, TcpiSndbufLimited) \
  XX(tcpi_delivered, TcpiDelivered) \
  XX(tcpi_delivered_ce, TcpiDeliveredCe) \
  XX(tcpi_bytes_sent, TcpiBytesSent) \
  XX(tcpi_bytes_retrans, TcpiBytesRetrans) \
  XX(tcpi_dsack_dups, TcpiDsackDups) \
  XX(tcpi_reord_seen, TcpiReordSeen)
#endif // __linux__
// Strtonum
// ````````
#ifndef LIBNDT_HAVE_STRTONUM
// clang-format off
/*
 * Copyright (c) 2004 Ted Unangst and Todd Miller
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define	INVALID		1
#define	TOOSMALL	2
#define	TOOLARGE	3

static long long
strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp)
{
	long long ll = 0;
	int error = 0;
	char *ep;
	struct errval {
		const char *errstr;
		int err;
	} ev[4] = {
		{ NULL,		0 },
		{ "invalid",	EINVAL },
		{ "too small",	ERANGE },
		{ "too large",	ERANGE },
	};

	ev[0].err = errno;
	errno = 0;
	if (minval > maxval) {
		error = INVALID;
	} else {
		ll = strtoll(numstr, &ep, 10);
		if (numstr == ep || *ep != '\0')
			error = INVALID;
		else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval)
			error = TOOSMALL;
		else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval)
			error = TOOLARGE;
	}
	if (errstrp != NULL)
		*errstrp = ev[error].errstr;
	errno = ev[error].err;
	if (error)
		ll = 0;

	return (ll);
}
// clang-format on
#endif  // LIBNDT_HAVE_STRTONUM

// WebSocket constants
// ```````````````````

// Opcodes. See <https://tools.ietf.org/html/rfc6455#section-11.8>.
constexpr uint8_t ws_opcode_continue = 0;
constexpr uint8_t ws_opcode_text = 1;
constexpr uint8_t ws_opcode_binary = 2;
constexpr uint8_t ws_opcode_close = 8;
constexpr uint8_t ws_opcode_ping = 9;
constexpr uint8_t ws_opcode_pong = 10;

// Constants useful to process the first octet of a websocket frame. For more
// info see <https://tools.ietf.org/html/rfc6455#section-5.2>.
constexpr uint8_t ws_fin_flag = 0x80;
constexpr uint8_t ws_reserved_mask = 0x70;
constexpr uint8_t ws_opcode_mask = 0x0f;

// Constants useful to process the second octet of a websocket frame. For more
// info see <https://tools.ietf.org/html/rfc6455#section-5.2>.
constexpr uint8_t ws_mask_flag = 0x80;
constexpr uint8_t ws_len_mask = 0x7f;

// Flags used to specify what HTTP headers are required and present into the
// websocket handshake where we upgrade from HTTP/1.1 to websocket.
constexpr uint64_t ws_f_connection = 1 << 0;
constexpr uint64_t ws_f_sec_ws_accept = 1 << 1;
constexpr uint64_t ws_f_sec_ws_protocol = 1 << 2;
constexpr uint64_t ws_f_upgrade = 1 << 3;

// Values of Sec-WebSocket-Protocol used by ndt-project/ndt.
constexpr const char *ws_proto_control = "ndt";
constexpr const char *ws_proto_c2s = "c2s";
constexpr const char *ws_proto_s2c = "s2c";
constexpr const char *ws_proto_ndt7 = "net.measurementlab.ndt.v7";

// Private constants
// `````````````````

constexpr auto max_loops = 256;
constexpr char msg_kickoff[] = "123456 654321";
constexpr size_t msg_kickoff_size = sizeof(msg_kickoff) - 1;

// SIGPIPE correctness
// ```````````````````

#if !defined _WIN32 && !defined LIBNDT_HAVE_MSG_NOSIGNAL && !defined LIBNDT_HAVE_SO_NOSIGPIPE
#error "No way to avoid SIGPIPE in the current thread when doing socket I/O."
#endif

// Private utils
// `````````````

// Format OpenSSL error as a C++ string.
static std::string ssl_format_error() noexcept {
  std::stringstream ss;
  for (unsigned short i = 0; i < USHRT_MAX; ++i) {
    unsigned long err = ERR_get_error();
    if (err == 0) {
      break;
    }
    ss << ((i > 0) ? ": " : "") << ERR_reason_error_string(err);
  }
  return ss.str();
}

// Map an error code to the corresponding string value.
static std::string libndt_perror(Err err) noexcept {
  std::string rv;
  //
#define LIBNDT_PERROR(value) \
  case Err::value:           \
    rv = #value;             \
    break
  //
  switch (err) {
    LIBNDT_PERROR(none);
    LIBNDT_PERROR(broken_pipe);
    LIBNDT_PERROR(connection_aborted);
    LIBNDT_PERROR(connection_refused);
    LIBNDT_PERROR(connection_reset);
    LIBNDT_PERROR(function_not_supported);
    LIBNDT_PERROR(host_unreachable);
    LIBNDT_PERROR(interrupted);
    LIBNDT_PERROR(invalid_argument);
    LIBNDT_PERROR(io_error);
    LIBNDT_PERROR(message_size);
    LIBNDT_PERROR(network_down);
    LIBNDT_PERROR(network_reset);
    LIBNDT_PERROR(network_unreachable);
    LIBNDT_PERROR(operation_in_progress);
    LIBNDT_PERROR(operation_would_block);
    LIBNDT_PERROR(timed_out);
    LIBNDT_PERROR(value_too_large);
    LIBNDT_PERROR(eof);
    LIBNDT_PERROR(ai_generic);
    LIBNDT_PERROR(ai_again);
    LIBNDT_PERROR(ai_fail);
    LIBNDT_PERROR(ai_noname);
    LIBNDT_PERROR(socks5h);
    LIBNDT_PERROR(ssl_generic);
    LIBNDT_PERROR(ssl_want_read);
    LIBNDT_PERROR(ssl_want_write);
    LIBNDT_PERROR(ssl_syscall);
    LIBNDT_PERROR(ws_proto);
  }
#undef LIBNDT_PERROR  // Tidy
  //
  if (err == Err::ssl_generic) {
    rv += ": ";
    rv += ssl_format_error();
  }
  //
  return rv;
}

// Generic macro for emitting logs.
#define LIBNDT_EMIT_LOG_EX(client, level, statements)      \
  do {                                                     \
    if (client->get_verbosity() >= verbosity_##level) {    \
      std::stringstream ss_log_lines;                      \
      ss_log_lines << statements;                          \
      std::string log_line;                                \
      while (std::getline(ss_log_lines, log_line, '\n')) { \
        if (!log_line.empty()) {                           \
          client->on_##level(std::move(log_line));         \
        }                                                  \
      }                                                    \
    }                                                      \
  } while (0)

#define LIBNDT_EMIT_WARNING_EX(clnt, stmnts) LIBNDT_EMIT_LOG_EX(clnt, warning, stmnts)
#define LIBNDT_EMIT_INFO_EX(clnt, stmnts) LIBNDT_EMIT_LOG_EX(clnt, info, stmnts)
#define LIBNDT_EMIT_DEBUG_EX(clnt, stmnts) LIBNDT_EMIT_LOG_EX(clnt, debug, stmnts)

#define LIBNDT_EMIT_WARNING(statements) LIBNDT_EMIT_WARNING_EX(this, statements)
#define LIBNDT_EMIT_INFO(statements) LIBNDT_EMIT_INFO_EX(this, statements)
#define LIBNDT_EMIT_DEBUG(statements) LIBNDT_EMIT_DEBUG_EX(this, statements)

#ifdef _WIN32
#define LIBNDT_OS_SHUT_RDWR SD_BOTH
#else
#define LIBNDT_OS_SHUT_RDWR SHUT_RDWR
#endif

static void random_printable_fill(char *buffer, size_t length) noexcept {
  static const std::string ascii =
      " !\"#$%&\'()*+,-./"          // before numbers
      "0123456789"                  // numbers
      ":;<=>?@"                     // after numbers
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"  // uppercase
      "[\\]^_`"                     // between upper and lower
      "abcdefghijklmnopqrstuvwxyz"  // lowercase
      "{|}~"                        // final
      ;
  // TODO(bassosimone): the random device is not actually random in a
  // mingw environment. Here we should perhaps take advantage of the
  // OpenSSL dependency, when available, and use OpenSSL.
  std::random_device rd;
  std::mt19937 g(rd());
  for (size_t i = 0; i < length; ++i) {
    buffer[i] = ascii[g() % ascii.size()];
  }
}

static double compute_speed_kbits(double data, double elapsed) noexcept {
  return (elapsed > 0.0) ? ((data * 8.0) / 1000.0 / elapsed) : 0.0;
}

// format_speed_from_kbits format the input speed, which must be in kbit/s, to
// a string describing the speed with a measurement unit.
static std::string format_speed_from_kbits(double speed) noexcept {
  std::string unit = "kbit/s";
  if (speed > 1000) {
    unit = "Mbit/s";
    speed /= 1000;
    if (speed > 1000) {
      unit = "Gbit/s";
      speed /= 1000;
    }
  }
  std::stringstream ss;
  ss << std::setprecision(3) << std::setw(6) << std::right
      << speed << " " << unit;
  return ss.str();
}

static std::string format_speed_from_kbits(double data, double elapsed) noexcept {
  return format_speed_from_kbits(compute_speed_kbits(data, elapsed));
}

static std::string represent(std::string message) noexcept {
  bool printable = true;
  for (auto &c : message) {
    if (c < ' ' || c > '~') {
      printable = false;
      break;
    }
  }
  if (printable) {
    return message;
  }
  std::stringstream ss;
  ss << "binary([";
  for (auto &c : message) {
    if (c <= ' ' || c > '~') {
      ss << "<0x" << std::fixed << std::setw(2) << std::setfill('0') << std::hex
         << (unsigned)(uint8_t)c << ">";
    } else {
      ss << (char)c;
    }
  }
  ss << "])";
  return ss.str();
}

static std::string trim(std::string s) noexcept {
  auto pos = s.find_first_not_of(" \t");
  if (pos != std::string::npos) {
    s = s.substr(pos);
  }
  pos = s.find_last_not_of(" \t");
  if (pos != std::string::npos) {
    s = s.substr(0, pos + 1);
  }
  return s;
}

static bool emit_result(Client *client, std::string scope,
                        std::string message) noexcept {
  std::stringstream ss_line{message};
  std::string line;
  while ((std::getline(ss_line, line, '\n'))) {
    std::vector<std::string> keyval;
    std::string token;
    std::stringstream ss_token{line};
    while ((std::getline(ss_token, token, ':'))) {
      keyval.push_back(token);
    }
    if (keyval.size() != 2) {
      LIBNDT_EMIT_WARNING_EX(client, "incorrectly formatted summary message: " << message);
      return false;
    }
    client->on_result(scope, trim(std::move(keyval[0])),
                      trim(std::move(keyval[1])));
  }
  return true;
}

// Private classes
// ```````````````

#ifdef _WIN32
// "There must be a call to WSACleanup for each successful call
//  to WSAStartup. Only the final WSACleanup function call performs
//  the actual cleanup. The preceding calls simply decrement
//  an internal reference count in the WS2_32.DLL."

Client::Winsock::Winsock() noexcept {
  WORD requested = MAKEWORD(2, 2);
  WSADATA data;
  if (::WSAStartup(requested, &data) != 0) {
    abort();
  }
}

Client::Winsock::~Winsock() noexcept {
  if (::WSACleanup() != 0) {
    abort();
  }
}
#endif  // _WIN32

class SocketVector {
 public:
  SocketVector(Client *c) noexcept;
  ~SocketVector() noexcept;
  Client *owner = nullptr;
  std::vector<Socket> sockets;
};

SocketVector::SocketVector(Client *c) noexcept : owner{c} {}

SocketVector::~SocketVector() noexcept {
  if (owner != nullptr) {
    for (auto &fd : sockets) {
      owner->netx_closesocket(fd);
    }
  }
}

// Client constructor and destructor
// `````````````````````````````````

Client::Client() noexcept {}

Client::Client(Settings settings) noexcept : Client::Client() {
  std::swap(settings_, settings);
}

Client::~Client() noexcept {
  if (sock_ != -1) {
    netx_closesocket(sock_);
  }
}

// Top-level API
// `````````````

bool Client::run() noexcept {
  std::vector<std::string> fqdns;
  if (!query_mlabns(&fqdns)) {
    return false;
  }
  for (auto &fqdn : fqdns) {
    LIBNDT_EMIT_INFO("trying to connect to " << fqdn);
    settings_.hostname = fqdn;
    // TODO(bassosimone): we will eventually want to refactor the code to
    // make ndt7 the default and ndt5 the optional case.
    if ((settings_.protocol_flags & protocol_flag_ndt7) != 0) {
      LIBNDT_EMIT_INFO("using the ndt7 protocol");
      if ((settings_.nettest_flags & nettest_flag_download) != 0) {
        // TODO(bassosimone): for now we do not try with more than one host
        // when using ndt7 and there's a failure. We may want to do that.
        if (!ndt7_download()) {
          LIBNDT_EMIT_WARNING("ndt7: download failed");
          // FALLTHROUGH
        }
      }
      if ((settings_.nettest_flags & nettest_flag_upload) != 0) {
        // TODO(bassosimone): same as above.
        if (!ndt7_upload()) {
          LIBNDT_EMIT_WARNING("ndt7: upload failed");
          // FALLTHROUGH
        }
      }
      LIBNDT_EMIT_INFO("ndt7: test complete");
      // TODO(bassosimone): here we may want to warn if the user selects
      // subtests that we actually do not implement.
      return true;
    }
    if (!connect()) {
      LIBNDT_EMIT_WARNING("cannot connect to remote host; trying another one");
      continue;
    }
    LIBNDT_EMIT_INFO("connected to remote host");
    if (!send_login()) {
      LIBNDT_EMIT_WARNING("cannot send login; trying another host");
      continue;
    }
    LIBNDT_EMIT_INFO("sent login message");
    if (!recv_kickoff()) {
      LIBNDT_EMIT_WARNING("failed to receive kickoff; trying another host");
      continue;
    }
    if (!wait_in_queue()) {
      LIBNDT_EMIT_WARNING("failed to wait in queue; trying another host");
      continue;
    }
    LIBNDT_EMIT_INFO("authorized to run test");
    // From this point on we fail the test in case of error rather than
    // trying with another host. The rationale of trying with another host
    // above is that sometimes NDT servers are busy and we would like to
    // use another one rather than creating queue at the busy one.
    if (!recv_version()) {
      return false;
    }
    LIBNDT_EMIT_INFO("received server version");
    if (!recv_tests_ids()) {
      return false;
    }
    LIBNDT_EMIT_INFO("received tests ids");
    if (!run_tests()) {
      return false;
    }
    LIBNDT_EMIT_INFO("finished running tests; now reading summary data:");
    if (!recv_results_and_logout()) {
      return false;
    }
    LIBNDT_EMIT_INFO("received logout message");
    if (!wait_close()) {
      return false;
    }
    LIBNDT_EMIT_INFO("connection closed");
    return true;
  }
  LIBNDT_EMIT_WARNING("no more hosts to try; failing the test");
  return false;
}

void Client::on_warning(const std::string &msg) const {
  std::clog << "[!] " << msg << std::endl;
}

void Client::on_info(const std::string &msg) const {
  std::clog << msg << std::endl;
}

void Client::on_debug(const std::string &msg) const {
  std::clog << "[D] " << msg << std::endl;
}

void Client::on_performance(NettestFlags tid, uint8_t nflows,
                            double measured_bytes,
                            double elapsed_time, double max_runtime) {
  auto percent = 0.0;
  if (max_runtime > 0.0) {
    percent = (elapsed_time * 100.0 / max_runtime);
  }
  LIBNDT_EMIT_INFO("  [" << std::fixed << std::setprecision(0) << std::setw(2)
                  << std::right << percent << "%]"
                  << " elapsed: " << std::fixed << std::setprecision(3)
                  << std::setw(6) << elapsed_time << " s;"
                  << " test_id: " << (int)tid << "; num_flows: " << (int)nflows
                  << "; speed: "
                  << format_speed_from_kbits(measured_bytes, elapsed_time));
}

void Client::on_result(std::string scope, std::string name, std::string value) {
  LIBNDT_EMIT_INFO("  - [" << scope << "] " << name << ": " << value);
}

void Client::on_server_busy(std::string msg) {
  LIBNDT_EMIT_WARNING("server is busy: " << msg);
}

// High-level API
// ``````````````

bool Client::query_mlabns(std::vector<std::string> *fqdns) noexcept {
  assert(fqdns != nullptr);
  if (!settings_.hostname.empty()) {
    LIBNDT_EMIT_DEBUG("no need to query mlab-ns; we have hostname");
    // When we already know the hostname that we want to use just fake out the
    // result of a mlabns query as like mlabns returned that hostname.
    fqdns->push_back(std::move(settings_.hostname));
    return true;
  }
  std::string mlabns_url = settings_.mlabns_base_url;
  if ((settings_.nettest_flags & nettest_flag_download_ext) != 0) {
    LIBNDT_EMIT_WARNING("tweaking mlabns settings to allow for multi stream download");
    LIBNDT_EMIT_WARNING("we need to use the neubot sliver and to force json since");
    LIBNDT_EMIT_WARNING("this is the only configuration supported by neubot's sliver");
    settings_.protocol_flags &= ~protocol_flag_tls;
    settings_.protocol_flags &= ~protocol_flag_websocket;
    settings_.protocol_flags |= protocol_flag_json;
    mlabns_url += "/neubot";  // only botticelli implements multi stream dload
  } else {
    if ((settings_.protocol_flags & protocol_flag_ndt7) != 0) {
      mlabns_url += "/ndt7";
    } else if ((settings_.protocol_flags & protocol_flag_tls) != 0) {
      mlabns_url += "/ndt_ssl";
    } else {
      mlabns_url += "/ndt";
    }
  }
  if (settings_.mlabns_policy == mlabns_policy_random) {
    mlabns_url += "?policy=random";
  } else if (settings_.mlabns_policy == mlabns_policy_geo_options) {
    mlabns_url += "?policy=geo_options";
  }
  std::string body;
  if (!query_mlabns_curl(mlabns_url, settings_.timeout, &body)) {
    return false;
  }
  LIBNDT_EMIT_DEBUG("mlabns reply: " << body);
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception &exc) {
    LIBNDT_EMIT_WARNING("cannot parse JSON: " << exc.what());
    return false;
  }
  // In some cases mlab-ns returns a single object but in other cases (e.g.
  // with the `geo_options` policy) it returns an array. Always make an
  // array so that we can write uniform code for processing mlab-ns response.
  if (json.is_object()) {
    auto array = nlohmann::json::array();
    array.push_back(json);
    std::swap(json, array);
  }
  for (auto &entry : json) {
    std::string fqdn;
    try {
      fqdn = entry.at("fqdn").get<std::string>();
    } catch (const nlohmann::json::exception &exc) {
      LIBNDT_EMIT_WARNING("cannot access FQDN field: " << exc.what());
      return false;
    }
    LIBNDT_EMIT_INFO("discovered host: " << fqdn);
    fqdns->push_back(std::move(fqdn));
  }
  return true;
}

bool Client::connect() noexcept {
  std::string port;
  if (!settings_.port.empty()) {
    port = settings_.port;
  } else if ((settings_.protocol_flags & protocol_flag_tls) != 0) {
    port = "3010";
  } else {
    port = "3001";
  }
  // We may be called more than once when looping over the list returned by
  // geo_options. Therefore, the socket may already be open. In such case we
  // want to close it such that we don't leak resources.
  if (is_socket_valid(sock_)) {
    LIBNDT_EMIT_DEBUG("closing socket openned in previous attempt");
    (void)netx_closesocket(sock_);
    sock_ = (Socket)-1;
  }
  return netx_maybews_dial(  //
             settings_.hostname, port,
             ws_f_connection | ws_f_upgrade | ws_f_sec_ws_accept |
                 ws_f_sec_ws_protocol,
             ws_proto_control, "/ndt_protocol", &sock_) == Err::none;
}

bool Client::send_login() noexcept {
  return msg_write_login(ndt_version_compat);
}

bool Client::recv_kickoff() noexcept {
  if ((settings_.protocol_flags & protocol_flag_websocket) != 0) {
    LIBNDT_EMIT_INFO("no kickoff when using websocket");
    return true;
  }
  char buf[msg_kickoff_size];
  auto err = netx_recvn(sock_, buf, sizeof(buf));
  if (err != Err::none) {
    LIBNDT_EMIT_WARNING("recv_kickoff: netx_recvn() failed");
    return false;
  }
  if (memcmp(buf, msg_kickoff, sizeof(buf)) != 0) {
    LIBNDT_EMIT_WARNING("recv_kickoff: invalid kickoff message");
    return false;
  }
  LIBNDT_EMIT_INFO("received kickoff message");
  return true;
}

bool Client::wait_in_queue() noexcept {
  std::string message;
  if (!msg_expect(msg_srv_queue, &message)) {
    return false;
  }
  // There is consensus among NDT developers that modern NDT should not
  // wait in queue rather it should fail immediately.
  if (message != "0") {
    on_server_busy(std::move(message));
    return false;
  }
  return true;
}

bool Client::recv_version() noexcept {
  std::string message;
  if (!msg_expect(msg_login, &message)) {
    return false;
  }
  // TODO(bassosimone): validate version number?
  LIBNDT_EMIT_DEBUG("server version: " << message);
  return true;
}

bool Client::recv_tests_ids() noexcept {
  std::string message;
  if (!msg_expect(msg_login, &message)) {
    return false;
  }
  std::istringstream ss{message};
  std::string cur;
  while ((std::getline(ss, cur, ' '))) {
    const char *errstr = nullptr;
    static_assert(sizeof(NettestFlags) == sizeof(uint8_t),
                  "Invalid NettestFlags size");
    auto tid = (uint8_t)sys_strtonum(cur.data(), 1, 256, &errstr);
    if (errstr != nullptr) {
      LIBNDT_EMIT_WARNING("recv_tests_ids: found invalid test-id: "
                   << cur.data() << " (error: " << errstr << ")");
      return false;
    }
    granted_suite_.push_back(NettestFlags{tid});
  }
  return true;
}

bool Client::run_tests() noexcept {
  for (auto &tid : granted_suite_) {
    if (tid == nettest_flag_upload) {
      LIBNDT_EMIT_INFO("running upload test");
      if (!run_upload()) {
        return false;
      }
    } else if (tid == nettest_flag_meta) {
      LIBNDT_EMIT_DEBUG("running meta test");  // don't annoy the user with this
      if (!run_meta()) {
        return false;
      }
    } else if (tid == nettest_flag_download ||
               tid == nettest_flag_download_ext) {
      LIBNDT_EMIT_INFO("running download test");
      if (!run_download()) {
        return false;
      }
    } else {
      LIBNDT_EMIT_WARNING("run_tests(): unexpected test id");
      return false;
    }
  }
  return true;
}

bool Client::recv_results_and_logout() noexcept {
  for (auto i = 0; i < max_loops; ++i) {  // don't loop forever
    std::string message;
    MsgType code = MsgType{0};
    if (!msg_read(&code, &message)) {
      return false;
    }
    if (code != msg_results && code != msg_logout) {
      LIBNDT_EMIT_WARNING("recv_results_and_logout: unexpected message type");
      return false;
    }
    if (code == msg_logout) {
      return true;
    }
    if (!emit_result(this, "summary", std::move(message))) {
      // NOTHING: apparently ndt-cloud returns a free text message in this
      // case and the warning has already been printed by emit_result(). We
      // used to fail here but probably it's more robust just to warn.
    }
  }
  LIBNDT_EMIT_WARNING("recv_results_and_logout: too many msg_results messages");
  return false;  // Too many loops
}

bool Client::wait_close() noexcept {
  // So, the NDT protocol specification just says: "At the end the Server MUST
  // close the whole test session by sending an empty MSG_LOGOUT message and
  // closing connection with the Client." The following code gives the server
  // one second to close the connection, using netx_wait_readable(). Once that
  // function returns, we unconditionally close the socket. This is simpler
  // than a previous implementation in that we do not care much about the state
  // of the socket after netx_wait_readable() returns. I don't think here
  // we've any "dirty shutdown" concerns, because the NDT protocol includes a
  // MSG_LOGOUT sent from the server, hence we know we reached the final state.
  //
  // Note: after reading RFC6455, I realized why the server SHOULD close the
  // connection rather than the client: so that the TIME_WAIT state is entered
  // by the server, such that there is little server side impact.
  constexpr Timeout wait_for_close = 3;
  (void)netx_wait_readable(sock_, wait_for_close);
  (void)netx_closesocket(sock_);
  return true;
}

// Mid-level API
// `````````````

bool Client::run_download() noexcept {
  SocketVector dload_socks{this};
  std::string port;
  uint8_t nflows = 1;
  if (!msg_expect_test_prepare(&port, &nflows)) {
    return false;
  }

  for (uint8_t i = 0; i < nflows; ++i) {
    Socket sock = (Socket)-1;
    // Implementation note: here connection attempts are serialized. This is
    // consistent with <https://tools.ietf.org/html/rfc6455#section-4.1>, and
    // namely with requirement 2: "If multiple connections to the same IP
    // address are attempted simultaneously, the client MUST serialize them".
    Err err = netx_maybews_dial(  //
        settings_.hostname, port,
        ws_f_connection | ws_f_upgrade | ws_f_sec_ws_accept
          | ws_f_sec_ws_protocol, ws_proto_s2c, "/ndt_protocol",
        &sock);
    if (err != Err::none) {
      break;
    }
    dload_socks.sockets.push_back(sock);
  }
  if (dload_socks.sockets.size() != nflows) {
    LIBNDT_EMIT_WARNING("run_download: not all connect succeeded");
    return false;
  }

  if (!msg_expect_empty(msg_test_start)) {
    return false;
  }
  LIBNDT_EMIT_DEBUG("run_download: got the test_start message");

  double client_side_speed = 0.0;
  {
    std::atomic<uint8_t> active{0};
    auto begin = std::chrono::steady_clock::now();
    std::atomic<uint64_t> total_data{0};
    auto max_runtime = settings_.max_runtime;
    auto ws = (settings_.protocol_flags & protocol_flag_websocket) != 0;
    const Client *const_this = this;
    for (Socket fd : dload_socks.sockets) {
      // TODO(bassosimone): increment active inside the thread main function
      // as this is more consistent with the fact that thread decrements it
      active += 1;  // atomic
      auto main = [
        &active,       // reference to atomic
        begin,         // copy for safety
        fd,            // copy for safety
        max_runtime,   // copy for safety
        const_this,    // const pointer
        &total_data,   // reference to atomic
        ws             // copy for safety
      ]() noexcept {
        constexpr size_t ndt_bufsize = 131072;
        std::unique_ptr<char[]> buf(new char[ndt_bufsize]);
        for (;;) {
          auto err = Err::none;
          Size n = 0;
          if (ws) {
            uint8_t op = 0;
            err = const_this->ws_recvmsg(
                    fd, &op, (uint8_t *)buf.get(), ndt_bufsize, &n);
            if (err == Err::none && op != ws_opcode_binary) {
              LIBNDT_EMIT_WARNING_EX(const_this,
                "run_download: unexpected opcode: " << (unsigned int)op);
              break;
            }
          } else {
            err = const_this->netx_recv(fd, buf.get(), ndt_bufsize, &n);
          }
          if (err != Err::none) {
            if (err != Err::eof) {
              LIBNDT_EMIT_WARNING_EX(const_this,
                "run_download: receiving: " << libndt_perror(err));
            }
            break;
          }
          total_data += (uint64_t)n;   // atomic
          auto now = std::chrono::steady_clock::now();
          std::chrono::duration<double> elapsed = now - begin;
          if (elapsed.count() > max_runtime) {
            break;
          }
        }
        active -= 1;  // atomic
      };
      std::thread thread{std::move(main)};
      thread.detach();
    }
    auto prev = begin;
    for (;;) {
      constexpr int timeout_msec = 250;
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_msec));
      if (active <= 0) {
        break;
      }
      auto now = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed = now - begin;
      on_performance(nettest_flag_download,             //
                     active,                            // atomic
                     static_cast<double>(total_data),   // atomic
                     elapsed.count(),                   //
                     settings_.max_runtime);
      prev = now;
    }
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - begin;
    client_side_speed = compute_speed_kbits(  //
        static_cast<double>(total_data), elapsed.count());
  }

  {
    // TODO(bassosimone): emit this information.
    MsgType code = MsgType{0};
    std::string message;
    if (!msg_read_legacy(&code, &message)) {  // legacy on purpose!
      return false;
    }
    if (code != msg_test_msg) {
      LIBNDT_EMIT_WARNING("run_download: unexpected message type");
      return false;
    }
    LIBNDT_EMIT_DEBUG("run_download: server computed speed: " << message);
  }

  if (!msg_write(msg_test_msg, std::to_string(client_side_speed))) {
    return false;
  }

  LIBNDT_EMIT_INFO("reading summary web100 variables");
  for (auto i = 0; i < max_loops; ++i) {  // don't loop forever
    std::string message;
    MsgType code = MsgType{0};
    if (!msg_read(&code, &message)) {
      return false;
    }
    if (code != msg_test_msg && code != msg_test_finalize) {
      LIBNDT_EMIT_WARNING("run_download: unexpected message type");
      return false;
    }
    if (code == msg_test_finalize) {
      return true;
    }
    if (!emit_result(this, "web100", std::move(message))) {
      // NOTHING: warning already printed by emit_result() and failing the whole
      // test - rather than warning - because of an incorrect data format is
      // probably being too strict in this context. So just keep going.
    }
  }

  LIBNDT_EMIT_WARNING("run_download: too many msg_test_msg messages");
  return false;  // Too many loops
}

bool Client::run_meta() noexcept {
  if (!msg_expect_empty(msg_test_prepare)) {
    return false;
  }
  if (!msg_expect_empty(msg_test_start)) {
    return false;
  }

  for (auto &kv : settings_.metadata) {
    std::stringstream ss;
    ss << kv.first << ":" << kv.second;
    if (!msg_write(msg_test_msg, ss.str())) {
      return false;
    }
  }
  if (!msg_write(msg_test_msg, "")) {
    return false;
  }

  if (!msg_expect_empty(msg_test_finalize)) {
    return false;
  }

  return true;
}

bool Client::run_upload() noexcept {
  SocketVector upload_socks{this};

  std::string port;
  uint8_t nflows = 1;
  if (!msg_expect_test_prepare(&port, &nflows)) {
    return false;
  }
  // TODO(bassosimone): implement C2S_EXT
  if (nflows != 1) {
    LIBNDT_EMIT_WARNING("run_upload: unexpected number of flows");
    return false;
  }

  {
    Socket sock = (Socket)-1;
    // Remark: in case we'll ever implement multi-stream here, remember that
    // WebSocket requires connections to be serialized. See above.
    Err err = netx_maybews_dial(  //
        settings_.hostname, port,
        ws_f_connection | ws_f_upgrade | ws_f_sec_ws_accept
          | ws_f_sec_ws_protocol, ws_proto_c2s, "/ndt_protocol",
        &sock);
    if (err != Err::none) {
      return false;
    }
    upload_socks.sockets.push_back(sock);
  }

  if (!msg_expect_empty(msg_test_start)) {
    return false;
  }

  double client_side_speed = 0.0;
  {
    std::atomic<uint8_t> active{0};
    auto begin = std::chrono::steady_clock::now();
    std::atomic<uint64_t> total_data{0};
    auto max_runtime = settings_.max_runtime;
    auto ws = (settings_.protocol_flags & protocol_flag_websocket) != 0;
    const Client *const_this = this;
    for (Socket fd : upload_socks.sockets) {
      // TODO(bassosimone): increment active inside the thread main function
      // as this is more consistent with the fact that thread decrements it
      active += 1;  // atomic
      auto main = [
        &active,       // reference to atomic
        begin,         // copy for safety
        fd,            // copy for safety
        max_runtime,   // copy for safety
        const_this,    // const pointer
        &total_data,   // reference to atomic
        ws             // copy for safety
      ]() noexcept {
        constexpr size_t ndt_bufsize = 131072;
        std::unique_ptr<char[]> buf(new char[ndt_bufsize]);
        {
          auto start = std::chrono::steady_clock::now();
          random_printable_fill(buf.get(), ndt_bufsize);
          auto now = std::chrono::steady_clock::now();
          std::chrono::duration<double> elapsed = now - start;
          LIBNDT_EMIT_DEBUG_EX(const_this,
            "run_upload: time to fill random buffer: " << elapsed.count());
        }
        std::string frame = const_this->ws_prepare_frame(
            ws_opcode_binary | ws_fin_flag, (uint8_t *)buf.get(), ndt_bufsize);
        for (;;) {
          Size n = 0;
          auto err = Err::none;
          if (ws) {
            err = const_this->netx_sendn(fd, frame.data(), frame.size());
            if (err == Err::none) {
              n = frame.size();
            }
          } else {
            err = const_this->netx_send(fd, buf.get(), ndt_bufsize, &n);
          }
          if (err != Err::none) {
            if (err != Err::broken_pipe) {
              LIBNDT_EMIT_WARNING_EX(const_this,
                "run_upload: sending: " << libndt_perror(err));
            }
            break;
          }
          total_data += (uint64_t)n;   // atomic
          auto now = std::chrono::steady_clock::now();
          std::chrono::duration<double> elapsed = now - begin;
          if (elapsed.count() > max_runtime) {
            break;
          }
        }
        active -= 1;  // atomic
      };
      std::thread thread{std::move(main)};
      thread.detach();
    }
    auto prev = begin;
    for (;;) {
      constexpr int timeout_msec = 250;
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_msec));
      if (active <= 0) {
        break;
      }
      auto now = std::chrono::steady_clock::now();
      std::chrono::duration<double> elapsed = now - begin;
      on_performance(nettest_flag_upload,               //
                     active,                            // atomic
                     static_cast<double>(total_data),   // atomic
                     elapsed.count(),                   //
                     settings_.max_runtime);
      prev = now;
    }
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - begin;
    client_side_speed = compute_speed_kbits(  //
        static_cast<double>(total_data), elapsed.count());
    LIBNDT_EMIT_DEBUG("run_upload: client computed speed: " << client_side_speed);
  }

  {
    std::string message;
    if (!msg_expect(msg_test_msg, &message)) {
      return false;
    }
    // TODO(bassosimone): emit this information
    LIBNDT_EMIT_DEBUG("run_upload: server computed speed: " << message);
  }

  if (!msg_expect_empty(msg_test_finalize)) {
    return false;
  }

  return true;
}

// ndt7 protocol API
// `````````````````

bool Client::ndt7_download() noexcept {
  if (!ndt7_connect("/ndt/v7/download")) {
    return false;
  }
  // The following value is the maximum amount of bytes that an implementation
  // SHOULD be prepared to handle when receiving ndt7 messages.
  constexpr Size ndt7_bufsiz = (1 << 17);
  std::unique_ptr<uint8_t[]> buff{new uint8_t[ndt7_bufsiz]};
  auto begin = std::chrono::steady_clock::now();
  auto latest = begin;
  Size total = 0;
  for (;;) {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - begin;
    if (elapsed.count() > settings_.max_runtime) {
      LIBNDT_EMIT_WARNING("ndt7: download running for too much time");
      return false;
    }
    constexpr auto measurement_interval = 0.25;
    std::chrono::duration<double> interval = now - latest;
    if (interval.count() > measurement_interval) {
      on_performance(nettest_flag_download, 1, static_cast<double>(total),
                     elapsed.count(), settings_.max_runtime);
      latest = now;
    }
    uint8_t opcode = 0;
    Size count = 0;
    Err err = ws_recvmsg(sock_, &opcode, buff.get(), ndt7_bufsiz, &count);
    if (err != Err::none) {
      if (err == Err::eof) {
        break;
      }
      return false;
    }
    if (opcode == ws_opcode_text) {
      // The following is an issue both on armv7 and on Windows 32 bit: the
      // definition of size we have chose is such that later conversion to
      // string is problematic because our size is 64 bit while size_t is 32
      // bit on the platfrom. That said, it's unlikely that the we'll get a
      // measurement that big, so the check to make sure the casting is okay
      // is not going to be a real problem, it's just a theoric issue.
      if (count <= SIZE_MAX) {
        std::string sinfo{(const char *)buff.get(), (size_t)count};
        on_result("ndt7", "download", std::move(sinfo));
      }
    }
    total += count;  // Assume we won't overflow
  }
  return true;
}

bool Client::ndt7_upload() noexcept {
  if (!ndt7_connect("/ndt/v7/upload")) {
    return false;
  }
  // Implementation note: we send messages smaller than the maximum message
  // size accepted by the protocol. We have chosen this value because it
  // currently seems to be a reasonable size for outgoing messages.
  constexpr Size ndt7_bufsiz = (1 << 13);
  std::unique_ptr<uint8_t[]> buff{new uint8_t[ndt7_bufsiz]};
  random_printable_fill((char *)buff.get(), ndt7_bufsiz);
  // The following is the expected ndt7 transfer time for a subtest.
  constexpr double max_upload_time = 10.0;
  auto begin = std::chrono::steady_clock::now();
  auto latest = begin;
  Size total = 0;
  std::string frame = ws_prepare_frame(ws_opcode_binary | ws_fin_flag,
                                       buff.get(), ndt7_bufsiz);
  for (;;) {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - begin;
    std::chrono::duration<double, std::micro> elapsed_ms =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    if (elapsed.count() > max_upload_time) {
      LIBNDT_EMIT_DEBUG("ndt7: upload has run for enough time");
      break;
    }
    constexpr auto measurement_interval = 0.25;
    std::chrono::duration<double> interval = now - latest;
    if (interval.count() > measurement_interval) {
      nlohmann::json measurement;
      measurement["AppInfo"] = nlohmann::json();
      measurement["AppInfo"]["ElapsedTime"] = (unsigned int) elapsed_ms.count();
      measurement["AppInfo"]["NumBytes"] = total;
#ifdef __linux__
      // Read tcp_info data for the socket and print it as JSON.
      struct tcp_info tcpinfo{};
      socklen_t tcpinfolen = sizeof(tcpinfo);
      if (sys_getsockopt(sock_, IPPROTO_TCP, TCP_INFO, (void *)&tcpinfo,
                         &tcpinfolen) == 0) {
        measurement["TCPInfo"] = nlohmann::json();
        measurement["TCPInfo"]["ElapsedTime"] = (unsigned int) elapsed_ms.count();
#define XX(lower_, upper_) measurement["TCPInfo"][#upper_] = (uint64_t)tcpinfo.lower_;
        NDT7_ENUM_TCP_INFO
#undef XX
      }
#endif  // __linux__
      on_performance(nettest_flag_upload, 1, static_cast<double>(total),
                     elapsed.count(), max_upload_time);
      // This could fail if there are non-utf8 characters. This structure just
      // contains integers and ASCII strings.
      std::string json = measurement.dump();
      on_result("ndt7", "upload", json);

      // Send measurement to the server.
      Err err = ws_send_frame(sock_, ws_opcode_text | ws_fin_flag,
                              (uint8_t *)json.data(), json.size());
      if (err != Err::none) {
        LIBNDT_EMIT_WARNING("ndt7: cannot send measurement");
        return false;
      }

      latest = now;
    }
    Err err = netx_sendn(sock_, frame.data(), frame.size());
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("ndt7: cannot send frame");
      return false;
    }
    total += ndt7_bufsiz;  // Assume we won't overflow
  }
  return true;
}

bool Client::ndt7_connect(std::string url_path) noexcept {
  std::string port = "443";
  if (!settings_.port.empty()) {
    port = settings_.port;
  }
  // Don't leak resources if the socket is already open.
  if (is_socket_valid(sock_)) {
    LIBNDT_EMIT_DEBUG("ndt7: closing socket openned in previous attempt");
    (void)netx_closesocket(sock_);
    sock_ = (Socket)-1;
  }
  // Note: ndt7 implies WebSocket and TLS
  settings_.protocol_flags |= protocol_flag_websocket | protocol_flag_tls;
  Err err = netx_maybews_dial(
      settings_.hostname, port,
      ws_f_connection | ws_f_upgrade | ws_f_sec_ws_accept |
          ws_f_sec_ws_protocol,
      ws_proto_ndt7, url_path, &sock_);
  if (err != Err::none) {
    return false;
  }
  LIBNDT_EMIT_INFO("ndt7: WebSocket connection established");
  return true;
}

// NDT protocol API
// ````````````````

bool Client::msg_write_login(const std::string &version) noexcept {
  static_assert(sizeof(settings_.nettest_flags) == 1,
                "nettest_flags too large");
  MsgType code = MsgType{0};
  settings_.nettest_flags |= nettest_flag_status | nettest_flag_meta;
  // Implementation note: judging from a GCC 8 warning, it seems that bitwise negation
  // leads to a promotion to `int` (not even `unsigned int`) on Linux. So, after that
  // we need first to ensure any bit except from 0xff is zero. After that, we can then
  // reduce again the size to NettestFlags (aka uint8_t; see above) to do the &=.
  if ((settings_.nettest_flags & nettest_flag_middlebox) != NettestFlags{0}) {
    LIBNDT_EMIT_WARNING("msg_write_login: nettest_flag_middlebox: not implemented");
    settings_.nettest_flags &= (NettestFlags)((~nettest_flag_middlebox) & 0xff);
  }
  if ((settings_.nettest_flags & nettest_flag_simple_firewall) != NettestFlags{0}) {
    LIBNDT_EMIT_WARNING(
        "msg_write_login: nettest_flag_simple_firewall: not implemented");
    settings_.nettest_flags &= (NettestFlags)((~nettest_flag_simple_firewall) & 0xff);
  }
  if ((settings_.nettest_flags & nettest_flag_upload_ext) != NettestFlags{0}) {
    LIBNDT_EMIT_WARNING("msg_write_login: nettest_flag_upload_ext: not implemented");
    settings_.nettest_flags &= (NettestFlags)((~nettest_flag_upload_ext) & 0xff);
  }
  std::string serio;
  if ((settings_.protocol_flags & protocol_flag_json) == 0) {
    serio = std::string{(char *)&settings_.nettest_flags,
                        sizeof(settings_.nettest_flags)};
    code = msg_login;
  } else {
    code = msg_extended_login;
    nlohmann::json msg{
        {"msg", version},
        {"tests", std::to_string((unsigned)settings_.nettest_flags)},
    };
    try {
      serio = msg.dump();
    } catch (nlohmann::json::exception &) {
      LIBNDT_EMIT_WARNING("msg_write_login: cannot serialize JSON");
      return false;
    }
  }
  assert(code != MsgType{0});
  if (!msg_write_legacy(code, std::move(serio))) {
    return false;
  }
  return true;
}

bool Client::msg_write(MsgType code, std::string &&msg) noexcept {
  LIBNDT_EMIT_DEBUG("msg_write: message to send: " << represent(msg));
  if ((settings_.protocol_flags & protocol_flag_json) != 0) {
    nlohmann::json json;
    json["msg"] = msg;
    try {
      msg = json.dump();
    } catch (const nlohmann::json::exception &) {
      LIBNDT_EMIT_WARNING("msg_write: cannot serialize JSON");
      return false;
    }
  }
  if (!msg_write_legacy(code, std::move(msg))) {
    return false;
  }
  return true;
}

bool Client::msg_write_legacy(MsgType code, std::string &&msg) noexcept {
  {
    LIBNDT_EMIT_DEBUG("msg_write_legacy: raw message: " << represent(msg));
    LIBNDT_EMIT_DEBUG("msg_write_legacy: message length: " << msg.size());
    char header[3];
    header[0] = (char)code;  // Sign change safe because we're serializing
    if (msg.size() > UINT16_MAX) {
      LIBNDT_EMIT_WARNING("msg_write_legacy: message too long");
      return false;
    }
    uint16_t len = (uint16_t)msg.size();
    len = htons(len);
    memcpy(&header[1], &len, sizeof(len));
    LIBNDT_EMIT_DEBUG("msg_write_legacy: header[0] (type): " << (int)header[0]);
    LIBNDT_EMIT_DEBUG("msg_write_legacy: header[1] (len-high): " << (int)header[1]);
    LIBNDT_EMIT_DEBUG("msg_write_legacy: header[2] (len-low): " << (int)header[2]);
    {
      auto err = Err::none;
      if ((settings_.protocol_flags & protocol_flag_websocket) != 0) {
        err = ws_send_frame(
            sock_,
            ws_opcode_binary | ((msg.size() <= 0) ? ws_fin_flag : 0),
            (uint8_t *)header, sizeof(header));
      } else {
        err = netx_sendn(sock_, header, sizeof(header));
      }
      if (err != Err::none) {
        LIBNDT_EMIT_WARNING("msg_write_legacy: cannot send NDT message header");
        return false;
      }
    }
    LIBNDT_EMIT_DEBUG("msg_write_legacy: sent message header");
  }
  if (msg.size() <= 0) {
    LIBNDT_EMIT_DEBUG("msg_write_legacy: zero length message");
    return true;
  }
  {
    auto err = Err::none;
    if ((settings_.protocol_flags & protocol_flag_websocket) != 0) {
      err = ws_send_frame(sock_, ws_opcode_continue | ws_fin_flag,
                          (uint8_t *)msg.data(), msg.size());
    } else {
      err = netx_sendn(sock_, msg.data(), msg.size());
    }
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("msg_write_legacy: cannot send NDT message body");
      return false;
    }
  }
  LIBNDT_EMIT_DEBUG("msg_write_legacy: sent message body");
  return true;
}

bool Client::msg_expect_test_prepare(std::string *pport,
                                     uint8_t *pnflows) noexcept {
  // Both download and upload tests send the same options vector containing
  // the port (non-extended case) and other parameters (otherwise). Currently
  // we only honour the port and the number of flows parameters.

  std::vector<std::string> options;
  {
    std::string message;
    if (!msg_expect(msg_test_prepare, &message)) {
      return false;
    }
    std::istringstream ss{message};
    std::string cur;
    while ((std::getline(ss, cur, ' '))) {
      options.push_back(cur);
    }
  }
  if (options.size() < 1) {
    LIBNDT_EMIT_WARNING("msg_expect_test_prepare: not enough options in vector");
    return false;
  }

  std::string port;
  {
    const char *error = nullptr;
    (void)sys_strtonum(options[0].data(), 1, UINT16_MAX, &error);
    if (error != nullptr) {
      LIBNDT_EMIT_WARNING("msg_expect_test_prepare: cannot parse port");
      return false;
    }
    port = options[0];
  }

  // Here we are being liberal; in theory we should only accept the
  // extra parameters when the test is extended.
  //
  // Also, we do not parse fields that we don't use.

  uint8_t nflows = 1;
  if (options.size() >= 6) {
    const char *error = nullptr;
    nflows = (uint8_t)sys_strtonum(options[5].c_str(), 1, 16, &error);
    if (error != nullptr) {
      LIBNDT_EMIT_WARNING("msg_expect_test_prepare: cannot parse num-flows");
      return false;
    }
  }

  *pport = port;
  *pnflows = nflows;
  return true;
}

bool Client::msg_expect_empty(MsgType expected_code) noexcept {
  std::string s;
  if (!msg_expect(expected_code, &s)) {
    return false;
  }
  if (s != "") {
    LIBNDT_EMIT_WARNING("msg_expect_empty: non-empty body");
    return false;
  }
  return true;
}

bool Client::msg_expect(MsgType expected_code, std::string *s) noexcept {
  assert(s != nullptr);
  MsgType code = MsgType{0};
  if (!msg_read(&code, s)) {
    return false;
  }
  if (code != expected_code) {
    LIBNDT_EMIT_WARNING("msg_expect: unexpected message type");
    return false;
  }
  return true;
}

bool Client::msg_read(MsgType *code, std::string *msg) noexcept {
  assert(code != nullptr && msg != nullptr);
  std::string s;
  if (!msg_read_legacy(code, &s)) {
    return false;
  }
  if ((settings_.protocol_flags & protocol_flag_json) == 0) {
    std::swap(s, *msg);
  } else {
    nlohmann::json json;
    try {
      json = nlohmann::json::parse(s);
    } catch (const nlohmann::json::exception &) {
      LIBNDT_EMIT_WARNING("msg_read: cannot parse JSON");
      return false;
    }
    try {
      *msg = json.at("msg").get<std::string>();
    } catch (const nlohmann::json::exception &) {
      LIBNDT_EMIT_WARNING("msg_read: cannot find 'msg' field");
      return false;
    }
  }
  LIBNDT_EMIT_DEBUG("msg_read: message: " << represent(*msg));
  return true;
}

bool Client::msg_read_legacy(MsgType *code, std::string *msg) noexcept {
  assert(code != nullptr && msg != nullptr);
  constexpr Size header_size = 3;
  constexpr Size max_body_size = UINT16_MAX;
  constexpr Size max_msg_size = header_size + max_body_size;
  char buffer[max_msg_size];
  uint16_t len = 0;
  *msg = "";
  {
    Size ws_msg_len = 0;
    if ((settings_.protocol_flags & protocol_flag_websocket) != 0) {
      uint8_t opcode = 0;
      auto err = ws_recvmsg(  //
          sock_, &opcode, (uint8_t *)buffer, sizeof(buffer), &ws_msg_len);
      if (err != Err::none) {
        LIBNDT_EMIT_WARNING(
            "msg_read_legacy: cannot read NDT message using websocket");
        return false;
      }
      if (ws_msg_len < header_size) {
        LIBNDT_EMIT_WARNING("msg_read_legacy: message too short");
        return false;
      }
      if (opcode != ws_opcode_binary) {
        LIBNDT_EMIT_WARNING("msg_ready_legacy: unexpected opcode: "
                     << (unsigned int)opcode);
        return false;
      }
      assert(ws_msg_len <= sizeof(buffer));
    } else {
      static_assert(sizeof(buffer) >= header_size,
                    "Not enough room in buffer to read the NDT header");
      auto err = netx_recvn(sock_, buffer, header_size);
      if (err != Err::none) {
        LIBNDT_EMIT_WARNING("msg_read_legacy: cannot read NDT message header");
        return false;
      }
    }
    LIBNDT_EMIT_DEBUG("msg_read_legacy: header[0] (type): " << (int)buffer[0]);
    LIBNDT_EMIT_DEBUG("msg_read_legacy: header[1] (len-high): " << (int)buffer[1]);
    LIBNDT_EMIT_DEBUG("msg_read_legacy: header[2] (len-low): " << (int)buffer[2]);
    static_assert(sizeof(MsgType) == sizeof(unsigned char),
                  "Unexpected MsgType size");
    *code = MsgType{(unsigned char)buffer[0]};
    memcpy(&len, &buffer[1], sizeof(len));
    len = ntohs(len);
    if ((settings_.protocol_flags & protocol_flag_websocket) != 0) {
      assert(ws_msg_len >= header_size);  // Proper check above
      if (len != ws_msg_len - header_size) {
        LIBNDT_EMIT_WARNING("msg_read_legacy: got inconsistent websocket message");
        return false;
      }
    }
    LIBNDT_EMIT_DEBUG("msg_read_legacy: message length: " << len);
  }
  if (len <= 0) {
    LIBNDT_EMIT_DEBUG("msg_read_legacy: zero length message");
    return true;
  }
  if ((settings_.protocol_flags & protocol_flag_websocket) == 0) {
    assert(sizeof(buffer) >= header_size &&
           sizeof(buffer) - header_size >= len);
    auto err = netx_recvn(sock_, &buffer[header_size], len);
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("msg_read_legacy: cannot read NDT message body");
      return false;
    }
  }
  // This is a stringy copy but we do not care much because the part that needs
  // to be efficient is the one running measurements not the one where we deal
  // with incoming and outgoing NDT control messages.
  *msg = std::string{&buffer[header_size], len};
  LIBNDT_EMIT_DEBUG("msg_read_legacy: raw message: " << represent(*msg));
  return true;
}

// WebSocket
// `````````
// This section contains the websocket implementation. Although this has been
// written from scratch while reading the RFC, it has beem very useful to be
// able to see the websocket implementation in ndt-project/ndt, to have another
// clear, simple existing implementation to compare with.
//
// - - - BEGIN WEBSOCKET IMPLEMENTATION - - - {

Err Client::ws_sendln(Socket fd, std::string line) noexcept {
  LIBNDT_EMIT_DEBUG("> " << line);
  line += "\r\n";
  return netx_sendn(fd, line.c_str(), line.size());
}

Err Client::ws_recvln(Socket fd, std::string *line, size_t maxlen) noexcept {
  if (line == nullptr || maxlen <= 0) {
    return Err::invalid_argument;
  }
  line->reserve(maxlen);
  line->clear();
  while (line->size() < maxlen) {
    char ch = {};
    auto err = netx_recvn(fd, &ch, sizeof(ch));
    if (err != Err::none) {
      return err;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      LIBNDT_EMIT_DEBUG("< " << *line);
      return Err::none;
    }
    *line += ch;
  }
  LIBNDT_EMIT_WARNING("ws_recvln: line too long");
  return Err::value_too_large;
}

Err Client::ws_handshake(Socket fd, std::string port, uint64_t ws_flags,
                         std::string ws_proto, std::string url_path) noexcept {
  std::string proto_header;
  {
    proto_header += "Sec-WebSocket-Protocol: ";
    proto_header += ws_proto;
  }
  {
    // Implementation note: we use the default WebSocket key provided in the RFC
    // so that we don't need to depend on OpenSSL for websocket.
    //
    // TODO(bassosimone): replace this with a randomly selected value that
    // varies for each connection. Or we're not compliant.
    constexpr auto key_header = "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==";
    std::stringstream host_header;
    host_header << "Host: " << settings_.hostname;
    // Adding nonstandard port as specified in RFC6455 Sect. 4.1.
    if ((settings_.protocol_flags & protocol_flag_tls) != 0) {
      if (port != "443") {
        host_header << ":" << port;
      }
    } else {
      if (port != "80") {
        host_header << ":" << port;
      }
    }
    std::stringstream request_line;
    request_line << "GET " << url_path << " HTTP/1.1";
    Err err = Err::none;
    if ((err = ws_sendln(fd, request_line.str())) != Err::none ||
        (err = ws_sendln(fd, host_header.str())) != Err::none ||
        (err = ws_sendln(fd, "Upgrade: websocket")) != Err::none ||
        (err = ws_sendln(fd, "Connection: Upgrade")) != Err::none ||
        (err = ws_sendln(fd, key_header)) != Err::none ||
        (err = ws_sendln(fd, proto_header)) != Err::none ||
        (err = ws_sendln(fd, "Sec-WebSocket-Version: 13")) != Err::none ||
        (err = ws_sendln(fd, "")) != Err::none) {
      LIBNDT_EMIT_WARNING("ws_handshake: cannot send HTTP upgrade request");
      return err;
    }
  }
  LIBNDT_EMIT_DEBUG("ws_handshake: sent HTTP/1.1 upgrade request");
  //
  // Limitations of the response processing code
  // ```````````````````````````````````````````
  // Apart from the limitations explicitly identified with TODO messages, the
  // algorithm to process the response has the following limitations:
  //
  // 1. we do not follow redirects (but we're not required to)
  //
  // 2. we do not fail the connection if the Sec-WebSocket-Extensions header is
  //    part of the handshake response (it would mean that an extension we do
  //    not support is being enforced by the server)
  //
  {
    // TODO(bassosimone): use the same value used by ndt-project/ndt
    static constexpr size_t max_line_length = 8000;
    std::string line;
    auto err = ws_recvln(fd, &line, max_line_length);
    if (err != Err::none) {
      return err;
    }
    // TODO(bassosimone): ignore text after 101
    if (line != "HTTP/1.1 101 Switching Protocols") {
      LIBNDT_EMIT_WARNING("ws_handshake: unexpected response line");
      return Err::ws_proto;
    }
    uint64_t flags = 0;
    // TODO(bassosimone): use the same value used by ndt-project/ndt
    constexpr size_t max_headers = 1000;
    for (size_t i = 0; i < max_headers; ++i) {
      // TODO(bassosimone): make header processing case insensitive.
      auto recvln_err = ws_recvln(fd, &line, max_line_length);
      if (recvln_err != Err::none) {
        return recvln_err;
      }
      if (line == "Upgrade: websocket") {
        flags |= ws_f_upgrade;
      } else if (line == "Connection: Upgrade") {
        flags |= ws_f_connection;
      } else if (line == "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") {
        flags |= ws_f_sec_ws_accept;
      } else if (line == proto_header) {
        flags |= ws_f_sec_ws_protocol;
      } else if (line == "") {
        if ((flags & ws_flags) != ws_flags) {
          LIBNDT_EMIT_WARNING("ws_handshake: received incorrect handshake");
          return Err::ws_proto;
        }
        LIBNDT_EMIT_DEBUG("ws_handshake: complete");
        return Err::none;
      }
    }
  }
  LIBNDT_EMIT_DEBUG("ws_handshake: got too many headers");
  return Err::value_too_large;
}

std::string Client::ws_prepare_frame(uint8_t first_byte, uint8_t *base,
                                     Size count) const noexcept {
  // TODO(bassosimone): perhaps move the RNG into Client?
  constexpr Size mask_size = 4;
  uint8_t mask[mask_size] = {};
  // "When preparing a masked frame, the client MUST pick a fresh masking
  //  key from the set of allowed 32-bit values." [RFC6455 Sect. 5.3]. Hence
  // we're not compliant (TODO(bassosimone)).
  random_printable_fill((char *)mask, sizeof(mask));
  std::stringstream ss;
  // Message header
  {
    // First byte
    {
      // TODO(bassosimone): add sanity checks for first byte
      ss << first_byte;
      LIBNDT_EMIT_DEBUG("ws_prepare_frame: FIN: " << std::boolalpha
                                        << ((first_byte & ws_fin_flag) != 0));
      LIBNDT_EMIT_DEBUG(
          "ws_prepare_frame: reserved: " << (first_byte & ws_reserved_mask));
      LIBNDT_EMIT_DEBUG("ws_prepare_frame: opcode: " << (first_byte & ws_opcode_mask));
    }
    // Length
    {
      LIBNDT_EMIT_DEBUG("ws_prepare_frame: mask flag: " << std::boolalpha << true);
      LIBNDT_EMIT_DEBUG("ws_prepare_frame: length: " << count);
      // Since this is a client implementation, we always include the MASK flag
      // as part of the second byte that we send on the wire. Also, the spec
      // says that we must emit the length in network byte order, which means
      // in practice that we should use big endian.
      //
      // See <https://tools.ietf.org/html/rfc6455#section-5.1>, and
      //     <https://tools.ietf.org/html/rfc6455#section-5.2>.
#define LB(value)                                                        \
  do {                                                                   \
    LIBNDT_EMIT_DEBUG("ws_prepare_frame: length byte: " << (unsigned int)(value)); \
    ss << (value);                                                       \
  } while (0)
      if (count < 126) {
        LB((uint8_t)((count & ws_len_mask) | ws_mask_flag));
      } else if (count < (1 << 16)) {
        LB((uint8_t)((126 & ws_len_mask) | ws_mask_flag));
        LB((uint8_t)((count >> 8) & 0xff));
        LB((uint8_t)(count & 0xff));
      } else {
        LB((uint8_t)((127 & ws_len_mask) | ws_mask_flag));
        LB((uint8_t)((count >> 56) & 0xff));
        LB((uint8_t)((count >> 48) & 0xff));
        LB((uint8_t)((count >> 40) & 0xff));
        LB((uint8_t)((count >> 32) & 0xff));
        LB((uint8_t)((count >> 24) & 0xff));
        LB((uint8_t)((count >> 16) & 0xff));
        LB((uint8_t)((count >> 8) & 0xff));
        LB((uint8_t)(count & 0xff));
      }
#undef LB  // Tidy
    }
    // Mask
    {
      for (Size i = 0; i < mask_size; ++i) {
        LIBNDT_EMIT_DEBUG("ws_prepare_frame: mask byte: " << (unsigned int)mask[i]
                                                << " ('" << mask[i] << "')");
        ss << (uint8_t)mask[i];
      }
    }
  }
  // As mentioned in the docs of this method, we will not include any
  // body in the frame if base is a null pointer.
  {
    for (Size i = 0; i < count && base != nullptr; ++i) {
      // Implementation note: judging from a GCC 8 warning, it seems that using
      // `^=` causes -Wconversion warnings, while using `= ... ^` does not.
      base[i] = base[i] ^ mask[i % mask_size];
      ss << base[i];
    }
  }
  return ss.str();
}

Err Client::ws_send_frame(Socket sock, uint8_t first_byte, uint8_t *base,
                          Size count) const noexcept {
  std::string prep = ws_prepare_frame(first_byte, base, count);
  return netx_sendn(sock, prep.c_str(), prep.size());
}

Err Client::ws_recv_any_frame(Socket sock, uint8_t *opcode, bool *fin,
      uint8_t *base, Size total, Size *count) const noexcept {
  // TODO(bassosimone): in this function we should consider an EOF as an
  // error, because with WebSocket we have explicit FIN mechanism.
  if (opcode == nullptr || fin == nullptr || count == nullptr) {
    LIBNDT_EMIT_WARNING("ws_recv_any_frame: passed invalid return arguments");
    return Err::invalid_argument;
  }
  *opcode = 0;
  *fin = false;
  *count = 0;
  if (base == nullptr || total <= 0) {
    LIBNDT_EMIT_WARNING("ws_recv_any_frame: passed invalid buffer arguments");
    return Err::invalid_argument;
  }
  // Message header
  Size length = 0;
  // This assert is because the code below assumes that Size is basically
  // a uint64_t value. On 32 bit systems my understanding is that the compiler
  // supports 64 bit integers via emulation, hence I believe there is no
  // need to be worried about using a 64 bit integer here. My understanding
  // is supported, e.g., by <https://stackoverflow.com/a/2692369>.
  static_assert(sizeof(Size) == sizeof(uint64_t), "Size is not 64 bit wide");
  {
    uint8_t buf[2];
    auto err = netx_recvn(sock, buf, sizeof(buf));
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("ws_recv_any_frame: netx_recvn() failed for header");
      return err;
    }
    LIBNDT_EMIT_DEBUG("ws_recv_any_frame: ws header: "
               << represent(std::string{(char *)buf, sizeof(buf)}));
    *fin = (buf[0] & ws_fin_flag) != 0;
    LIBNDT_EMIT_DEBUG("ws_recv_any_frame: FIN: " << std::boolalpha << *fin);
    uint8_t reserved = (uint8_t)(buf[0] & ws_reserved_mask);
    if (reserved != 0) {
      // They only make sense for extensions, which we don't use. So we return
      // error. See <https://tools.ietf.org/html/rfc6455#section-5.2>.
      LIBNDT_EMIT_WARNING("ws_recv_any_frame: invalid reserved bits: " << reserved);
      return Err::ws_proto;
    }
    *opcode = (uint8_t)(buf[0] & ws_opcode_mask);
    LIBNDT_EMIT_DEBUG("ws_recv_any_frame: opcode: " << (unsigned int)*opcode);
    switch (*opcode) {
      // clang-format off
      case ws_opcode_continue:
      case ws_opcode_text:
      case ws_opcode_binary:
      case ws_opcode_close:
      case ws_opcode_ping:
      case ws_opcode_pong: break;
      // clang-format off
      default:
        // See <https://tools.ietf.org/html/rfc6455#section-5.2>.
        LIBNDT_EMIT_WARNING("ws_recv_any_frame: invalid opcode");
        return Err::ws_proto;
    }
    auto hasmask = (buf[1] & ws_mask_flag) != 0;
    // We do not expect to receive a masked frame. This is client code and
    // the RFC says that a server MUST NOT mask its frames.
    //
    // See <https://tools.ietf.org/html/rfc6455#section-5.1>.
    if (hasmask) {
      LIBNDT_EMIT_WARNING("ws_recv_any_frame: received masked frame");
      return Err::invalid_argument;
    }
    length = (buf[1] & ws_len_mask);
    switch (*opcode) {
      case ws_opcode_close:
      case ws_opcode_ping:
      case ws_opcode_pong:
        if (length > 125 || *fin == false) {
          LIBNDT_EMIT_WARNING("ws_recv_any_frame: control messages MUST have a "
                       "payload length of 125 bytes or less and MUST NOT "
                       "be fragmented (see RFC6455 Sect 5.5.)");
          return Err::ws_proto;
        }
        break;
    }
    // As mentioned above, length is transmitted using big endian encoding.
#define AL(value)                                                            \
  do {                                                                       \
    LIBNDT_EMIT_DEBUG("ws_recv_any_frame: length byte: " << (unsigned int)(value)); \
    length += (value);                                                       \
  } while (0)
    // The following should not happen because the lenght is over 7 bits but
    // it's nice to enforce assertions to make assumptions explicit.
    assert(length <= 127);
    if (length == 126) {
      uint8_t len_buf[2];
      auto recvn_err = netx_recvn(sock, len_buf, sizeof(len_buf));
      if (recvn_err != Err::none) {
        LIBNDT_EMIT_WARNING(
            "ws_recv_any_frame: netx_recvn() failed for 16 bit length");
        return recvn_err;
      }
      LIBNDT_EMIT_DEBUG("ws_recv_any_frame: 16 bit length: "
                 << represent(std::string{(char *)len_buf, sizeof(len_buf)}));
      length = 0;  // Need to reset the length as AL() does +=
      AL(((Size)len_buf[0]) << 8);
      AL((Size)len_buf[1]);
    } else if (length == 127) {
      uint8_t len_buf[8];
      auto recvn_err = netx_recvn(sock, len_buf, sizeof(len_buf));
      if (recvn_err != Err::none) {
        LIBNDT_EMIT_WARNING(
            "ws_recv_any_frame: netx_recvn() failed for 64 bit length");
        return recvn_err;
      }
      LIBNDT_EMIT_DEBUG("ws_recv_any_frame: 64 bit length: "
                 << represent(std::string{(char *)len_buf, sizeof(len_buf)}));
      length = 0;  // Need to reset the length as AL() does +=
      AL(((Size)len_buf[0]) << 56);
      if ((len_buf[0] & 0x80) != 0) {
        // See <https://tools.ietf.org/html/rfc6455#section-5.2>: "[...] the
        // most significant bit MUST be 0."
        LIBNDT_EMIT_WARNING("ws_recv_any_frame: 64 bit length: invalid first bit");
        return Err::ws_proto;
      }
      AL(((Size)len_buf[1]) << 48);
      AL(((Size)len_buf[2]) << 40);
      AL(((Size)len_buf[3]) << 32);
      AL(((Size)len_buf[4]) << 24);
      AL(((Size)len_buf[5]) << 16);
      AL(((Size)len_buf[6]) << 8);
      AL(((Size)len_buf[7]));
    }
#undef AL  // Tidy
    if (length > total) {
      LIBNDT_EMIT_WARNING("ws_recv_any_frame: buffer too small");
      return Err::message_size;
    }
    LIBNDT_EMIT_DEBUG("ws_recv_any_frame: length: " << length);
  }
  LIBNDT_EMIT_DEBUG("ws_recv_any_frame: received header");
  // Message body
  if (length > 0) {
    assert(length <= total);
    auto err = netx_recvn(sock, base, length);
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("ws_recv_any_frame: netx_recvn() failed for body");
      return err;
    }
    // This makes the code too noisy when using -verbose. It may still be
    // useful to remove the comment when debugging.
    /*
    LIBNDT_EMIT_DEBUG("ws_recv_any_frame: received body: "
               << represent(std::string{(char *)base, length}));
    */
    *count = length;
  } else {
    LIBNDT_EMIT_DEBUG("ws_recv_any_frame: no body in this message");
    assert(*count == 0);
  }
  return Err::none;
}

Err Client::ws_recv_frame(Socket sock, uint8_t *opcode, bool *fin,
      uint8_t *base, Size total, Size *count) const noexcept {
  // "Control frames (see Section 5.5) MAY be injected in the middle of
  // a fragmented message.  Control frames themselves MUST NOT be fragmented."
  //    -- RFC6455 Section 5.4.
  if (opcode == nullptr || fin == nullptr || count == nullptr) {
    LIBNDT_EMIT_WARNING("ws_recv_frame: passed invalid return arguments");
    return Err::invalid_argument;
  }
  if (base == nullptr || total <= 0) {
    LIBNDT_EMIT_WARNING("ws_recv_frame: passed invalid buffer arguments");
    return Err::invalid_argument;
  }
  auto err = Err::none;
again:
  *opcode = 0;
  *fin = false;
  *count = 0;
  err = ws_recv_any_frame(sock, opcode, fin, base, total, count);
  if (err != Err::none) {
    LIBNDT_EMIT_WARNING("ws_recv_frame: ws_recv_any_frame() failed");
    return err;
  }
  // "The application MUST NOT send any more data frames after sending a
  // Close frame." (RFC6455 Sect. 5.5.1). We're good as long as, for example,
  // we don't ever send a CLOSE but we just reply to CLOSE and then return
  // with an error, which will cause the connection to be closed. Note that
  // we MUST reply with CLOSE here (again Sect. 5.5.1).
  if (*opcode == ws_opcode_close) {
    LIBNDT_EMIT_DEBUG("ws_recv_frame: received CLOSE frame; sending CLOSE back");
    // Setting the FIN flag because control messages MUST NOT be fragmented
    // as specified in Section 5.5 of RFC6455.
    (void)ws_send_frame(sock, ws_opcode_close | ws_fin_flag, nullptr, 0);
    // TODO(bassosimone): distinguish between a shutdown at the socket layer
    // and a proper shutdown implemented at the WebSocket layer.
    return Err::eof;
  }
  if (*opcode == ws_opcode_pong) {
    // RFC6455 Sect. 5.5.3 says that we must ignore a PONG.
    LIBNDT_EMIT_DEBUG("ws_recv_frame: received PONG frame; continuing to read");
    goto again;
  }
  if (*opcode == ws_opcode_ping) {
    // TODO(bassosimone): in theory a malicious server could DoS us by sending
    // a constant stream of PING frames for a long time.
    LIBNDT_EMIT_DEBUG("ws_recv_frame: received PING frame; PONGing back");
    assert(*count <= total);
    err = ws_send_frame(sock, ws_opcode_pong | ws_fin_flag, base, *count);
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("ws_recv_frame: ws_send_frame() failed for PONG frame");
      return err;
    }
    LIBNDT_EMIT_DEBUG("ws_recv_frame: continuing to read after PONG");
    goto again;
  }
  return Err::none;
}

Err Client::ws_recvmsg(  //
    Socket sock, uint8_t *opcode, uint8_t *base, Size total,
    Size *count) const noexcept {
  // General remark from RFC6455 Sect. 5.4: "[I]n absence of extensions, senders
  // and receivers must not depend on [...] specific frame boundaries."
  //
  // Also: "In the absence of any extension, a receiver doesn't have to buffer
  // the whole frame in order to process it." (Sect 5.4). However, currently
  // this implementation does that because we know NDT messages are "smallish"
  // not only for the control protocol but also for c2s and s2c, where in
  // general we attempt to use messages smaller than 256K.
  if (opcode == nullptr || count == nullptr) {
    LIBNDT_EMIT_WARNING("ws_recv: passed invalid return arguments");
    return Err::invalid_argument;
  }
  if (base == nullptr || total <= 0) {
    LIBNDT_EMIT_WARNING("ws_recv: passed invalid buffer arguments");
    return Err::invalid_argument;
  }
  bool fin = false;
  *opcode = 0;
  *count = 0;
  auto err = ws_recv_frame(sock, opcode, &fin, base, total, count);
  if (err != Err::none) {
    // We don't want to scary the user in case of clean EOF
    if (err != Err::eof) {
      LIBNDT_EMIT_WARNING("ws_recv: ws_recv_frame() failed for first frame");
    }
    return err;
  }
  if (*opcode != ws_opcode_binary && *opcode != ws_opcode_text) {
    LIBNDT_EMIT_WARNING("ws_recv: received unexpected opcode: " << *opcode);
    return Err::ws_proto;
  }
  if (fin) {
    LIBNDT_EMIT_DEBUG("ws_recv: the first frame is also the last frame");
    return Err::none;
  }
  while (*count < total) {
    if ((uintptr_t)base > UINTPTR_MAX - *count) {
      LIBNDT_EMIT_WARNING("ws_recv: avoiding pointer overflow");
      return Err::value_too_large;
    }
    uint8_t op = 0;
    Size n = 0;
    err = ws_recv_frame(sock, &op, &fin, base + *count, total - *count, &n);
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("ws_recv: ws_recv_frame() failed for continuation frame");
      return err;
    }
    if (*count > SizeMax - n) {
      LIBNDT_EMIT_WARNING("ws_recv: avoiding integer overflow");
      return Err::value_too_large;
    }
    *count += n;
    if (op != ws_opcode_continue) {
      LIBNDT_EMIT_WARNING("ws_recv: received unexpected opcode: " << op);
      return Err::ws_proto;
    }
    if (fin) {
      LIBNDT_EMIT_DEBUG("ws_recv: this is the last frame");
      return Err::none;
    }
    LIBNDT_EMIT_DEBUG("ws_recv: this is not the last frame");
  }
  LIBNDT_EMIT_WARNING("ws_recv: buffer smaller than incoming message");
  return Err::message_size;
}

// } - - - END WEBSOCKET IMPLEMENTATION - - -

// Networking layer
// ````````````````

// Required by OpenSSL code below. Must be outside because we want the code
// to compile also where we don't have OpenSSL support enabled.
#ifdef _WIN32
#define OS_SET_LAST_ERROR(ec) ::SetLastError(ec)
#define OS_EINVAL WSAEINVAL
#else
#define OS_SET_LAST_ERROR(ec) errno = ec
#define OS_EINVAL EINVAL
#endif

// - - - BEGIN BIO IMPLEMENTATION - - - {
//
// This BIO implementation is based on the implementation of rabbitmq-c
// by @alanxz: <https://github.com/alanxz/rabbitmq-c/pull/402>.
//
// The code is available under the MIT license.
//
// The purpose of this BIO implementation is to pass the MSG_NOSIGNAL
// flag to socket I/O functions on Linux systems. While there, it seems
// convenient to route these I/O calls to the mockable methods of the
// client class, allowing for (1) more regress testing and (2) the
// possibility to very easily observe bytes on the wire. (I know that
// OpenSSL also allows that using callbacks but since we're making a
// BIO that possibility comes out very easily anyway.)
//
// We assume that a OpenSSL 1.1.0-like API is available.
/*-
 * Portions created by Alan Antonuk are Copyright (c) 2017 Alan Antonuk.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// Helper used to route read and write calls to Client's I/O methods. We
// disregard the const qualifier of the `base` argument for the write operation,
// but that is not a big deal since we add it again before calling the real
// Socket op (see libndt_bio_write() below).
static int libndt_bio_operation(
    BIO *bio, char *base, int count,
    std::function<Ssize(Client *, Socket, char *, Size)> operation,
    std::function<void(BIO *)> set_retry) noexcept {
  // Implementation note: before we have a valid Client pointer we cannot
  // of course use mocked functions. Hence OS_SET_LAST_ERROR().
  if (bio == nullptr || base == nullptr || count <= 0) {
    OS_SET_LAST_ERROR(OS_EINVAL);
    return -1;
  }
  auto clnt = static_cast<Client *>(::BIO_get_data(bio));
  if (clnt == nullptr) {
    OS_SET_LAST_ERROR(OS_EINVAL);
    return -1;
  }
  // Using a `int` to store a `SOCKET` is safe for internal non documented
  // reasons: even on Windows64 kernel handles use only 24 bits. See also
  // this Stack Overflow post: <https://stackoverflow.com/a/1953738>.
  int sock{};
  ::BIO_get_fd(bio, &sock);
  ::BIO_clear_retry_flags(bio);
  // Cast to Socket safe as int is okay to represent a Socket as we explained
  // above. Cast to Size safe because we've checked for negative above.
  Ssize rv = operation(clnt, (Socket)sock, base, (Size)count);
  if (rv < 0) {
    assert(rv == -1);
    auto err = clnt->netx_map_errno(clnt->sys_get_last_error());
    if (err == Err::operation_would_block) {
      set_retry(bio);
    }
    return -1;
  }
  // Cast to int safe because count was initially int. We anyway deploy an
  // assertion just in case (TM) but that should not happen (TM).
  assert(rv <= INT_MAX);
  return (int)rv;
}

// Write data using the underlying socket.
static int libndt_bio_write(BIO *bio, const char *base, int count) noexcept {
  // clang-format off
  return libndt_bio_operation(
      bio, (char *)base, count,
      [](Client *clnt, Socket sock, char *base, Size count) noexcept {
        return clnt->sys_send(sock, (const char *)base, count);
      },
      [](BIO *bio) noexcept { ::BIO_set_retry_write(bio); });
  // clang-format on
}

// Read data using the underlying socket.
static int libndt_bio_read(BIO *bio, char *base, int count) noexcept {
  // clang-format off
  return libndt_bio_operation(
      bio, base, count,
      [](Client *clnt, Socket sock, char *base, Size count) noexcept {
        return clnt->sys_recv(sock, base, count);
      },
      [](BIO *bio) noexcept { ::BIO_set_retry_read(bio); });
  // clang-format on
}

class BioMethodDeleter {
 public:
  void operator()(BIO_METHOD *meth) noexcept {
    if (meth != nullptr) {
      ::BIO_meth_free(meth);
    }
  }
};
using UniqueBioMethod = std::unique_ptr<BIO_METHOD, BioMethodDeleter>;

static BIO_METHOD *libndt_bio_method() noexcept {
  static std::atomic_bool initialized{false};
  static UniqueBioMethod method;
  static std::mutex mutex;
  if (!initialized) {
    std::unique_lock<std::mutex> _{mutex};
    if (!initialized) {
      BIO_METHOD *mm = ::BIO_meth_new(BIO_TYPE_SOCKET, "libndt_bio_method");
      if (mm == nullptr) {
        return nullptr;
      }
      // BIO_s_socket() returns a const BIO_METHOD in OpenSSL v1.1.0. We cast
      // that back to non const for the purpose of getting its methods.
      BIO_METHOD *m = (BIO_METHOD *)BIO_s_socket();
      BIO_meth_set_create(mm, BIO_meth_get_create(m));
      BIO_meth_set_destroy(mm, BIO_meth_get_destroy(m));
      BIO_meth_set_ctrl(mm, BIO_meth_get_ctrl(m));
      BIO_meth_set_callback_ctrl(mm, BIO_meth_get_callback_ctrl(m));
      BIO_meth_set_read(mm, libndt_bio_read);
      BIO_meth_set_write(mm, libndt_bio_write);
      BIO_meth_set_gets(mm, BIO_meth_get_gets(m));
      BIO_meth_set_puts(mm, BIO_meth_get_puts(m));
      method.reset(mm);
      initialized = true;
    }
  }
  return method.get();
}

// } - - - END BIO IMPLEMENTATION - - -

// Common function to map OpenSSL errors to Err.
static Err map_ssl_error(const Client *client, SSL *ssl, int ret) noexcept {
  auto reason = ::SSL_get_error(ssl, ret);
  switch (reason) {
    case SSL_ERROR_NONE:
      return Err::none;
    case SSL_ERROR_ZERO_RETURN:
      // TODO(bassosimone): consider the issue of dirty shutdown.
      return Err::eof;
    case SSL_ERROR_WANT_READ:
      return Err::ssl_want_read;
    case SSL_ERROR_WANT_WRITE:
      return Err::ssl_want_write;
    case SSL_ERROR_SYSCALL:
      auto ecode = client->sys_get_last_error();
      if (ecode) {
        return client->netx_map_errno(ecode);
      }
      return Err::ssl_syscall;
  }
  // TODO(bassosimone): in this case it may be nice to print the error queue
  // so to give the user a better understanding of what has happened.
  return Err::ssl_generic;
}

// Retry simple, nonblocking OpenSSL operations such as handshake or shutdown.
static Err ssl_retry_unary_op(std::string opname, Client *client, SSL *ssl,
                              Socket fd, Timeout timeout,
                              std::function<int(SSL *)> unary_op) noexcept {
  auto err = Err::none;
again:
  err = map_ssl_error(client, ssl, unary_op(ssl));
  // Retry if needed
  if (err == Err::ssl_want_read) {
    err = client->netx_wait_readable(fd, timeout);
    if (err == Err::none) {
      // TODO(bassosimone): make sure we don't loop in this function forever.
      goto again;
    }
  } else if (err == Err::ssl_want_write) {
    err = client->netx_wait_writeable(fd, timeout);
    if (err == Err::none) {
      goto again;
    }
  }
  // Otherwise let the caller know
  if (err != Err::none) {
    LIBNDT_EMIT_WARNING_EX(client, opname << " failed: " << libndt_perror(err));
  }
  return err;
}

Err Client::netx_maybews_dial(const std::string &hostname,
                              const std::string &port, uint64_t ws_flags,
                              std::string ws_protocol, std::string url_path,
                              Socket *sock) noexcept {
  auto err = netx_maybessl_dial(hostname, port, sock);
  if (err != Err::none) {
    return err;
  }
  LIBNDT_EMIT_DEBUG("netx_maybews_dial: netx_maybessl_dial() returned successfully");
  if ((settings_.protocol_flags & protocol_flag_websocket) == 0) {
    LIBNDT_EMIT_DEBUG("netx_maybews_dial: websocket not enabled");
    return Err::none;
  }
  LIBNDT_EMIT_DEBUG("netx_maybews_dial: about to start websocket handhsake");
  err = ws_handshake(*sock, port, ws_flags, ws_protocol, url_path);
  if (err != Err::none) {
    (void)netx_closesocket(*sock);
    *sock = (Socket)-1;
    return err;
  }
  LIBNDT_EMIT_DEBUG("netx_maybews_dial: established websocket channel");
  return Err::none;
}

Err Client::netx_maybessl_dial(const std::string &hostname,
                               const std::string &port, Socket *sock) noexcept {
  // Temporarily clear the TLS flag because I/O functions inside of socks5h
  // code would otherwise fail given we've not established TLS yet. Then restore
  // the original flags right after the socks5h code returns.
  auto flags = settings_.protocol_flags;
  settings_.protocol_flags &= ~protocol_flag_tls;
  auto err = netx_maybesocks5h_dial(hostname, port, sock);
  settings_.protocol_flags = flags;
  if (err != Err::none) {
    return err;
  }
  LIBNDT_EMIT_DEBUG(
      "netx_maybessl_dial: netx_maybesocks5h_dial() returned successfully");
  if ((settings_.protocol_flags & protocol_flag_tls) == 0) {
    LIBNDT_EMIT_DEBUG("netx_maybessl_dial: TLS not enabled");
    return Err::none;
  }
  LIBNDT_EMIT_DEBUG("netx_maybetls_dial: about to start TLS handshake");
  if (settings_.ca_bundle_path.empty() && settings_.tls_verify_peer) {
#ifndef _WIN32
    // See <https://serverfault.com/a/722646>
    std::vector<std::string> candidates{
        "/etc/ssl/cert.pem",                   // macOS
        "/etc/ssl/certs/ca-certificates.crt",  // Debian
    };
    for (auto &candidate : candidates) {
      if (access(candidate.c_str(), R_OK) == 0) {
        LIBNDT_EMIT_DEBUG("Using '" << candidate.c_str() << "' as CA");
        settings_.ca_bundle_path = candidate;
        break;
      }
    }
    if (settings_.ca_bundle_path.empty()) {
#endif
      LIBNDT_EMIT_WARNING(
          "You did not provide me with a CA bundle path. Without this "
          "information I cannot validate the other TLS endpoint. So, "
          "I will not continue to run this test.");
      return Err::invalid_argument;
#ifndef _WIN32
    }
#endif
  }
  SSL *ssl = nullptr;
  {
    // TODO(bassosimone): understand whether we can remove old SSL versions
    // taking into account that the NDT server runs on very old code.
    SSL_CTX *ctx = ::SSL_CTX_new(SSLv23_client_method());
    if (ctx == nullptr) {
      LIBNDT_EMIT_WARNING("SSL_CTX_new() failed");
      netx_closesocket(*sock);
      return Err::ssl_generic;
    }
    LIBNDT_EMIT_DEBUG("SSL_CTX created");
    if (settings_.tls_verify_peer) {
      if (!::SSL_CTX_load_verify_locations(  //
              ctx, settings_.ca_bundle_path.c_str(), nullptr)) {
        LIBNDT_EMIT_WARNING("Cannot load the CA bundle path from the file system");
        ::SSL_CTX_free(ctx);
        netx_closesocket(*sock);
        return Err::ssl_generic;
      }
      LIBNDT_EMIT_DEBUG("Loaded the CA bundle path");
    }
    ssl = ::SSL_new(ctx);
    if (ssl == nullptr) {
      LIBNDT_EMIT_WARNING("SSL_new() failed");
      ::SSL_CTX_free(ctx);
      netx_closesocket(*sock);
      return Err::ssl_generic;
    }
    LIBNDT_EMIT_DEBUG("SSL created");
    ::SSL_CTX_free(ctx);  // Referenced by `ssl` so safe to free here
    assert(fd_to_ssl_.count(*sock) == 0);
    // Implementation note: after this point `netx_closesocket(*sock)` will
    // imply that `::SSL_free(ssl)` is also called.
    fd_to_ssl_[*sock] = ssl;
  }
  BIO *bio = ::BIO_new(libndt_bio_method());
  if (bio == nullptr) {
    LIBNDT_EMIT_WARNING("BIO_new() failed");
    netx_closesocket(*sock);
    //::SSL_free(ssl); // MUST NOT be called because of fd_to_ssl
    return Err::ssl_generic;
  }
  LIBNDT_EMIT_DEBUG("libndt BIO created");
  // We use BIO_NOCLOSE because it's the socket that owns the BIO and the SSL
  // via fd_to_ssl rather than the other way around. Note that sockets are
  // always `int` in OpenSSL notwithstanding their definition on Windows, so
  // here we're casting unconditionally to silence compiler warnings.
  //
  // See <https://www.openssl.org/docs/man1.1.1/man3/BIO_s_socket.html> and
  //     <https://stackoverflow.com/questions/1953639> for why this is scary
  //     but fundamentally the right thing to do in this context.
  ::BIO_set_fd(bio, (int)*sock, BIO_NOCLOSE);
  // For historical reasons, if the two BIOs are equal, the SSL object will
  // increase the refcount of bio just once rather than twice.
  ::SSL_set_bio(ssl, bio, bio);
  ::BIO_set_data(bio, this);
  ::SSL_set_connect_state(ssl);
  LIBNDT_EMIT_DEBUG("Socket added to SSL context");
  if (settings_.tls_verify_peer) {
    // This approach for validating the hostname should work with versions
    // of OpenSSL greater than v1.0.2 and with LibreSSL. Code taken from the
    // wiki: <https://wiki.openssl.org/index.php/Hostname_validation>.
    X509_VERIFY_PARAM *p = SSL_get0_param(ssl);
    assert(p != nullptr);
    X509_VERIFY_PARAM_set_hostflags(p, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (!::X509_VERIFY_PARAM_set1_host(p, hostname.data(), hostname.size())) {
      LIBNDT_EMIT_WARNING("Cannot set the hostname for hostname validation");
      netx_closesocket(*sock);
      //::SSL_free(ssl); // MUST NOT be called because of fd_to_ssl
      return Err::ssl_generic;
    }
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    LIBNDT_EMIT_DEBUG("SSL_VERIFY_PEER configured");
  }
  err = ssl_retry_unary_op("SSL_do_handshake", this, ssl, *sock,
                           settings_.timeout, [](SSL *ssl) -> int {
                             ERR_clear_error();
                             return ::SSL_do_handshake(ssl);
                           });
  if (err != Err::none) {
    netx_closesocket(*sock);
    //::SSL_free(ssl); // MUST NOT be called because of fd_to_ssl
    return Err::ssl_generic;
  }
  LIBNDT_EMIT_DEBUG("SSL handshake complete");
  return Err::none;
}

Err Client::netx_maybesocks5h_dial(const std::string &hostname,
                                   const std::string &port,
                                   Socket *sock) noexcept {
  if (settings_.socks5h_port.empty()) {
    LIBNDT_EMIT_DEBUG("socks5h: not configured, connecting directly");
    return netx_dial(hostname, port, sock);
  }
  {
    auto err = netx_dial("127.0.0.1", settings_.socks5h_port, sock);
    if (err != Err::none) {
      return err;
    }
  }
  LIBNDT_EMIT_INFO("socks5h: connected to proxy");
  {
    char auth_request[] = {
        5,  // version
        1,  // number of methods
        0   // "no auth" method
    };
    auto err = netx_sendn(*sock, auth_request, sizeof(auth_request));
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("socks5h: cannot send auth_request");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return err;
    }
    LIBNDT_EMIT_DEBUG("socks5h: sent this auth request: "
               << represent(std::string{auth_request, sizeof(auth_request)}));
  }
  {
    char auth_response[2] = {
        0,  // version
        0   // method
    };
    auto err = netx_recvn(*sock, auth_response, sizeof(auth_response));
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("socks5h: cannot recv auth_response");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return err;
    }
    constexpr uint8_t version = 5;
    if (auth_response[0] != version) {
      LIBNDT_EMIT_WARNING("socks5h: received unexpected version number");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return Err::socks5h;
    }
    constexpr uint8_t auth_method = 0;
    if (auth_response[1] != auth_method) {
      LIBNDT_EMIT_WARNING("socks5h: received unexpected auth_method");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return Err::socks5h;
    }
    LIBNDT_EMIT_DEBUG("socks5h: authenticated with proxy; response: "
               << represent(std::string{auth_response, sizeof(auth_response)}));
  }
  {
    std::string connect_request;
    {
      std::stringstream ss;
      ss << (uint8_t)5;  // version
      ss << (uint8_t)1;  // CMD_CONNECT
      ss << (uint8_t)0;  // reserved
      ss << (uint8_t)3;  // ATYPE_DOMAINNAME
      if (hostname.size() > UINT8_MAX) {
        LIBNDT_EMIT_WARNING("socks5h: hostname is too long");
        netx_closesocket(*sock);
        *sock = (libndt::Socket)-1;
        return Err::invalid_argument;
      }
      ss << (uint8_t)hostname.size();
      ss << hostname;
      uint16_t portno{};
      {
        const char *errstr = nullptr;
        portno = (uint16_t)sys_strtonum(port.c_str(), 0, UINT16_MAX, &errstr);
        if (errstr != nullptr) {
          LIBNDT_EMIT_WARNING("socks5h: invalid port number: " << errstr);
          netx_closesocket(*sock);
          *sock = (libndt::Socket)-1;
          return Err::invalid_argument;
        }
      }
      portno = htons(portno);
      ss << (uint8_t)((char *)&portno)[0] << (uint8_t)((char *)&portno)[1];
      connect_request = ss.str();
      LIBNDT_EMIT_DEBUG("socks5h: connect_request: " << represent(connect_request));
    }
    auto err = netx_sendn(  //
        *sock, connect_request.data(), connect_request.size());
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("socks5h: cannot send connect_request");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return err;
    }
    LIBNDT_EMIT_DEBUG("socks5h: sent connect request");
  }
  {
    char connect_response_hdr[] = {
        0,  // version
        0,  // error
        0,  // reserved
        0   // type
    };
    auto err = netx_recvn(  //
        *sock, connect_response_hdr, sizeof(connect_response_hdr));
    if (err != Err::none) {
      LIBNDT_EMIT_WARNING("socks5h: cannot recv connect_response_hdr");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return err;
    }
    LIBNDT_EMIT_DEBUG("socks5h: connect_response_hdr: " << represent(std::string{
                   connect_response_hdr, sizeof(connect_response_hdr)}));
    constexpr uint8_t version = 5;
    if (connect_response_hdr[0] != version) {
      LIBNDT_EMIT_WARNING("socks5h: invalid message version");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return Err::socks5h;
    }
    if (connect_response_hdr[1] != 0) {
      // TODO(bassosimone): map the socks5 error to a system error
      LIBNDT_EMIT_WARNING("socks5h: connect() failed: "
                   << (unsigned)(uint8_t)connect_response_hdr[1]);
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return Err::io_error;
    }
    if (connect_response_hdr[2] != 0) {
      LIBNDT_EMIT_WARNING("socks5h: invalid reserved field");
      netx_closesocket(*sock);
      *sock = (libndt::Socket)-1;
      return Err::socks5h;
    }
    // receive IP or domain
    switch (connect_response_hdr[3]) {
      case 1:  // ipv4
      {
        constexpr Size expected = 4;  // ipv4
        char buf[expected];
        auto recvn_err = netx_recvn(*sock, buf, sizeof(buf));
        if (recvn_err != Err::none) {
          LIBNDT_EMIT_WARNING("socks5h: cannot recv ipv4 address");
          netx_closesocket(*sock);
          *sock = (libndt::Socket)-1;
          return recvn_err;
        }
        // TODO(bassosimone): log the ipv4 address. However tor returns a zero
        // ipv4 and so there is little added value in logging.
        break;
      }
      case 3:  // domain
      {
        uint8_t len = 0;
        auto recvn_err = netx_recvn(*sock, &len, sizeof(len));
        if (recvn_err != Err::none) {
          LIBNDT_EMIT_WARNING("socks5h: cannot recv domain length");
          netx_closesocket(*sock);
          *sock = (libndt::Socket)-1;
          return recvn_err;
        }
        char domain[UINT8_MAX + 1];  // space for final '\0'
        recvn_err = netx_recvn(*sock, domain, len);
        if (recvn_err != Err::none) {
          LIBNDT_EMIT_WARNING("socks5h: cannot recv domain");
          netx_closesocket(*sock);
          *sock = (libndt::Socket)-1;
          return recvn_err;
        }
        domain[len] = 0;
        LIBNDT_EMIT_DEBUG("socks5h: domain: " << domain);
        break;
      }
      case 4:  // ipv6
      {
        constexpr Size expected = 16;  // ipv6
        char buf[expected];
        auto recvn_err = netx_recvn(*sock, buf, sizeof(buf));
        if (recvn_err != Err::none) {
          LIBNDT_EMIT_WARNING("socks5h: cannot recv ipv6 address");
          netx_closesocket(*sock);
          *sock = (libndt::Socket)-1;
          return recvn_err;
        }
        // TODO(bassosimone): log the ipv6 address. However tor returns a zero
        // ipv6 and so there is little added value in logging.
        break;
      }
      default:
        LIBNDT_EMIT_WARNING("socks5h: invalid address type");
        netx_closesocket(*sock);
        *sock = (libndt::Socket)-1;
        return Err::socks5h;
    }
    // receive the port
    {
      uint16_t real_port = 0;
      auto recvn_err = netx_recvn(*sock, &real_port, sizeof(real_port));
      if (recvn_err != Err::none) {
        LIBNDT_EMIT_WARNING("socks5h: cannot recv port");
        netx_closesocket(*sock);
        *sock = (libndt::Socket)-1;
        return recvn_err;
      }
      real_port = ntohs(real_port);
      LIBNDT_EMIT_DEBUG("socks5h: port number: " << real_port);
    }
  }
  LIBNDT_EMIT_INFO("socks5h: the proxy has successfully connected");
  return Err::none;
}

#ifdef _WIN32
#define E(name) WSAE##name
#else
#define E(name) E##name
#endif

/*static*/ Err Client::netx_map_errno(int ec) noexcept {
  // clang-format off
  switch (ec) {
    case 0: {
      assert(false);  // we don't expect `errno` to be zero
      return Err::io_error;
    }
#ifndef _WIN32
    case E(PIPE): return Err::broken_pipe;
#endif
    case E(CONNABORTED): return Err::connection_aborted;
    case E(CONNREFUSED): return Err::connection_refused;
    case E(CONNRESET): return Err::connection_reset;
    case E(HOSTUNREACH): return Err::host_unreachable;
    case E(INTR): return Err::interrupted;
    case E(INVAL): return Err::invalid_argument;
#ifndef _WIN32
    case E(IO): return Err::io_error;
#endif
    case E(NETDOWN): return Err::network_down;
    case E(NETRESET): return Err::network_reset;
    case E(NETUNREACH): return Err::network_unreachable;
    case E(INPROGRESS): return Err::operation_in_progress;
    case E(WOULDBLOCK): return Err::operation_would_block;
#if !defined _WIN32 && EAGAIN != EWOULDBLOCK
    case E(AGAIN): return Err::operation_would_block;
#endif
    case E(TIMEDOUT): return Err::timed_out;
  }
  // clang-format on
  return Err::io_error;
}

#undef E  // Tidy up

Err Client::netx_map_eai(int ec) noexcept {
  // clang-format off
  switch (ec) {
    case EAI_AGAIN: return Err::ai_again;
    case EAI_FAIL: return Err::ai_fail;
    case EAI_NONAME: return Err::ai_noname;
#ifdef EAI_SYSTEM
    case EAI_SYSTEM: {
      return netx_map_errno(sys_get_last_error());
    }
#endif
  }
  // clang-format on
  return Err::ai_generic;
}

#ifdef _WIN32
// Depending on the version of Winsock it's either EAGAIN or EINPROGRESS
#define CONNECT_IN_PROGRESS(e) \
  (e == Err::operation_would_block || e == Err::operation_in_progress)
#else
#define CONNECT_IN_PROGRESS(e) (e == Err::operation_in_progress)
#endif

Err Client::netx_dial(const std::string &hostname, const std::string &port,
                      Socket *sock) noexcept {
  assert(sock != nullptr);
  if (*sock != -1) {
    LIBNDT_EMIT_WARNING("netx_dial: socket already connected");
    return Err::invalid_argument;
  }
  // Implementation note: we could perform getaddrinfo() in one pass but having
  // a virtual API that resolves a hostname to a vector of IP addresses makes
  // life easier when you want to override hostname resolution, because you have
  // to reimplement a simpler method, compared to reimplementing getaddrinfo().
  std::vector<std::string> addresses;
  Err err;
  if ((err = netx_resolve(hostname, &addresses)) != Err::none) {
    return err;
  }
  for (auto &addr : addresses) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo *rp = nullptr;
    int rv = sys_getaddrinfo(addr.data(), port.data(), &hints, &rp);
    if (rv != 0) {
      LIBNDT_EMIT_WARNING("netx_dial: unexpected getaddrinfo() failure");
      return netx_map_eai(rv);
    }
    assert(rp);
    for (auto aip = rp; (aip); aip = aip->ai_next) {
      sys_set_last_error(0);
      *sock = sys_socket(aip->ai_family, aip->ai_socktype, 0);
      if (!is_socket_valid(*sock)) {
        LIBNDT_EMIT_WARNING("netx_dial: socket() failed");
        continue;
      }
#ifdef LIBNDT_HAVE_SO_NOSIGPIPE
      // Implementation note: SO_NOSIGPIPE is the nonportable BSD solution to
      // avoid SIGPIPE when writing on a connection closed by the peer.
      {
        auto on = 1;
        if (::setsockopt(  //
                *sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) {
          LIBNDT_EMIT_WARNING("netx_dial: setsockopt(..., SO_NOSIGPIPE) failed");
          sys_closesocket(*sock);
          *sock = -1;
          continue;
        }
      }
#endif  // LIBNDT_HAVE_SO_NOSIGPIPE
      if (netx_setnonblocking(*sock, true) != Err::none) {
        LIBNDT_EMIT_WARNING("netx_dial: netx_setnonblocking() failed");
        sys_closesocket(*sock);
        *sock = (libndt::Socket)-1;
        continue;
      }
      // While on Unix ai_addrlen is socklen_t, it's size_t on Windows. Just
      // for the sake of correctness, add a check that ensures that the size has
      // a reasonable value before casting to socklen_t. My understanding is
      // that size_t is `ULONG_PTR` while socklen_t is most likely `int`.
#ifdef _WIN32
      if (aip->ai_addrlen > sizeof(sockaddr_in6)) {
        LIBNDT_EMIT_WARNING("netx_dial: unexpected size of aip->ai_addrlen");
        sys_closesocket(*sock);
        *sock = (libndt::Socket)-1;
        continue;
      }
#endif
      if (sys_connect(*sock, aip->ai_addr, (socklen_t)aip->ai_addrlen) == 0) {
        LIBNDT_EMIT_DEBUG("netx_dial: connect(): okay immediately");
        break;
      }
      auto connect_err = netx_map_errno(sys_get_last_error());
      if (CONNECT_IN_PROGRESS(connect_err)) {
        connect_err = netx_wait_writeable(*sock, settings_.timeout);
        if (connect_err == Err::none) {
          int soerr = 0;
          socklen_t soerrlen = sizeof(soerr);
          if (sys_getsockopt(*sock, SOL_SOCKET, SO_ERROR, (void *)&soerr,
                             &soerrlen) == 0) {
            assert(soerrlen == sizeof(soerr));
            if (soerr == 0) {
              LIBNDT_EMIT_DEBUG("netx_dial: connect(): okay");
              break;
            }
            sys_set_last_error(soerr);
          }
        }
      }
      LIBNDT_EMIT_WARNING("netx_dial: connect() failed: "
                   << libndt_perror(netx_map_errno(sys_get_last_error())));
      sys_closesocket(*sock);
      *sock = (libndt::Socket)-1;
    }
    sys_freeaddrinfo(rp);
    if (*sock != -1) {
      break;  // we have a connection!
    }
  }
  // TODO(bassosimone): it's possible to write a better algorithm here
  return *sock != -1 ? Err::none : Err::io_error;
}

#undef CONNECT_IN_PROGRESS  // Tidy

Err Client::netx_recv(Socket fd, void *base, Size count,
                      Size *actual) const noexcept {
  auto err = Err::none;
again:
  err = netx_recv_nonblocking(fd, base, count, actual);
  if (err == Err::none) {
    return Err::none;
  }
  if (err == Err::operation_would_block || err == Err::ssl_want_read) {
    err = netx_wait_readable(fd, settings_.timeout);
  } else if (err == Err::ssl_want_write) {
    err = netx_wait_writeable(fd, settings_.timeout);
  }
  if (err == Err::none) {
    goto again;
  }
  LIBNDT_EMIT_WARNING(
      "netx_recv: netx_recv_nonblocking() failed: " << libndt_perror(err));
  return err;
}

Err Client::netx_recv_nonblocking(Socket fd, void *base, Size count,
                                  Size *actual) const noexcept {
  assert(base != nullptr && actual != nullptr);
  *actual = 0;
  if (count <= 0) {
    LIBNDT_EMIT_WARNING(
        "netx_recv_nonblocking: explicitly disallowing zero read; use "
        "netx_poll() to check the state of a socket");
    return Err::invalid_argument;
  }
  sys_set_last_error(0);
  if ((settings_.protocol_flags & protocol_flag_tls) != 0) {
    if (count > INT_MAX) {
      return Err::invalid_argument;
    }
    if (fd_to_ssl_.count(fd) != 1) {
      return Err::invalid_argument;
    }
    auto ssl = fd_to_ssl_.at(fd);
    // TODO(bassosimone): add mocks and regress tests for OpenSSL.
    ERR_clear_error();
    int ret = ::SSL_read(ssl, base, (int)count);
    if (ret <= 0) {
      return map_ssl_error(this, ssl, ret);
    }
    *actual = (Size)ret;
    return Err::none;
  }
  auto rv = sys_recv(fd, base, count);
  if (rv < 0) {
    assert(rv == -1);
    return netx_map_errno(sys_get_last_error());
  }
  if (rv == 0) {
    assert(count > 0);  // guaranteed by the above check
    return Err::eof;
  }
  *actual = (Size)rv;
  return Err::none;
}

Err Client::netx_recvn(Socket fd, void *base, Size count) const noexcept {
  Size off = 0;
  while (off < count) {
    Size n = 0;
    if ((uintptr_t)base > UINTPTR_MAX - off) {
      return Err::value_too_large;
    }
    Err err = netx_recv(fd, ((char *)base) + off, count - off, &n);
    if (err != Err::none) {
      return err;
    }
    if (off > SizeMax - n) {
      return Err::value_too_large;
    }
    off += n;
  }
  return Err::none;
}

Err Client::netx_send(Socket fd, const void *base, Size count,
                      Size *actual) const noexcept {
  auto err = Err::none;
again:
  err = netx_send_nonblocking(fd, base, count, actual);
  if (err == Err::none) {
    return Err::none;
  }
  if (err == Err::ssl_want_read) {
    err = netx_wait_readable(fd, settings_.timeout);
  } else if (err == Err::operation_would_block || err == Err::ssl_want_write) {
    err = netx_wait_writeable(fd, settings_.timeout);
  }
  if (err == Err::none) {
    goto again;
  }
  LIBNDT_EMIT_WARNING(
      "netx_send: netx_send_nonblocking() failed: " << libndt_perror(err));
  return err;
}

Err Client::netx_send_nonblocking(Socket fd, const void *base, Size count,
                                  Size *actual) const noexcept {
  assert(base != nullptr && actual != nullptr);
  *actual = 0;
  if (count <= 0) {
    LIBNDT_EMIT_WARNING(
        "netx_send_nonblocking: explicitly disallowing zero send; use "
        "netx_poll() to check the state of a socket");
    return Err::invalid_argument;
  }
  sys_set_last_error(0);
  if ((settings_.protocol_flags & protocol_flag_tls) != 0) {
    if (count > INT_MAX) {
      return Err::invalid_argument;
    }
    if (fd_to_ssl_.count(fd) != 1) {
      return Err::invalid_argument;
    }
    auto ssl = fd_to_ssl_.at(fd);
    ERR_clear_error();
    // TODO(bassosimone): add mocks and regress tests for OpenSSL.
    int ret = ::SSL_write(ssl, base, (int)count);
    if (ret <= 0) {
      return map_ssl_error(this, ssl, ret);
    }
    *actual = (Size)ret;
    return Err::none;
  }
  auto rv = sys_send(fd, base, count);
  if (rv < 0) {
    assert(rv == -1);
    return netx_map_errno(sys_get_last_error());
  }
  // Send() should not return zero unless count is zero. So consider a zero
  // return value as an I/O error rather than EOF.
  if (rv == 0) {
    assert(count > 0);  // guaranteed by the above check
    return Err::io_error;
  }
  *actual = (Size)rv;
  return Err::none;
}

Err Client::netx_sendn(Socket fd, const void *base, Size count) const noexcept {
  Size off = 0;
  while (off < count) {
    Size n = 0;
    if ((uintptr_t)base > UINTPTR_MAX - off) {
      return Err::value_too_large;
    }
    Err err = netx_send(fd, ((char *)base) + off, count - off, &n);
    if (err != Err::none) {
      return err;
    }
    if (off > SizeMax - n) {
      return Err::value_too_large;
    }
    off += n;
  }
  return Err::none;
}

Err Client::netx_resolve(const std::string &hostname,
                         std::vector<std::string> *addrs) noexcept {
  assert(addrs != nullptr);
  LIBNDT_EMIT_DEBUG("netx_resolve: " << hostname);
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;
  addrinfo *rp = nullptr;
  constexpr const char *portno = "80";  // any port would do
  int rv = sys_getaddrinfo(hostname.data(), portno, &hints, &rp);
  if (rv != 0) {
    hints.ai_flags &= ~AI_NUMERICHOST;
    rv = sys_getaddrinfo(hostname.data(), portno, &hints, &rp);
    if (rv != 0) {
      auto err = netx_map_eai(rv);
      LIBNDT_EMIT_WARNING(
          "netx_resolve: getaddrinfo() failed: " << libndt_perror(err));
      return err;
    }
    // FALLTHROUGH
  }
  assert(rp);
  LIBNDT_EMIT_DEBUG("netx_resolve: getaddrinfo(): okay");
  Err result = Err::none;
  for (auto aip = rp; (aip); aip = aip->ai_next) {
    char address[NI_MAXHOST], port[NI_MAXSERV];
    // The following casts from `size_t` to `socklen_t` are safe for sure
    // because NI_MAXHOST and NI_MAXSERV are small values. To make sure this
    // assumption is correct, deploy the following static assertion. Here I am
    // using INT_MAX as upper bound since socklen_t SHOULD be `int`.
    static_assert(sizeof(address) <= INT_MAX && sizeof(port) <= INT_MAX,
                  "Wrong assumption about NI_MAXHOST or NI_MAXSERV");
    // Additionally on Windows there's a cast from size_t to socklen_t that
    // needs to be handled as we do above for getaddrinfo().
#ifdef _WIN32
    if (aip->ai_addrlen > sizeof(sockaddr_in6)) {
      LIBNDT_EMIT_WARNING("netx_resolve: unexpected size of aip->ai_addrlen");
      result = Err::value_too_large;
      break;
    }
#endif
    if (sys_getnameinfo(aip->ai_addr, (socklen_t)aip->ai_addrlen, address,
                        (socklen_t)sizeof(address), port,
                        (socklen_t)sizeof(port),
                        NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
      LIBNDT_EMIT_WARNING("netx_resolve: unexpected getnameinfo() failure");
      result = Err::ai_generic;
      break;
    }
    addrs->push_back(address);  // we only care about address
    LIBNDT_EMIT_DEBUG("netx_resolve: - " << address);
  }
  sys_freeaddrinfo(rp);
  return result;
}

Err Client::netx_setnonblocking(Socket fd, bool enable) noexcept {
#ifdef _WIN32
  u_long lv = (enable) ? 1UL : 0UL;
  if (sys_ioctlsocket(fd, FIONBIO, &lv) != 0) {
    return netx_map_errno(sys_get_last_error());
  }
#else
  auto flags = sys_fcntl(fd, F_GETFL);
  if (flags < 0) {
    assert(flags == -1);
    return netx_map_errno(sys_get_last_error());
  }
  if (enable) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }
  if (sys_fcntl(fd, F_SETFL, flags) != 0) {
    return netx_map_errno(sys_get_last_error());
  }
#endif
  return Err::none;
}

static Err netx_wait(const Client *client, Socket fd, Timeout timeout,
                     short expected_events) noexcept {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events |= expected_events;
  std::vector<pollfd> pfds;
  pfds.push_back(pfd);
  // The following makes sure that it's okay to cast Timeout (an unsigned int
  // type) to poll()'s timeout type (i.e. signed int).
  static_assert(sizeof(timeout) == sizeof(int), "Unexpected Timeout size");
  if (timeout > INT_MAX / 1000) {
    timeout = INT_MAX / 1000;
  }
  auto err = client->netx_poll(&pfds, (int)timeout * 1000);
  // Either it's success and something happened or we failed and nothing
  // must have happened on the socket. We previously checked whether we had
  // `expected_events` set however the flags actually set by poll are
  // dependent on the system and file descriptor type. Hence it is more
  // robust to only make sure that at least a flag is set.
  //
  // Also, note that we explicitly clear revents in next_poll() before
  // calling the system implementation of poll().
  //
  // See also Stack Overflow: <https://stackoverflow.com/a/25249958>.
  assert((err == Err::none && pfds[0].revents != 0) ||
         (err != Err::none && pfds[0].revents == 0));
  return err;
}

Err Client::netx_wait_readable(Socket fd, Timeout timeout) const noexcept {
  return netx_wait(this, fd, timeout, POLLIN);
}

Err Client::netx_wait_writeable(Socket fd, Timeout timeout) const noexcept {
  return netx_wait(this, fd, timeout, POLLOUT);
}

Err Client::netx_poll(
      std::vector<pollfd> *pfds, int timeout_msec) const noexcept {
  if (pfds == nullptr) {
    LIBNDT_EMIT_WARNING("netx_poll: passed a null vector of descriptors");
    return Err::invalid_argument;
  }
  for (auto &pfd : *pfds) {
    pfd.revents = 0;  // clear unconditionally
  }
  int rv = 0;
#ifndef _WIN32
again:
#endif
  // Different operating systems have different representations of size_t
  // and of nfds_t. Overcome these differences by choosing a smaller
  // representation of the fdset size and letting the compiler promote
  // it to the correct integer. We don't need many fds in any case.
  if (pfds->size() > UINT8_MAX) {
    LIBNDT_EMIT_WARNING("netx_poll: avoiding overflow");
    return Err::value_too_large;
  }
  rv = sys_poll(pfds->data(), (uint8_t)pfds->size(), timeout_msec);
  // TODO(bassosimone): handle the case where POLLNVAL is returned.
#ifdef _WIN32
  if (rv == SOCKET_ERROR) {
    return netx_map_errno(sys_get_last_error());
  }
#else
  if (rv < 0) {
    assert(rv == -1);
    auto err = netx_map_errno(sys_get_last_error());
    if (err == Err::interrupted) {
      goto again;
    }
    return err;
  }
#endif
  return (rv == 0) ? Err::timed_out : Err::none;
}

Err Client::netx_shutdown_both(Socket fd) noexcept {
  if ((settings_.protocol_flags & protocol_flag_tls) != 0) {
    if (fd_to_ssl_.count(fd) != 1) {
      return Err::invalid_argument;
    }
    auto ssl = fd_to_ssl_.at(fd);
    auto err = ssl_retry_unary_op(  //
        "SSL_shutdown", this, ssl, fd, settings_.timeout,
        [](SSL *ssl) -> int {
          ERR_clear_error();
          return ::SSL_shutdown(ssl);
        });
    if (err != Err::none) {
      return err;
    }
  }
  if (sys_shutdown(fd, LIBNDT_OS_SHUT_RDWR) != 0) {
    return netx_map_errno(sys_get_last_error());
  }
  return Err::none;
}

Err Client::netx_closesocket(Socket fd) noexcept {
  if ((settings_.protocol_flags & protocol_flag_tls) != 0) {
    if (fd_to_ssl_.count(fd) != 1) {
      return Err::invalid_argument;
    }
    ::SSL_free(fd_to_ssl_.at(fd));
    fd_to_ssl_.erase(fd);
  }
  if (sys_closesocket(fd) != 0) {
    return netx_map_errno(sys_get_last_error());
  }
  return Err::none;
}

// Dependencies (cURL)
// ```````````````````
}  // namespace libndt
}  // namespace measurement_kit
extern "C" {

static size_t curl_callback(char *ptr, size_t size, size_t nmemb,
                            void *userdata) {
  // Note: I have this habit of using `<= 0` rather than `== 0` even for
  // unsigned numbers because that makes the check robust when there is a
  // refactoring in which the number later becomes signed. In this case
  // it's probably a bit redundant because it's a cURL API but I still like
  // to continue to use it to avoid losing the habit. Spelling this out
  // explicitly here such that it's clear why I am doing it.
  if (nmemb <= 0) {
    return 0;  // This means "no body"
  }
  if (size > SIZE_MAX / nmemb) {
    // Note: if size is zero we end up here because we already excluded with
    // the above check the case where nmemb is zero.
    assert(false);
    return 0;
  }
  auto realsiz = size * nmemb;  // Overflow not possible (see above)
  auto ss = static_cast<std::stringstream *>(userdata);
  (*ss) << std::string{ptr, realsiz};
  // From fwrite(3): "[the return value] equals the number of bytes
  // written _only_ when `size` equals `1`".
  return nmemb;
}

}  // extern "C"
namespace measurement_kit {
namespace libndt {

void Client::CurlDeleter::operator()(CURL *handle) noexcept {
  if (handle != nullptr) {
    curl_easy_cleanup(handle);
  }
}

bool Client::curlx_get_maybe_socks5(const std::string &proxy_port,
                                    const std::string &url, long timeout,
                                    std::string *body) noexcept {
  auto handle = curlx_easy_init();
  if (!handle) {
    LIBNDT_EMIT_WARNING("curlx: cannot initialize cURL");
    return false;
  }
  if (!proxy_port.empty()) {
    std::stringstream ss;
    ss << "socks5h://127.0.0.1:" << proxy_port;
    if (curlx_setopt_proxy(handle, ss.str()) != CURLE_OK) {
      LIBNDT_EMIT_WARNING("curlx: cannot configure proxy: " << ss.str());
      return false;
    }
  }
  return curlx_get(handle, url, timeout, body);
}

bool Client::curlx_get(UniqueCurl &handle, const std::string &url, long timeout,
                       std::string *body) noexcept {
  if (body == nullptr) {
    LIBNDT_EMIT_WARNING("curlx: passed a nullptr body");
    return false;
  }
  std::stringstream ss;
  if (curlx_setopt_url(handle, url) != CURLE_OK) {
    LIBNDT_EMIT_WARNING("curlx: cannot set URL: " << url);
    return false;
  }
  if (curlx_setopt_writefunction(handle, curl_callback) != CURLE_OK) {
    LIBNDT_EMIT_WARNING("curlx: cannot set callback function");
    return false;
  }
  if (curlx_setopt_writedata(handle, &ss) != CURLE_OK) {
    LIBNDT_EMIT_WARNING("curlx: cannot set callback function context");
    return false;
  }
  if (curlx_setopt_timeout(handle, timeout) != CURLE_OK) {
    LIBNDT_EMIT_WARNING("curlx: cannot set timeout");
    return false;
  }
  if (curlx_setopt_failonerror(handle) != CURLE_OK) {
    LIBNDT_EMIT_WARNING("curlx: cannot set fail-on-error option");
    return false;
  }
  LIBNDT_EMIT_DEBUG("curlx: performing request");
  auto rv = curlx_perform(handle);
  if (rv != CURLE_OK) {
    LIBNDT_EMIT_WARNING("curlx: cURL failed: " << curl_easy_strerror(rv));
    return false;
  }
  long response_code = 0L;
  if (curlx_getinfo_response_code(handle, &response_code) != 0) {
    LIBNDT_EMIT_WARNING("curlx: cannot get the response code");
    return false;
  }
  if (response_code == 204) {
    LIBNDT_EMIT_WARNING("curlx: mlab-ns is out of capacity");
    return false;
  }
  if (response_code != 200) {
    LIBNDT_EMIT_WARNING("curlx: unexpected mlab-ns response");
    return false;
  }
  LIBNDT_EMIT_DEBUG("curlx: request complete");
  *body = ss.str();
  return true;
}

CURLcode Client::curlx_setopt_url(UniqueCurl &handle, const std::string &url) noexcept {
  assert(handle);
  return ::curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
}

CURLcode Client::curlx_setopt_proxy(UniqueCurl &handle, const std::string &url) noexcept {
  assert(handle);
  return ::curl_easy_setopt(handle.get(), CURLOPT_PROXY, url.c_str());
}

CURLcode Client::curlx_setopt_writefunction(UniqueCurl &handle, size_t (*callback)(char *ptr, size_t size, size_t nmemb, void *userdata)) noexcept {
  assert(handle);
  return ::curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, callback);
}

CURLcode Client::curlx_setopt_writedata(UniqueCurl &handle, void *pointer) noexcept {
  assert(handle);
  return ::curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, pointer);
}

CURLcode Client::curlx_setopt_timeout(UniqueCurl &handle, long timeout) noexcept {
  assert(handle);
  return ::curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, timeout);
}

CURLcode Client::curlx_setopt_failonerror(UniqueCurl &handle) noexcept {
  assert(handle);
  return ::curl_easy_setopt(handle.get(), CURLOPT_FAILONERROR, 1L);
}

CURLcode Client::curlx_perform(UniqueCurl &handle) noexcept {
  assert(handle);
  return ::curl_easy_perform(handle.get());
}

Client::UniqueCurl Client::curlx_easy_init() noexcept {
  return Client::UniqueCurl{::curl_easy_init()};
}

CURLcode Client::curlx_getinfo_response_code(
    UniqueCurl &handle, long *response_code) noexcept {
  assert(handle);
  assert(response_code);
  return ::curl_easy_getinfo(
      handle.get(), CURLINFO_RESPONSE_CODE, response_code);
}

bool Client::query_mlabns_curl(const std::string &url, long timeout,
                               std::string *body) noexcept {
  return curlx_get_maybe_socks5(settings_.socks5h_port, url, timeout, body);
}

// Other helpers
// `````````````

Verbosity Client::get_verbosity() const noexcept {
  return settings_.verbosity;
}

// Dependencies (libc)
// ```````````````````

#ifdef _WIN32
#define LIBNDT_AS_OS_BUFFER(b) ((char *)b)
#define LIBNDT_AS_OS_BUFFER_LEN(n) ((int)n)
#define LIBNDT_OS_SSIZE_MAX INT_MAX
#define LIBNDT_AS_OS_OPTION_VALUE(x) ((char *)x)
#else
#define LIBNDT_AS_OS_BUFFER(b) ((char *)b)
#define LIBNDT_AS_OS_BUFFER_LEN(n) ((size_t)n)
#define LIBNDT_OS_SSIZE_MAX SSIZE_MAX
#define LIBNDT_AS_OS_OPTION_VALUE(x) ((void *)x)
#endif

int Client::sys_get_last_error() const noexcept {
#ifdef _WIN32
  return GetLastError();
#else
  return errno;
#endif
}

void Client::sys_set_last_error(int err) const noexcept {
#ifdef _WIN32
  SetLastError(err);
#else
  errno = err;
#endif
}

int Client::sys_getaddrinfo(const char *domain, const char *port,
                            const addrinfo *hints, addrinfo **res) noexcept {
  return ::getaddrinfo(domain, port, hints, res);
}

int Client::sys_getnameinfo(const sockaddr *sa, socklen_t salen, char *host,
                            socklen_t hostlen, char *serv, socklen_t servlen,
                            int flags) noexcept {
  return ::getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
}

void Client::sys_freeaddrinfo(addrinfo *aip) noexcept { ::freeaddrinfo(aip); }

Socket Client::sys_socket(int domain, int type, int protocol) noexcept {
  return (Socket)::socket(domain, type, protocol);
}

int Client::sys_connect(Socket fd, const sockaddr *sa, socklen_t len) noexcept {
  return ::connect(fd, sa, len);
}

Ssize Client::sys_recv(Socket fd, void *base, Size count) const noexcept {
  if (count > LIBNDT_OS_SSIZE_MAX) {
    sys_set_last_error(OS_EINVAL);
    return -1;
  }
  int flags = 0;
#ifdef LIBNDT_HAVE_MSG_NOSIGNAL
  // On Linux systems this flag prevents socket ops from raising SIGPIPE.
  flags |= MSG_NOSIGNAL;
#endif
  return (Ssize)::recv(fd, LIBNDT_AS_OS_BUFFER(base), LIBNDT_AS_OS_BUFFER_LEN(count), flags);
}

Ssize Client::sys_send(Socket fd, const void *base, Size count) const noexcept {
  if (count > LIBNDT_OS_SSIZE_MAX) {
    sys_set_last_error(OS_EINVAL);
    return -1;
  }
  int flags = 0;
#ifdef LIBNDT_HAVE_MSG_NOSIGNAL
  // On Linux systems this flag prevents socket ops from raising SIGPIPE.
  flags |= MSG_NOSIGNAL;
#endif
  return (Ssize)::send(fd, LIBNDT_AS_OS_BUFFER(base), LIBNDT_AS_OS_BUFFER_LEN(count), flags);
}

int Client::sys_shutdown(Socket fd, int shutdown_how) noexcept {
  return ::shutdown(fd, shutdown_how);
}

int Client::sys_closesocket(Socket fd) noexcept {
#ifdef _WIN32
  return ::closesocket(fd);
#else
  return ::close(fd);
#endif
}

#ifdef _WIN32
int Client::sys_poll(LPWSAPOLLFD fds, ULONG nfds, INT timeout) const noexcept {
  return ::WSAPoll(fds, nfds, timeout);
}
#else
int Client::sys_poll(pollfd *fds, nfds_t nfds, int timeout) const noexcept {
  return ::poll(fds, nfds, timeout);
}
#endif

long long Client::sys_strtonum(const char *s, long long minval,
                               long long maxval, const char **errp) noexcept {
  return strtonum(s, minval, maxval, errp);
}

#ifdef _WIN32
int Client::sys_ioctlsocket(Socket s, long cmd, u_long *argp) noexcept {
  return ::ioctlsocket(s, cmd, argp);
}
#else
int Client::sys_fcntl(Socket s, int cmd) noexcept { return ::fcntl(s, cmd); }
int Client::sys_fcntl(Socket s, int cmd, int arg) noexcept {
  return ::fcntl(s, cmd, arg);
}
#endif

int Client::sys_getsockopt(Socket socket, int level, int name, void *value,
                           socklen_t *len) noexcept {
  return ::getsockopt(socket, level, name, LIBNDT_AS_OS_OPTION_VALUE(value), len);
}

#endif  // LIBNDT_NO_INLINE_IMPL
}  // namespace libndt
}  // namespace measurement_kit
#endif
