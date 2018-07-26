// Part of Measurement Kit <https://measurement-kit.github.io/>.
// Measurement Kit is free software under the BSD license. See AUTHORS
// and LICENSE for more information on the copying conditions.

#include "libndt.hpp"

#ifndef _WIN32
#include <arpa/inet.h>   // IWYU pragma: keep
#include <sys/select.h>  // IWYU pragma: keep
#include <sys/socket.h>
#endif

#include <assert.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#endif
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
#include <thread>
#include <utility>
#include <vector>

#ifdef HAVE_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#include "curlx.hpp"
#include "json.hpp"
#include "strtonum.h"

#if !defined _WIN32 && !defined HAVE_MSG_NOSIGNAL && !defined HAVE_SO_NOSIGPIPE
#error "No way to avoid SIGPIPE in the current thread when doing socket I/O."
#endif

namespace libndt {

// Private constants

constexpr auto max_loops = 256;

constexpr char msg_kickoff[] = "123456 654321";
constexpr size_t msg_kickoff_size = sizeof(msg_kickoff) - 1;

// Private utils

#ifdef HAVE_OPENSSL
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
#endif  // HAVE_OPENSSL

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
#ifdef HAVE_OPENSSL
  if (err == Err::ssl_generic) {
    rv += ": ";
    rv += ssl_format_error();
  }
#endif  // HAVE_OPENSSL
  //
  return rv;
}

// Generic macro for emitting logs. We lock the mutex when logging because
// some log messages are emitted by background threads. Accessing the verbosity
// is constant and verbosity does not change throughout the Client lifecycle,
// hence it is a safe thing to do. Usually you probably don't want to log like
// crazy, hence it's probably okay to use a mutex in this macro.
#define EMIT_LOG_EX(client, level, statements)             \
  do {                                                     \
    if (client->get_verbosity() >= verbosity_##level) {    \
      std::unique_lock<std::mutex> _{client->get_mutex()}; \
      std::stringstream ss;                                \
      ss << statements;                                    \
      client->on_##level(ss.str());                        \
    }                                                      \
  } while (0)

#define EMIT_WARNING_EX(clnt, stmnts) EMIT_LOG_EX(clnt, warning, stmnts)
#define EMIT_INFO_EX(clnt, stmnts) EMIT_LOG_EX(clnt, info, stmnts)
#define EMIT_DEBUG_EX(clnt, stmnts) EMIT_LOG_EX(clnt, debug, stmnts)

#define EMIT_WARNING(statements) EMIT_WARNING_EX(this, statements)
#define EMIT_INFO(statements) EMIT_INFO_EX(this, statements)
#define EMIT_DEBUG(statements) EMIT_DEBUG_EX(this, statements)

#ifdef _WIN32
#define OS_SHUT_RDWR SD_BOTH
#else
#define OS_SHUT_RDWR SHUT_RDWR
#endif

class Client::Impl {
 public:
  Socket sock = -1;
  std::vector<NettestFlags> granted_suite;
  Settings settings;
#ifdef HAVE_OPENSSL
  std::map<Socket, SSL *> fd_to_ssl;
#endif
  std::mutex mutex;
};

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
  std::random_device rd;
  std::mt19937 g(rd());
  for (size_t i = 0; i < length; ++i) {
    buffer[i] = ascii[g() % ascii.size()];
  }
}

static double compute_speed(double data, double elapsed) noexcept {
  return (elapsed > 0.0) ? ((data * 8.0) / 1000.0 / elapsed) : 0.0;
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
  std::stringstream ss{message};
  std::string line;
  while ((std::getline(ss, line, '\n'))) {
    std::vector<std::string> keyval;
    std::string token;
    std::stringstream ss{line};
    while ((std::getline(ss, token, ':'))) {
      keyval.push_back(token);
    }
    if (keyval.size() != 2) {
      EMIT_WARNING_EX(client, "incorrectly formatted summary message: " << message);
      return false;
    }
    client->on_result(scope, trim(std::move(keyval[0])),
                      trim(std::move(keyval[1])));
  }
  return true;
}

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

// Constructor and destructor

Client::Client() noexcept { impl.reset(new Client::Impl); }

Client::Client(Settings settings) noexcept : Client::Client() {
  std::swap(impl->settings, settings);
}

Client::~Client() noexcept {
  if (impl->sock != -1) {
    netx_closesocket(impl->sock);
  }
}

// Top-level API

bool Client::run() noexcept {
  std::vector<std::string> fqdns;
  if (!query_mlabns(&fqdns)) {
    return false;
  }
  for (auto &fqdn : fqdns) {
    EMIT_INFO("trying to connect to " << fqdn);
    impl->settings.hostname = fqdn;
    if (!connect()) {
      EMIT_WARNING("cannot connect to remote host; trying another one");
      continue;
    }
    EMIT_INFO("connected to remote host");
    if (!send_login()) {
      EMIT_WARNING("cannot send login; trying another host");
      continue;
    }
    EMIT_INFO("sent login message");
    if (!recv_kickoff()) {
      EMIT_WARNING("failed to receive kickoff; trying another host");
      continue;
    }
    if (!wait_in_queue()) {
      EMIT_WARNING("failed to wait in queue; trying another host");
      continue;
    }
    EMIT_INFO("authorized to run test");
    // From this point on we fail the test in case of error rather than
    // trying with another host. The rationale of trying with another host
    // above is that sometimes NDT servers are busy and we would like to
    // use another one rather than creating queue at the busy one.
    if (!recv_version()) {
      return false;
    }
    EMIT_INFO("received server version");
    if (!recv_tests_ids()) {
      return false;
    }
    EMIT_INFO("received tests ids");
    if (!run_tests()) {
      return false;
    }
    EMIT_INFO("finished running tests; now reading summary data:");
    if (!recv_results_and_logout()) {
      return false;
    }
    EMIT_INFO("received logout message");
    if (!wait_close()) {
      return false;
    }
    EMIT_INFO("connection closed");
    return true;
  }
  EMIT_WARNING("no more hosts to try; failing the test");
  return false;
}

void Client::on_warning(const std::string &msg) {
  std::clog << "[!] " << msg << std::endl;
}

void Client::on_info(const std::string &msg) { std::clog << msg << std::endl; }

void Client::on_debug(const std::string &msg) {
  std::clog << "[D] " << msg << std::endl;
}

void Client::on_performance(NettestFlags tid, uint8_t nflows,
                            double measured_bytes, double measured_interval,
                            double elapsed_time, double max_runtime) {
  auto speed = compute_speed(measured_bytes, measured_interval);
  EMIT_INFO("  [" << std::fixed << std::setprecision(0) << std::setw(2)
                  << std::right << (elapsed_time * 100.0 / max_runtime) << "%]"
                  << " elapsed: " << std::fixed << std::setprecision(3)
                  << std::setw(6) << elapsed_time << " s;"
                  << " test_id: " << (int)tid << " num_flows: " << (int)nflows
                  << " speed: " << std::setprecision(0) << std::setw(8)
                  << std::right << speed << " kbit/s");
}

void Client::on_result(std::string scope, std::string name, std::string value) {
  EMIT_INFO("  - [" << scope << "] " << name << ": " << value);
}

void Client::on_server_busy(std::string msg) {
  EMIT_WARNING("server is busy: " << msg);
}

// High-level API

bool Client::query_mlabns(std::vector<std::string> *fqdns) noexcept {
  assert(fqdns != nullptr);
  if (!impl->settings.hostname.empty()) {
    EMIT_DEBUG("no need to query mlab-ns; we have hostname");
    // When we already know the hostname that we want to use just fake out the
    // result of a mlabns query as like mlabns returned that hostname.
    fqdns->push_back(std::move(impl->settings.hostname));
    return true;
  }
  std::string mlabns_url = impl->settings.mlabns_base_url;
  if ((impl->settings.nettest_flags & nettest_flag_download_ext) != 0) {
    EMIT_WARNING("tweaking mlabns settings to allow for multi stream download");
    EMIT_WARNING("we need to use the neubot sliver and to force json since");
    EMIT_WARNING("this is the only configuration supported by neubot's sliver");
    impl->settings.protocol_flags &= ~protocol_flag_tls;
    impl->settings.protocol_flags &= ~protocol_flag_websocket;
    impl->settings.protocol_flags |= protocol_flag_json;
    mlabns_url += "/neubot";  // only botticelli implements multi stream dload
  } else {
    if ((impl->settings.protocol_flags & protocol_flag_tls) != 0) {
      mlabns_url += "/ndt_ssl";
    } else {
      mlabns_url += "/ndt";
    }
  }
  if (impl->settings.mlabns_policy == mlabns_policy_random) {
    mlabns_url += "?policy=random";
  } else if (impl->settings.mlabns_policy == mlabns_policy_geo_options) {
    mlabns_url += "?policy=geo_options";
  }
  std::string body;
  if (!query_mlabns_curl(mlabns_url, impl->settings.timeout, &body)) {
    return false;
  }
  EMIT_DEBUG("mlabns reply: " << body);
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception &exc) {
    EMIT_WARNING("cannot parse JSON: " << exc.what());
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
  for (auto &json : json) {
    std::string fqdn;
    try {
      fqdn = json.at("fqdn");
    } catch (const nlohmann::json::exception &exc) {
      EMIT_WARNING("cannot access FQDN field: " << exc.what());
      return false;
    }
    EMIT_INFO("discovered host: " << fqdn);
    fqdns->push_back(std::move(fqdn));
  }
  return true;
}

bool Client::connect() noexcept {
  std::string port;
  if (!impl->settings.port.empty()) {
    port = impl->settings.port;
  } else if ((impl->settings.protocol_flags & protocol_flag_tls) != 0) {
    port = "3010";
  } else {
    port = "3001";
  }
  // We may be called more than once when looping over the list returned by
  // geo_options. Therefore, the socket may already be open. In such case we
  // want to close it such that we don't leak resources.
  if (is_socket_valid(impl->sock)) {
    EMIT_DEBUG("closing socket openned in previous attempt");
    (void)netx_closesocket(impl->sock);
    impl->sock = (Socket)-1;
  }
  return netx_maybews_dial(  //
             impl->settings.hostname, port,
             ws_f_connection | ws_f_upgrade | ws_f_sec_ws_accept |
                 ws_f_sec_ws_protocol,
             ws_proto_control, &impl->sock) == Err::none;
}

bool Client::send_login() noexcept {
  return msg_write_login(ndt_version_compat);
}

bool Client::recv_kickoff() noexcept {
  if ((impl->settings.protocol_flags & protocol_flag_websocket) != 0) {
    EMIT_INFO("no kickoff when using websocket");
    return true;
  }
  char buf[msg_kickoff_size];
  auto err = netx_recvn(impl->sock, buf, sizeof(buf));
  if (err != Err::none) {
    EMIT_WARNING("recv_kickoff: netx_recvn() failed");
    return false;
  }
  if (memcmp(buf, msg_kickoff, sizeof(buf)) != 0) {
    EMIT_WARNING("recv_kickoff: invalid kickoff message");
    return false;
  }
  EMIT_INFO("received kickoff message");
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
  EMIT_DEBUG("server version: " << message);
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
      EMIT_WARNING("recv_tests_ids: found invalid test-id: "
                   << cur.data() << " (error: " << errstr << ")");
      return false;
    }
    impl->granted_suite.push_back(NettestFlags{tid});
  }
  return true;
}

bool Client::run_tests() noexcept {
  for (auto &tid : impl->granted_suite) {
    if (tid == nettest_flag_upload) {
      EMIT_INFO("running upload test");
      if (!run_upload()) {
        return false;
      }
    } else if (tid == nettest_flag_meta) {
      EMIT_DEBUG("running meta test");  // don't annoy the user with this
      if (!run_meta()) {
        return false;
      }
    } else if (tid == nettest_flag_download ||
               tid == nettest_flag_download_ext) {
      EMIT_INFO("running download test");
      if (!run_download()) {
        return false;
      }
    } else {
      EMIT_WARNING("run_tests(): unexpected test id");
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
      EMIT_WARNING("recv_results_and_logout: unexpected message type");
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
  EMIT_WARNING("recv_results_and_logout: too many msg_results messages");
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
  (void)netx_wait_readable(impl->sock, wait_for_close);
  (void)netx_closesocket(impl->sock);
  return true;
}

// Mid-level API

bool Client::run_download() noexcept {
  SocketVector dload_socks{this};
  std::string port;
  uint8_t nflows = 1;
  if (!msg_expect_test_prepare(&port, &nflows)) {
    return false;
  }

  for (uint8_t i = 0; i < nflows; ++i) {
    Socket sock = -1;
    // Implementation note: here connection attempts are serialized. This is
    // consistent with <https://tools.ietf.org/html/rfc6455#section-4.1>, and
    // namely with requirement 2: "If multiple connections to the same IP
    // address are attempted simultaneously, the client MUST serialize them".
    Err err = netx_maybews_dial(  //
        impl->settings.hostname, port,
        ws_f_connection | ws_f_upgrade | ws_f_sec_ws_accept
          | ws_f_sec_ws_protocol, ws_proto_s2c,
        &sock);
    if (err != Err::none) {
      break;
    }
    dload_socks.sockets.push_back(sock);
  }
  if (dload_socks.sockets.size() != nflows) {
    EMIT_WARNING("run_download: not all connect succeeded");
    return false;
  }

  if (!msg_expect_empty(msg_test_start)) {
    return false;
  }
  EMIT_DEBUG("run_download: got the test_start message");

  double client_side_speed = 0.0;
  {
    std::atomic<uint8_t> active{0};
    auto begin = std::chrono::steady_clock::now();
    std::atomic<uint64_t> recent_data{0};
    std::atomic<uint64_t> total_data{0};
    auto max_runtime = impl->settings.max_runtime;
    auto ws = (impl->settings.protocol_flags & protocol_flag_websocket) != 0;
    for (Socket fd : dload_socks.sockets) {
      active += 1;  // atomic
      auto main = [
        &active,       // reference to atomic
        begin,         // copy for safety
        fd,            // copy for safety
        max_runtime,   // copy for safety
        &recent_data,  // reference to atomic
        this,          // pointer (careful!)
        &total_data,   // reference to atomic
        ws             // copy for safety
      ]() noexcept {
        char buf[131072];
        for (;;) {
          auto err = Err::none;
          Size n = 0;
          if (ws) {
            uint8_t op = 0;
            err = ws_recvmsg(fd, &op, (uint8_t *)buf, sizeof (buf), &n);
            if (err == Err::none && op != ws_opcode_binary) {
              EMIT_WARNING("run_download: unexpected opcode: "
                           << (unsigned int)op);
              break;
            }
          } else {
            err = netx_recv(fd, buf, sizeof(buf), &n);
          }
          if (err != Err::none) {
            if (err != Err::eof) {
              EMIT_WARNING("run_download: receiving: " << libndt_perror(err));
            }
            break;
          }
          recent_data += (uint64_t)n;  // atomic
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
      constexpr int timeout_msec = 500;
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_msec));
      if (active <= 0) {
        break;
      }
      auto now = std::chrono::steady_clock::now();
      std::chrono::duration<double> measurement_interval = now - prev;
      std::chrono::duration<double> elapsed = now - begin;
      if (measurement_interval.count() > 0.25) {
        on_performance(nettest_flag_download,             //
                       active,                            // atomic
                       static_cast<double>(recent_data),  // atomic
                       measurement_interval.count(),      //
                       elapsed.count(),                   //
                       impl->settings.max_runtime);
        recent_data = 0;  // atomic
        prev = now;
      }
    }
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - begin;
    client_side_speed = compute_speed(  //
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
      EMIT_WARNING("run_download: unexpected message type");
      return false;
    }
    EMIT_DEBUG("run_download: server computed speed: " << message);
  }

  if (!msg_write(msg_test_msg, std::to_string(client_side_speed))) {
    return false;
  }

  EMIT_INFO("reading summary web100 variables");
  for (auto i = 0; i < max_loops; ++i) {  // don't loop forever
    std::string message;
    MsgType code = MsgType{0};
    if (!msg_read(&code, &message)) {
      return false;
    }
    if (code != msg_test_msg && code != msg_test_finalize) {
      EMIT_WARNING("run_download: unexpected message type");
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

  EMIT_WARNING("run_download: too many msg_test_msg messages");
  return false;  // Too many loops
}

bool Client::run_meta() noexcept {
  if (!msg_expect_empty(msg_test_prepare)) {
    return false;
  }
  if (!msg_expect_empty(msg_test_start)) {
    return false;
  }

  for (auto &kv : impl->settings.metadata) {
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
    EMIT_WARNING("run_upload: unexpected number of flows");
    return false;
  }

  {
    Socket sock = -1;
    // Remark: in case we'll even implement multi-stream here, remember that
    // websocket requires connections to be serialized. See above.
    Err err = netx_maybews_dial(  //
        impl->settings.hostname, port,
        ws_f_connection | ws_f_upgrade | ws_f_sec_ws_accept
          | ws_f_sec_ws_protocol, ws_proto_c2s,
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
    std::atomic<uint64_t> recent_data{0};
    std::atomic<uint64_t> total_data{0};
    auto max_runtime = impl->settings.max_runtime;
    auto ws = (impl->settings.protocol_flags & protocol_flag_websocket) != 0;
    for (Socket fd : upload_socks.sockets) {
      active += 1;  // atomic
      auto main = [
        &active,       // reference to atomic
        begin,         // copy for safety
        fd,            // copy for safety
        max_runtime,   // copy for safety
        &recent_data,  // reference to atomic
        this,          // pointer (careful!)
        &total_data,   // reference to atomic
        ws             // copy for safety
      ]() noexcept {
        char buf[131072];
        {
          auto begin = std::chrono::steady_clock::now();
          random_printable_fill(buf, sizeof(buf));
          auto now = std::chrono::steady_clock::now();
          std::chrono::duration<double> elapsed = now - begin;
          EMIT_DEBUG("run_upload: time to fill random buffer: "
                     << elapsed.count());
        }
        for (;;) {
          Size n = 0;
          auto err = Err::none;
          if (ws) {
            err = ws_send_frame(fd, ws_opcode_binary | ws_fin_flag,
                      (uint8_t *)buf, sizeof (buf));
            if (err == Err::none) {
              n = sizeof (buf);
            }
          } else {
            err = netx_send(fd, buf, sizeof(buf), &n);
          }
          if (err != Err::none) {
            if (err != Err::broken_pipe) {
              EMIT_WARNING("run_upload: sending: " << libndt_perror(err));
            }
            break;
          }
          recent_data += (uint64_t)n;  // atomic
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
      constexpr int timeout_msec = 500;
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_msec));
      if (active <= 0) {
        break;
      }
      auto now = std::chrono::steady_clock::now();
      std::chrono::duration<double> measurement_interval = now - prev;
      std::chrono::duration<double> elapsed = now - begin;
      if (measurement_interval.count() > 0.25) {
        on_performance(nettest_flag_upload,               //
                       active,                            // atomic
                       static_cast<double>(recent_data),  // atomic
                       measurement_interval.count(),      //
                       elapsed.count(),                   //
                       impl->settings.max_runtime);
        recent_data = 0;  // atomic
        prev = now;
      }
    }
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = now - begin;
    client_side_speed = compute_speed(  //
        static_cast<double>(total_data), elapsed.count());
    EMIT_DEBUG("run_upload: client computed speed: " << client_side_speed);
  }

  {
    std::string message;
    if (!msg_expect(msg_test_msg, &message)) {
      return false;
    }
    // TODO(bassosimone): emit this information
    EMIT_DEBUG("run_upload: server computed speed: " << message);
  }

  if (!msg_expect_empty(msg_test_finalize)) {
    return false;
  }

  return true;
}

// NDT protocol API

bool Client::msg_write_login(const std::string &version) noexcept {
  static_assert(sizeof(impl->settings.nettest_flags) == 1,
                "nettest_flags too large");
  MsgType code = MsgType{0};
  impl->settings.nettest_flags |= nettest_flag_status | nettest_flag_meta;
  if ((impl->settings.nettest_flags & nettest_flag_middlebox) !=
      NettestFlags{0}) {
    EMIT_WARNING("msg_write_login: nettest_flag_middlebox: not implemented");
    impl->settings.nettest_flags &= ~nettest_flag_middlebox;
  }
  if ((impl->settings.nettest_flags & nettest_flag_simple_firewall) !=
      NettestFlags{0}) {
    EMIT_WARNING(
        "msg_write_login: nettest_flag_simple_firewall: not implemented");
    impl->settings.nettest_flags &= ~nettest_flag_simple_firewall;
  }
  if ((impl->settings.nettest_flags & nettest_flag_upload_ext) !=
      NettestFlags{0}) {
    EMIT_WARNING("msg_write_login: nettest_flag_upload_ext: not implemented");
    impl->settings.nettest_flags &= ~nettest_flag_upload_ext;
  }
  std::string serio;
  if ((impl->settings.protocol_flags & protocol_flag_json) == 0) {
    serio = std::string{(char *)&impl->settings.nettest_flags,
                        sizeof(impl->settings.nettest_flags)};
    code = msg_login;
  } else {
    code = msg_extended_login;
    nlohmann::json msg{
        {"msg", version},
        {"tests", std::to_string((unsigned)impl->settings.nettest_flags)},
    };
    try {
      serio = msg.dump();
    } catch (nlohmann::json::exception &) {
      EMIT_WARNING("msg_write_login: cannot serialize JSON");
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
  EMIT_DEBUG("msg_write: message to send: " << represent(msg));
  if ((impl->settings.protocol_flags & protocol_flag_json) != 0) {
    nlohmann::json json;
    json["msg"] = msg;
    try {
      msg = json.dump();
    } catch (const nlohmann::json::exception &) {
      EMIT_WARNING("msg_write: cannot serialize JSON");
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
    EMIT_DEBUG("msg_write_legacy: raw message: " << represent(msg));
    EMIT_DEBUG("msg_write_legacy: message length: " << msg.size());
    char header[3];
    header[0] = code;
    if (msg.size() > UINT16_MAX) {
      EMIT_WARNING("msg_write_legacy: message too long");
      return false;
    }
    uint16_t len = (uint16_t)msg.size();
    len = htons(len);
    memcpy(&header[1], &len, sizeof(len));
    EMIT_DEBUG("msg_write_legacy: header[0] (type): " << (int)header[0]);
    EMIT_DEBUG("msg_write_legacy: header[1] (len-high): " << (int)header[1]);
    EMIT_DEBUG("msg_write_legacy: header[2] (len-low): " << (int)header[2]);
    {
      auto err = Err::none;
      if ((impl->settings.protocol_flags & protocol_flag_websocket) != 0) {
        err = ws_send_frame(
            impl->sock,
            ws_opcode_binary | ((msg.size() <= 0) ? ws_fin_flag : 0),
            (uint8_t *)header, sizeof(header));
      } else {
        err = netx_sendn(impl->sock, header, sizeof(header));
      }
      if (err != Err::none) {
        EMIT_WARNING("msg_write_legacy: cannot send NDT message header");
        return false;
      }
    }
    EMIT_DEBUG("msg_write_legacy: sent message header");
  }
  if (msg.size() <= 0) {
    EMIT_DEBUG("msg_write_legacy: zero length message");
    return true;
  }
  {
    auto err = Err::none;
    if ((impl->settings.protocol_flags & protocol_flag_websocket) != 0) {
      err = ws_send_frame(impl->sock, ws_opcode_continue | ws_fin_flag,
                          (uint8_t *)msg.data(), msg.size());
    } else {
      err = netx_sendn(impl->sock, msg.data(), msg.size());
    }
    if (err != Err::none) {
      EMIT_WARNING("msg_write_legacy: cannot send NDT message body");
      return false;
    }
  }
  EMIT_DEBUG("msg_write_legacy: sent message body");
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
    EMIT_WARNING("msg_expect_test_prepare: not enough options in vector");
    return false;
  }

  std::string port;
  {
    const char *error = nullptr;
    (void)sys_strtonum(options[0].data(), 1, UINT16_MAX, &error);
    if (error != nullptr) {
      EMIT_WARNING("msg_expect_test_prepare: cannot parse port");
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
      EMIT_WARNING("msg_expect_test_prepare: cannot parse num-flows");
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
    EMIT_WARNING("msg_expect_empty: non-empty body");
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
    EMIT_WARNING("msg_expect: unexpected message type");
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
  if ((impl->settings.protocol_flags & protocol_flag_json) == 0) {
    std::swap(s, *msg);
  } else {
    nlohmann::json json;
    try {
      json = nlohmann::json::parse(s);
    } catch (const nlohmann::json::exception &) {
      EMIT_WARNING("msg_read: cannot parse JSON");
      return false;
    }
    try {
      *msg = json.at("msg");
    } catch (const nlohmann::json::exception &) {
      EMIT_WARNING("msg_read: cannot find 'msg' field");
      return false;
    }
  }
  EMIT_DEBUG("msg_read: message: " << represent(*msg));
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
    if ((impl->settings.protocol_flags & protocol_flag_websocket) != 0) {
      uint8_t opcode = 0;
      auto err = ws_recvmsg(  //
          impl->sock, &opcode, (uint8_t *)buffer, sizeof(buffer), &ws_msg_len);
      if (err != Err::none) {
        EMIT_WARNING(
            "msg_read_legacy: cannot read NDT message using websocket");
        return false;
      }
      if (ws_msg_len < header_size) {
        EMIT_WARNING("msg_read_legacy: message too short");
        return false;
      }
      if (opcode != ws_opcode_binary) {
        EMIT_WARNING("msg_ready_legacy: unexpected opcode: "
                     << (unsigned int)opcode);
        return false;
      }
      assert(ws_msg_len <= sizeof(buffer));
    } else {
      static_assert(sizeof(buffer) >= header_size,
                    "Not enough room in buffer to read the NDT header");
      auto err = netx_recvn(impl->sock, buffer, header_size);
      if (err != Err::none) {
        EMIT_WARNING("msg_read_legacy: cannot read NDT message header");
        return false;
      }
    }
    EMIT_DEBUG("msg_read_legacy: header[0] (type): " << (int)buffer[0]);
    EMIT_DEBUG("msg_read_legacy: header[1] (len-high): " << (int)buffer[1]);
    EMIT_DEBUG("msg_read_legacy: header[2] (len-low): " << (int)buffer[2]);
    static_assert(sizeof(MsgType) == sizeof(unsigned char),
                  "Unexpected MsgType size");
    *code = MsgType{(unsigned char)buffer[0]};
    memcpy(&len, &buffer[1], sizeof(len));
    len = ntohs(len);
    if ((impl->settings.protocol_flags & protocol_flag_websocket) != 0) {
      assert(ws_msg_len >= header_size);  // Proper check above
      if (len != ws_msg_len - header_size) {
        EMIT_WARNING("msg_read_legacy: got inconsistent websocket message");
        return false;
      }
    }
    EMIT_DEBUG("msg_read_legacy: message length: " << len);
  }
  if (len <= 0) {
    EMIT_DEBUG("msg_read_legacy: zero length message");
    return true;
  }
  if ((impl->settings.protocol_flags & protocol_flag_websocket) == 0) {
    assert(sizeof(buffer) >= header_size &&
           sizeof(buffer) - header_size >= len);
    auto err = netx_recvn(impl->sock, &buffer[header_size], len);
    if (err != Err::none) {
      EMIT_WARNING("msg_read_legacy: cannot read NDT message body");
      return false;
    }
  }
  // This is a stringy copy but we do not care much because the part that needs
  // to be efficient is the one running measurements not the one where we deal
  // with incoming and outgoing NDT control messages.
  *msg = std::string{&buffer[header_size], len};
  EMIT_DEBUG("msg_read_legacy: raw message: " << represent(*msg));
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
  EMIT_DEBUG("> " << line);
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
      EMIT_DEBUG("< " << *line);
      return Err::none;
    }
    *line += ch;
  }
  EMIT_WARNING("ws_recvln: line too long");
  return Err::value_too_large;
}

Err Client::ws_handshake(Socket fd, std::string port, uint64_t ws_flags,
                         std::string ws_proto) noexcept {
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
    host_header << "Host: " << impl->settings.hostname;
    // Adding nonstandard port as specified in RFC6455 Sect. 4.1.
    if ((impl->settings.protocol_flags & protocol_flag_tls) != 0) {
      if (port != "443") {
        host_header << ":" << port;
      }
    } else {
      if (port != "80") {
        host_header << ":" << port;
      }
    }
    Err err = Err::none;
    if ((err = ws_sendln(fd, "GET /ndt_protocol HTTP/1.1")) != Err::none ||
        (err = ws_sendln(fd, host_header.str())) != Err::none ||
        (err = ws_sendln(fd, "Upgrade: websocket")) != Err::none ||
        (err = ws_sendln(fd, "Connection: Upgrade")) != Err::none ||
        (err = ws_sendln(fd, key_header)) != Err::none ||
        (err = ws_sendln(fd, proto_header)) != Err::none ||
        (err = ws_sendln(fd, "Sec-WebSocket-Version: 13")) != Err::none ||
        (err = ws_sendln(fd, "")) != Err::none) {
      EMIT_WARNING("ws_handshake: cannot send HTTP upgrade request");
      return err;
    }
  }
  EMIT_DEBUG("ws_handshake: sent HTTP/1.1 upgrade request");
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
      EMIT_WARNING("ws_handshake: unexpected response line");
      return Err::ws_proto;
    }
    uint64_t flags = 0;
    // TODO(bassosimone): use the same value used by ndt-project/ndt
    constexpr size_t max_headers = 1000;
    for (size_t i = 0; i < max_headers; ++i) {
      // TODO(bassosimone): make header processing case insensitive.
      auto err = ws_recvln(fd, &line, max_line_length);
      if (err != Err::none) {
        return err;
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
          EMIT_WARNING("ws_handshake: received incorrect handshake");
          return Err::ws_proto;
        }
        EMIT_DEBUG("ws_handshake: complete");
        return Err::none;
      }
    }
  }
  EMIT_DEBUG("ws_handshake: got too many headers");
  return Err::value_too_large;
}

Err Client::ws_send_frame(Socket sock, uint8_t first_byte, uint8_t *base,
                          Size count) noexcept {
  // TODO(bassosimone): perhaps move the RNG into Client?
  constexpr Size mask_size = 4;
  uint8_t mask[mask_size] = {};
  // "When preparing a masked frame, the client MUST pick a fresh masking
  //  key from the set of allowed 32-bit values." [RFC6455 Sect. 5.3]. Hence
  // we're not compliant (TODO(bassosimone)).
  random_printable_fill((char *)mask, sizeof(mask));
  // Message header
  {
    std::stringstream ss;
    // First byte
    {
      // TODO(bassosimone): add sanity checks for first byte
      ss << first_byte;
      EMIT_DEBUG("ws_send_frame: FIN: " << std::boolalpha
                                        << ((first_byte & ws_fin_flag) != 0));
      EMIT_DEBUG(
          "ws_send_frame: reserved: " << (first_byte & ws_reserved_mask));
      EMIT_DEBUG("ws_send_frame: opcode: " << (first_byte & ws_opcode_mask));
    }
    // Length
    {
      EMIT_DEBUG("ws_send_frame: mask flag: " << std::boolalpha << true);
      EMIT_DEBUG("ws_send_frame: length: " << count);
      // Since this is a client implementation, we always include the MASK flag
      // as part of the second byte that we send on the wire. Also, the spec
      // says that we must emit the length in network byte order, which means
      // in practice that we should use big endian.
      //
      // See <https://tools.ietf.org/html/rfc6455#section-5.1>, and
      //     <https://tools.ietf.org/html/rfc6455#section-5.2>.
#define LB(value)                                                        \
  do {                                                                   \
    EMIT_DEBUG("ws_send_frame: length byte: " << (unsigned int)(value)); \
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
        EMIT_DEBUG("ws_send_frame: mask byte: " << (unsigned int)mask[i]
                                                << " ('" << mask[i] << "')");
        ss << (uint8_t)mask[i];
      }
    }
    // Send header
    auto header = ss.str();
    EMIT_DEBUG("ws_send_frame: ws header: " << represent(header));
    auto err = netx_sendn(sock, header.c_str(), header.size());
    if (err != Err::none) {
      EMIT_WARNING("ws_send_frame: netx_sendn() failed when sending header");
      return err;
    }
  }
  EMIT_DEBUG("ws_send_frame: header sent");
  {
    if (count <= 0) {
      EMIT_DEBUG("ws_send_frame: no body provided");
      return Err::none;
    }
    if (base == nullptr && count > 0) {
      EMIT_WARNING("ws_send_frame: passed a null pointer with nonzero length");
      return Err::invalid_argument;
    }
    // Debug messages printing the body are commented out because they're too
    // much verbose. Still they may be useful for future debugging.
    /*
    EMIT_DEBUG("ws_send_frame: body unmasked: "
               << represent(std::string{(char *)base, count}));
    */
    for (Size i = 0; i < count; ++i) {
      base[i] ^= (uint8_t)mask[i % mask_size];
    }
    /*
    EMIT_DEBUG("ws_send_frame: body masked: "
               << represent(std::string{(char *)base, count}));
    */
    auto err = netx_sendn(sock, base, count);
    if (err != Err::none) {
      EMIT_WARNING("ws_send_frame: netx_sendn() failed when sending body");
      return err;
    }
  }
  EMIT_DEBUG("ws_send_frame: body sent");
  return Err::none;
}

Err Client::ws_recv_any_frame(Socket sock, uint8_t *opcode, bool *fin,
                              uint8_t *base, Size total, Size *count) noexcept {
  if (opcode == nullptr || fin == nullptr || count == nullptr) {
    EMIT_WARNING("ws_recv_any_frame: passed invalid return arguments");
    return Err::invalid_argument;
  }
  *opcode = 0;
  *fin = false;
  *count = 0;
  if (base == nullptr || total <= 0) {
    EMIT_WARNING("ws_recv_any_frame: passed invalid buffer arguments");
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
      EMIT_WARNING("ws_recv_any_frame: netx_recvn() failed for header");
      return err;
    }
    EMIT_DEBUG("ws_recv_any_frame: ws header: "
               << represent(std::string{(char *)buf, sizeof(buf)}));
    *fin = (buf[0] & ws_fin_flag) != 0;
    EMIT_DEBUG("ws_recv_any_frame: FIN: " << std::boolalpha << *fin);
    uint8_t reserved = (uint8_t)(buf[0] & ws_reserved_mask);
    if (reserved != 0) {
      // They only make sense for extensions, which we don't use. So we return
      // error. See <https://tools.ietf.org/html/rfc6455#section-5.2>.
      EMIT_WARNING("ws_recv_any_frame: invalid reserved bits: " << reserved);
      return Err::ws_proto;
    }
    *opcode = (uint8_t)(buf[0] & ws_opcode_mask);
    EMIT_DEBUG("ws_recv_any_frame: opcode: " << (unsigned int)*opcode);
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
        EMIT_WARNING("ws_recv_any_frame: invalid opcode");
        return Err::ws_proto;
    }
    auto hasmask = (buf[1] & ws_mask_flag) != 0;
    // We do not expect to receive a masked frame. This is client code and
    // the RFC says that a server MUST not mask its frames.
    //
    // See <https://tools.ietf.org/html/rfc6455#section-5.1>.
    if (hasmask) {
      EMIT_WARNING("ws_recv_any_frame: received masked frame");
      return Err::invalid_argument;
    }
    length = (buf[1] & ws_len_mask);
    switch (*opcode) {
      case ws_opcode_close:
      case ws_opcode_ping:
      case ws_opcode_pong:
        if (length > 125 || *fin == false) {
          EMIT_WARNING("ws_recv_any_frame: control messages MUST have a "
                       "payload length of 125 bytes or less and MUST NOT "
                       "be fragmented (see RFC6455 Sect 5.5.)");
          return Err::ws_proto;
        }
        break;
    }
    // As mentioned above, length is transmitted using big endian encoding.
#define AL(value)                                                            \
  do {                                                                       \
    EMIT_DEBUG("ws_recv_any_frame: length byte: " << (unsigned int)(value)); \
    length += (value);                                                       \
  } while (0)
    assert(length <= 127);  // should not happen, just in case
    if (length == 126) {
      uint8_t buf[2];
      auto err = netx_recvn(sock, buf, sizeof(buf));
      if (err != Err::none) {
        EMIT_WARNING(
            "ws_recv_any_frame: netx_recvn() failed for 16 bit length");
        return err;
      }
      EMIT_DEBUG("ws_recv_any_frame: 16 bit length: "
                 << represent(std::string{(char *)buf, sizeof(buf)}));
      length = 0;  // Need to reset the length as AL() does +=
      AL(((Size)buf[0]) << 8);
      AL((Size)buf[1]);
    } else if (length == 127) {
      uint8_t buf[8];
      auto err = netx_recvn(sock, buf, sizeof(buf));
      if (err != Err::none) {
        EMIT_WARNING(
            "ws_recv_any_frame: netx_recvn() failed for 64 bit length");
        return err;
      }
      EMIT_DEBUG("ws_recv_any_frame: 64 bit length: "
                 << represent(std::string{(char *)buf, sizeof(buf)}));
      length = 0;  // Need to reset the length as AL() does +=
      AL(((Size)buf[0]) << 56);
      if ((buf[0] & 0x80) != 0) {
        // See <https://tools.ietf.org/html/rfc6455#section-5.2>: "[...] the
        // most significant bit MUST be 0."
        EMIT_WARNING("ws_recv_any_frame: 64 bit length: invalid first bit");
        return Err::ws_proto;
      }
      AL(((Size)buf[1]) << 48);
      AL(((Size)buf[2]) << 40);
      AL(((Size)buf[3]) << 32);
      AL(((Size)buf[4]) << 24);
      AL(((Size)buf[5]) << 16);
      AL(((Size)buf[6]) << 8);
      AL(((Size)buf[7]));
    }
#undef AL  // Tidy
    if (length > total) {
      EMIT_WARNING("ws_recv_any_frame: buffer too small");
      return Err::message_size;
    }
    EMIT_DEBUG("ws_recv_any_frame: length: " << length);
  }
  EMIT_DEBUG("ws_recv_any_frame: received header");
  // Message body
  if (length > 0) {
    assert(length <= total);
    auto err = netx_recvn(sock, base, length);
    if (err != Err::none) {
      EMIT_WARNING("ws_recv_any_frame: netx_recvn() failed for body");
      return err;
    }
    // This makes the code too noisy when using -verbose. It may still be
    // useful to remove the comment when debugging.
    /*
    EMIT_DEBUG("ws_recv_any_frame: received body: "
               << represent(std::string{(char *)base, length}));
    */
    *count = length;
  } else {
    EMIT_DEBUG("ws_recv_any_frame: no body in this message");
    assert(*count == 0);
  }
  return Err::none;
}

Err Client::ws_recv_frame(Socket sock, uint8_t *opcode, bool *fin,
                          uint8_t *base, Size total, Size *count) noexcept {
  // "Control frames (see Section 5.5) MAY be injected in the middle of
  // a fragmented message.  Control frames themselves MUST NOT be fragmented."
  //    -- RFC6455 Section 5.4.
  if (opcode == nullptr || fin == nullptr || count == nullptr) {
    EMIT_WARNING("ws_recv_frame: passed invalid return arguments");
    return Err::invalid_argument;
  }
  if (base == nullptr || total <= 0) {
    EMIT_WARNING("ws_recv_frame: passed invalid buffer arguments");
    return Err::invalid_argument;
  }
  auto err = Err::none;
again:
  *opcode = 0;
  *fin = false;
  *count = 0;
  err = ws_recv_any_frame(sock, opcode, fin, base, total, count);
  if (err != Err::none) {
    EMIT_WARNING("ws_recv_frame: ws_recv_any_frame() failed");
    return err;
  }
  // "The application MUST NOT send any more data frames after sending a
  // Close frame." (RFC6455 Sect. 5.5.1). We're good as long as, for example,
  // we don't ever send a CLOSE but we just reply to CLOSE and then return
  // with an error, which will cause the connection to be closed. Note that
  // we MUST reply with CLOSE here (again Sect. 5.5.1).
  if (*opcode == ws_opcode_close) {
    EMIT_DEBUG("ws_recv_frame: received CLOSE frame; sending CLOSE back");
    // Setting the FIN flag because control messages MUST NOT be fragmented
    // as specified in Section 5.5 of RFC6455.
    (void)ws_send_frame(sock, ws_opcode_close | ws_fin_flag, nullptr, 0);
    // TODO(bassosimone): distinguish between a shutdown at the socket layer
    // and a proper shutdown implemented at the WebSocket layer.
    return Err::eof;
  }
  if (*opcode == ws_opcode_pong) {
    // RFC6455 Sect. 5.5.3 says that we must ignore a PONG.
    EMIT_DEBUG("ws_recv_frame: received PONG frame; continuing to read");
    goto again;
  }
  if (*opcode == ws_opcode_ping) {
    // TODO(bassosimone): in theory a malicious server could DoS us by sending
    // a constant stream of PING frames for a long time.
    EMIT_DEBUG("ws_recv_frame: received PING frame; PONGing back");
    assert(*count <= total);
    err = ws_send_frame(sock, ws_opcode_pong, base, *count);
    if (err != Err::none) {
      EMIT_WARNING("ws_recv_frame: ws_send_frame() failed for PONG frame");
      return err;
    }
    EMIT_DEBUG("ws_recv_frame: continuing to read after PONG");
    goto again;
  }
  return Err::none;
}

Err Client::ws_recvmsg(  //
    Socket sock, uint8_t *opcode, uint8_t *base, Size total,
    Size *count) noexcept {
  // General remark from RFC6455 Sect. 5.4: "[I]n absence of extensions, senders
  // and receivers must not depend on [...] specific frame boundaries."
  //
  // Also: "In the absence of any extension, a receiver doesn't have to buffer
  // the whole frame in order to process it." (Sect 5.4). However, currently
  // this implementation does that because we know NDT messages are "smallish"
  // not only for the control protocol but also for c2s and s2c, where in
  // general we attempt to use messages smaller than 256K.
  if (opcode == nullptr || count == nullptr) {
    EMIT_WARNING("ws_recv: passed invalid return arguments");
    return Err::invalid_argument;
  }
  if (base == nullptr || total <= 0) {
    EMIT_WARNING("ws_recv: passed invalid buffer arguments");
    return Err::invalid_argument;
  }
  bool fin = false;
  *opcode = 0;
  *count = 0;
  auto err = ws_recv_frame(sock, opcode, &fin, base, total, count);
  if (err != Err::none) {
    EMIT_WARNING("ws_recv: ws_recv_frame() failed for first frame");
    return err;
  }
  if (*opcode != ws_opcode_binary && *opcode != ws_opcode_text) {
    EMIT_WARNING("ws_recv: received unexpected opcode: " << *opcode);
    return Err::ws_proto;
  }
  if (fin) {
    EMIT_DEBUG("ws_recv: the first frame is also the last frame");
    return Err::none;
  }
  while (*count < total) {
    if ((uintptr_t)base > UINTPTR_MAX - *count) {
      EMIT_WARNING("ws_recv: avoiding pointer overflow");
      return Err::value_too_large;
    }
    uint8_t op = 0;
    Size n = 0;
    err = ws_recv_frame(sock, &op, &fin, base + *count, total - *count, &n);
    if (err != Err::none) {
      EMIT_WARNING("ws_recv: ws_recv_frame() failed for continuation frame");
      return err;
    }
    if (*count > SizeMax - n) {
      EMIT_WARNING("ws_recv: avoiding integer overflow");
      return Err::value_too_large;
    }
    *count += n;
    if (op != ws_opcode_continue) {
      EMIT_WARNING("ws_recv: received unexpected opcode: " << op);
      return Err::ws_proto;
    }
    if (fin) {
      EMIT_DEBUG("ws_recv: this is the last frame");
      return Err::none;
    }
    EMIT_DEBUG("ws_recv: this is not the last frame");
  }
  EMIT_WARNING("ws_recv: buffer smaller than incoming message");
  return Err::message_size;
}

// } - - - END WEBSOCKET IMPLEMENTATION - - -

// Networking layer

// Required by OpenSSL code below. Must be outside because we want the code
// to compile also where we don't have OpenSSL support enabled.
#ifdef _WIN32
#define OS_SET_LAST_ERROR(ec) ::SetLastError(ec)
#define OS_EINVAL WSAEINVAL
#else
#define OS_SET_LAST_ERROR(ec) errno = ec
#define OS_EINVAL EINVAL
#endif

#ifdef HAVE_OPENSSL

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
static Err map_ssl_error(Client *client, SSL *ssl, int ret) noexcept {
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
    EMIT_WARNING_EX(client, opname << " failed: " << libndt_perror(err));
  }
  return err;
}

#endif  // HAVE_OPENSSL

Err Client::netx_maybews_dial(const std::string &hostname,
                              const std::string &port, uint64_t ws_flags,
                              std::string ws_protocol, Socket *sock) noexcept {
  auto err = netx_maybessl_dial(hostname, port, sock);
  if (err != Err::none) {
    return err;
  }
  EMIT_DEBUG("netx_maybews_dial: netx_maybessl_dial() returned successfully");
  if ((impl->settings.protocol_flags & protocol_flag_websocket) == 0) {
    EMIT_DEBUG("netx_maybews_dial: websocket not enabled");
    return Err::none;
  }
  EMIT_DEBUG("netx_maybews_dial: about to start websocket handhsake");
  err = ws_handshake(*sock, port, ws_flags, ws_protocol);
  if (err != Err::none) {
    (void)netx_closesocket(*sock);
    *sock = (Socket)-1;
    return err;
  }
  EMIT_DEBUG("netx_maybews_dial: established websocket channel");
  return Err::none;
}

Err Client::netx_maybessl_dial(const std::string &hostname,
                               const std::string &port, Socket *sock) noexcept {
  auto err = netx_maybesocks5h_dial(hostname, port, sock);
  if (err != Err::none) {
    return err;
  }
  EMIT_DEBUG(
      "netx_maybessl_dial: netx_maybesocks5h_dial() returned successfully");
  if ((impl->settings.protocol_flags & protocol_flag_tls) == 0) {
    EMIT_DEBUG("netx_maybessl_dial: TLS not enabled");
    return Err::none;
  }
#ifdef HAVE_OPENSSL
  EMIT_DEBUG("netx_maybetls_dial: about to start TLS handshake");
  if (impl->settings.ca_bundle_path.empty() && impl->settings.tls_verify_peer) {
#ifndef _WIN32
    // See <https://serverfault.com/a/722646>
    std::vector<std::string> candidates{
        "/etc/ssl/cert.pem",                   // macOS
        "/etc/ssl/certs/ca-certificates.crt",  // Debian
    };
    for (auto &candidate : candidates) {
      if (access(candidate.c_str(), R_OK) == 0) {
        EMIT_DEBUG("Using '" << candidate.c_str() << "' as CA");
        impl->settings.ca_bundle_path = candidate;
        break;
      }
    }
    if (impl->settings.ca_bundle_path.empty()) {
#endif
      EMIT_WARNING(
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
      EMIT_WARNING("SSL_CTX_new() failed");
      netx_closesocket(*sock);
      return Err::ssl_generic;
    }
    EMIT_DEBUG("SSL_CTX created");
    if (impl->settings.tls_verify_peer) {
      if (!::SSL_CTX_load_verify_locations(  //
              ctx, impl->settings.ca_bundle_path.c_str(), nullptr)) {
        EMIT_WARNING("Cannot load the CA bundle path from the file system");
        ::SSL_CTX_free(ctx);
        netx_closesocket(*sock);
        return Err::ssl_generic;
      }
      EMIT_DEBUG("Loaded the CA bundle path");
    }
    ssl = ::SSL_new(ctx);
    if (ssl == nullptr) {
      EMIT_WARNING("SSL_new() failed");
      ::SSL_CTX_free(ctx);
      netx_closesocket(*sock);
      return Err::ssl_generic;
    }
    EMIT_DEBUG("SSL created");
    ::SSL_CTX_free(ctx);  // Referenced by `ssl` so safe to free here
    assert(impl->fd_to_ssl.count(*sock) == 0);
    // Implementation note: after this point `netx_closesocket(*sock)` will
    // imply that `::SSL_free(ssl)` is also called.
    impl->fd_to_ssl[*sock] = ssl;
  }
  BIO *bio = ::BIO_new(libndt_bio_method());
  if (bio == nullptr) {
    EMIT_WARNING("BIO_new() failed");
    netx_closesocket(*sock);
    //::SSL_free(ssl); // MUST NOT be called because of fd_to_ssl
    return Err::ssl_generic;
  }
  EMIT_DEBUG("libndt BIO created");
  // We use BIO_NOCLOSE because it's the socket that owns the BIO and the SSL
  // via fd_to_ssl rather than the other way around.
  ::BIO_set_fd(bio, *sock, BIO_NOCLOSE);
  // For historical reasons, if the two BIOs are equal, the SSL object will
  // increase the refcount of bio just once rather than twice.
  ::SSL_set_bio(ssl, bio, bio);
  ::BIO_set_data(bio, this);
  ::SSL_set_connect_state(ssl);
  EMIT_DEBUG("Socket added to SSL context");
  if (impl->settings.tls_verify_peer) {
    // This approach for validating the hostname should work with versions
    // of OpenSSL greater than v1.0.2 and with LibreSSL. Code taken from the
    // wiki: <https://wiki.openssl.org/index.php/Hostname_validation>.
    X509_VERIFY_PARAM *p = SSL_get0_param(ssl);
    assert(p != nullptr);
    X509_VERIFY_PARAM_set_hostflags(p, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (!::X509_VERIFY_PARAM_set1_host(p, hostname.data(), hostname.size())) {
      EMIT_WARNING("Cannot set the hostname for hostname validation");
      netx_closesocket(*sock);
      //::SSL_free(ssl); // MUST NOT be called because of fd_to_ssl
      return Err::ssl_generic;
    }
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    EMIT_DEBUG("SSL_VERIFY_PEER configured");
  }
  err = ssl_retry_unary_op("SSL_do_handshake", this, ssl, *sock,
                           impl->settings.timeout, [](SSL *ssl) -> int {
                             ERR_clear_error();
                             return ::SSL_do_handshake(ssl);
                           });
  if (err != Err::none) {
    netx_closesocket(*sock);
    //::SSL_free(ssl); // MUST NOT be called because of fd_to_ssl
    return Err::ssl_generic;
  }
  EMIT_DEBUG("SSL handshake complete");
  return Err::none;
#else
  EMIT_WARNING("SSL support not compiled in");
  return Err::function_not_supported;
#endif
}

Err Client::netx_maybesocks5h_dial(const std::string &hostname,
                                   const std::string &port,
                                   Socket *sock) noexcept {
  if (impl->settings.socks5h_port.empty()) {
    EMIT_DEBUG("socks5h: not configured, connecting directly");
    return netx_dial(hostname, port, sock);
  }
  {
    auto err = netx_dial("127.0.0.1", impl->settings.socks5h_port, sock);
    if (err != Err::none) {
      return err;
    }
  }
  EMIT_INFO("socks5h: connected to proxy");
  {
    char auth_request[] = {
        5,  // version
        1,  // number of methods
        0   // "no auth" method
    };
    auto err = netx_sendn(*sock, auth_request, sizeof(auth_request));
    if (err != Err::none) {
      EMIT_WARNING("socks5h: cannot send auth_request");
      netx_closesocket(*sock);
      *sock = -1;
      return err;
    }
    EMIT_DEBUG("socks5h: sent this auth request: "
               << represent(std::string{auth_request, sizeof(auth_request)}));
  }
  {
    char auth_response[2] = {
        0,  // version
        0   // method
    };
    auto err = netx_recvn(*sock, auth_response, sizeof(auth_response));
    if (err != Err::none) {
      EMIT_WARNING("socks5h: cannot recv auth_response");
      netx_closesocket(*sock);
      *sock = -1;
      return err;
    }
    constexpr uint8_t version = 5;
    if (auth_response[0] != version) {
      EMIT_WARNING("socks5h: received unexpected version number");
      netx_closesocket(*sock);
      *sock = -1;
      return Err::socks5h;
    }
    constexpr uint8_t auth_method = 0;
    if (auth_response[1] != auth_method) {
      EMIT_WARNING("socks5h: received unexpected auth_method");
      netx_closesocket(*sock);
      *sock = -1;
      return Err::socks5h;
    }
    EMIT_DEBUG("socks5h: authenticated with proxy; response: "
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
        EMIT_WARNING("socks5h: hostname is too long");
        netx_closesocket(*sock);
        *sock = -1;
        return Err::invalid_argument;
      }
      ss << (uint8_t)hostname.size();
      ss << hostname;
      uint16_t portno{};
      {
        const char *errstr = nullptr;
        portno = (uint16_t)sys_strtonum(port.c_str(), 0, UINT16_MAX, &errstr);
        if (errstr != nullptr) {
          EMIT_WARNING("socks5h: invalid port number: " << errstr);
          netx_closesocket(*sock);
          *sock = -1;
          return Err::invalid_argument;
        }
      }
      portno = htons(portno);
      ss << (uint8_t)((char *)&portno)[0] << (uint8_t)((char *)&portno)[1];
      connect_request = ss.str();
      EMIT_DEBUG("socks5h: connect_request: " << represent(connect_request));
    }
    auto err = netx_sendn(  //
        *sock, connect_request.data(), connect_request.size());
    if (err != Err::none) {
      EMIT_WARNING("socks5h: cannot send connect_request");
      netx_closesocket(*sock);
      *sock = -1;
      return err;
    }
    EMIT_DEBUG("socks5h: sent connect request");
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
      EMIT_WARNING("socks5h: cannot recv connect_response_hdr");
      netx_closesocket(*sock);
      *sock = -1;
      return err;
    }
    EMIT_DEBUG("socks5h: connect_response_hdr: " << represent(std::string{
                   connect_response_hdr, sizeof(connect_response_hdr)}));
    constexpr uint8_t version = 5;
    if (connect_response_hdr[0] != version) {
      EMIT_WARNING("socks5h: invalid message version");
      netx_closesocket(*sock);
      *sock = -1;
      return Err::socks5h;
    }
    if (connect_response_hdr[1] != 0) {
      // TODO(bassosimone): map the socks5 error to a system error
      EMIT_WARNING("socks5h: connect() failed: "
                   << (unsigned)(uint8_t)connect_response_hdr[1]);
      netx_closesocket(*sock);
      *sock = -1;
      return Err::io_error;
    }
    if (connect_response_hdr[2] != 0) {
      EMIT_WARNING("socks5h: invalid reserved field");
      netx_closesocket(*sock);
      *sock = -1;
      return Err::socks5h;
    }
    // receive IP or domain
    switch (connect_response_hdr[3]) {
      case 1:  // ipv4
      {
        constexpr Size expected = 4;  // ipv4
        char buf[expected];
        auto err = netx_recvn(*sock, buf, sizeof(buf));
        if (err != Err::none) {
          EMIT_WARNING("socks5h: cannot recv ipv4 address");
          netx_closesocket(*sock);
          *sock = -1;
          return err;
        }
        // TODO(bassosimone): log the ipv4 address. However tor returns a zero
        // ipv4 and so there is little added value in logging.
        break;
      }
      case 3:  // domain
      {
        uint8_t len = 0;
        auto err = netx_recvn(*sock, &len, sizeof(len));
        if (err != Err::none) {
          EMIT_WARNING("socks5h: cannot recv domain length");
          netx_closesocket(*sock);
          *sock = -1;
          return err;
        }
        char domain[UINT8_MAX + 1];  // space for final '\0'
        err = netx_recvn(*sock, domain, len);
        if (err != Err::none) {
          EMIT_WARNING("socks5h: cannot recv domain");
          netx_closesocket(*sock);
          *sock = -1;
          return err;
        }
        domain[len] = 0;
        EMIT_DEBUG("socks5h: domain: " << domain);
        break;
      }
      case 4:  // ipv6
      {
        constexpr Size expected = 16;  // ipv6
        char buf[expected];
        auto err = netx_recvn(*sock, buf, sizeof(buf));
        if (err != Err::none) {
          EMIT_WARNING("socks5h: cannot recv ipv6 address");
          netx_closesocket(*sock);
          *sock = -1;
          return err;
        }
        // TODO(bassosimone): log the ipv6 address. However tor returns a zero
        // ipv6 and so there is little added value in logging.
        break;
      }
      default:
        EMIT_WARNING("socks5h: invalid address type");
        netx_closesocket(*sock);
        *sock = -1;
        return Err::socks5h;
    }
    // receive the port
    {
      uint16_t port = 0;
      auto err = netx_recvn(*sock, &port, sizeof(port));
      if (err != Err::none) {
        EMIT_WARNING("socks5h: cannot recv port");
        netx_closesocket(*sock);
        *sock = -1;
        return err;
      }
      port = ntohs(port);
      EMIT_DEBUG("socks5h: port number: " << port);
    }
  }
  EMIT_INFO("socks5h: the proxy has successfully connected");
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
    EMIT_WARNING("netx_dial: socket already connected");
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
      EMIT_WARNING("netx_dial: unexpected getaddrinfo() failure");
      return netx_map_eai(rv);
    }
    assert(rp);
    for (auto aip = rp; (aip); aip = aip->ai_next) {
      sys_set_last_error(0);
      *sock = sys_socket(aip->ai_family, aip->ai_socktype, 0);
      if (!is_socket_valid(*sock)) {
        EMIT_WARNING("netx_dial: socket() failed");
        continue;
      }
#ifdef HAVE_SO_NOSIGPIPE
      // Implementation note: SO_NOSIGPIPE is the nonportable BSD solution to
      // avoid SIGPIPE when writing on a connection closed by the peer.
      {
        auto on = 1;
        if (::setsockopt(  //
                *sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) != 0) {
          EMIT_WARNING("netx_dial: setsockopt(..., SO_NOSIGPIPE) failed");
          sys_closesocket(*sock);
          *sock = -1;
          continue;
        }
      }
#endif  // HAVE_SO_NOSIGPIPE
      if (netx_setnonblocking(*sock, true) != Err::none) {
        EMIT_WARNING("netx_dial: netx_setnonblocking() failed");
        sys_closesocket(*sock);
        *sock = -1;
        continue;
      }
      // While on Unix ai_addrlen is socklen_t, it's size_t on Windows. Just
      // for the sake of correctness, add a check that ensures that the size has
      // a reasonable value before casting to socklen_t. My understanding is
      // that size_t is `ULONG_PTR` while socklen_t is most likely `int`.
#ifdef _WIN32
      if (aip->ai_addrlen > sizeof(sockaddr_in6)) {
        EMIT_WARNING("netx_dial: unexpected size of aip->ai_addrlen");
        sys_closesocket(*sock);
        *sock = -1;
        continue;
      }
#endif
      if (sys_connect(*sock, aip->ai_addr, (socklen_t)aip->ai_addrlen) == 0) {
        EMIT_DEBUG("netx_dial: connect(): okay immediately");
        break;
      }
      auto err = netx_map_errno(sys_get_last_error());
      if (CONNECT_IN_PROGRESS(err)) {
        err = netx_wait_writeable(*sock, impl->settings.timeout);
        if (err == Err::none) {
          int soerr = 0;
          socklen_t soerrlen = sizeof(soerr);
          if (sys_getsockopt(*sock, SOL_SOCKET, SO_ERROR, (void *)&soerr,
                             &soerrlen) == 0) {
            assert(soerrlen == sizeof(soerr));
            if (soerr == 0) {
              EMIT_DEBUG("netx_dial: connect(): okay");
              break;
            }
            sys_set_last_error(soerr);
          }
        }
      }
      EMIT_WARNING("netx_dial: connect() failed: "
                   << libndt_perror(netx_map_errno(sys_get_last_error())));
      sys_closesocket(*sock);
      *sock = -1;
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
                      Size *actual) noexcept {
  auto err = Err::none;
again:
  err = netx_recv_nonblocking(fd, base, count, actual);
  if (err == Err::none) {
    return Err::none;
  }
  if (err == Err::operation_would_block || err == Err::ssl_want_read) {
    err = netx_wait_readable(fd, impl->settings.timeout);
  } else if (err == Err::ssl_want_write) {
    err = netx_wait_writeable(fd, impl->settings.timeout);
  }
  if (err == Err::none) {
    goto again;
  }
  EMIT_WARNING(
      "netx_recv: netx_recv_nonblocking() failed: " << libndt_perror(err));
  return err;
}

Err Client::netx_recv_nonblocking(Socket fd, void *base, Size count,
                                  Size *actual) noexcept {
  assert(base != nullptr && actual != nullptr);
  *actual = 0;
  if (count <= 0) {
    EMIT_WARNING(
        "netx_recv_nonblocking: explicitly disallowing zero read; use "
        "netx_select() to check the state of a socket");
    return Err::invalid_argument;
  }
  sys_set_last_error(0);
#ifdef HAVE_OPENSSL
  if ((impl->settings.protocol_flags & protocol_flag_tls) != 0) {
    if (count > INT_MAX) {
      return Err::invalid_argument;
    }
    if (impl->fd_to_ssl.count(fd) != 1) {
      return Err::invalid_argument;
    }
    auto ssl = impl->fd_to_ssl.at(fd);
    // TODO(bassosimone): add mocks and regress tests for OpenSSL.
    ERR_clear_error();
    int ret = ::SSL_read(ssl, base, count);
    if (ret <= 0) {
      return map_ssl_error(this, ssl, ret);
    }
    *actual = (Size)ret;
    return Err::none;
  }
#endif
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

Err Client::netx_recvn(Socket fd, void *base, Size count) noexcept {
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
                      Size *actual) noexcept {
  auto err = Err::none;
again:
  err = netx_send_nonblocking(fd, base, count, actual);
  if (err == Err::none) {
    return Err::none;
  }
  if (err == Err::ssl_want_read) {
    err = netx_wait_readable(fd, impl->settings.timeout);
  } else if (err == Err::operation_would_block || err == Err::ssl_want_write) {
    err = netx_wait_writeable(fd, impl->settings.timeout);
  }
  if (err == Err::none) {
    goto again;
  }
  EMIT_WARNING(
      "netx_send: netx_send_nonblocking() failed: " << libndt_perror(err));
  return err;
}

Err Client::netx_send_nonblocking(Socket fd, const void *base, Size count,
                                  Size *actual) noexcept {
  assert(base != nullptr && actual != nullptr);
  *actual = 0;
  if (count <= 0) {
    EMIT_WARNING(
        "netx_send_nonblocking: explicitly disallowing zero send; use "
        "netx_select() to check the state of a socket");
    return Err::invalid_argument;
  }
  sys_set_last_error(0);
#ifdef HAVE_OPENSSL
  if ((impl->settings.protocol_flags & protocol_flag_tls) != 0) {
    if (count > INT_MAX) {
      return Err::invalid_argument;
    }
    if (impl->fd_to_ssl.count(fd) != 1) {
      return Err::invalid_argument;
    }
    auto ssl = impl->fd_to_ssl.at(fd);
    ERR_clear_error();
    // TODO(bassosimone): add mocks and regress tests for OpenSSL.
    int ret = ::SSL_write(ssl, base, count);
    if (ret <= 0) {
      return map_ssl_error(this, ssl, ret);
    }
    *actual = (Size)ret;
    return Err::none;
  }
#endif
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

Err Client::netx_sendn(Socket fd, const void *base, Size count) noexcept {
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
  EMIT_DEBUG("netx_resolve: " << hostname);
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
      EMIT_WARNING(
          "netx_resolve: getaddrinfo() failed: " << libndt_perror(err));
      return err;
    }
    // FALLTHROUGH
  }
  assert(rp);
  EMIT_DEBUG("netx_resolve: getaddrinfo(): okay");
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
      EMIT_WARNING("netx_resolve: unexpected size of aip->ai_addrlen");
      result = Err::value_too_large;
      break;
    }
#endif
    if (sys_getnameinfo(aip->ai_addr, (socklen_t)aip->ai_addrlen, address,
                        (socklen_t)sizeof(address), port,
                        (socklen_t)sizeof(port),
                        NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
      EMIT_WARNING("netx_resolve: unexpected getnameinfo() failure");
      result = Err::ai_generic;
      break;
    }
    addrs->push_back(address);  // we only care about address
    EMIT_DEBUG("netx_resolve: - " << address);
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

static Err netx_wait(Client *client, Socket fd, Timeout timeout,
                     short expected_events) noexcept {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events |= expected_events;
  std::vector<pollfd> pfds;
  pfds.push_back(pfd);
  static_assert(sizeof(timeout) == sizeof(int), "Unexpected Timeout size");
  if (timeout > INT_MAX / 1000) {
    timeout = INT_MAX / 1000;
  }
  auto err = client->netx_poll(&pfds, timeout * 1000);
  // Either it's success and something happened or we failed and nothing
  // must have happened on the socket. We previously checked whether we had
  // `expected_events` set however that the flags actually set by poll are
  // dependent on the system and file descriptor type. Hence it is more
  // robust to only make sure that some flag is actually set.
  //
  // See also Stack Overflow: <https://stackoverflow.com/a/25249958>.
  assert((err == Err::none && pfds[0].revents != 0) ||
         (err != Err::none && pfds[0].revents == 0));
  return err;
}

Err Client::netx_wait_readable(Socket fd, Timeout timeout) noexcept {
  return netx_wait(this, fd, timeout, POLLIN);
}

Err Client::netx_wait_writeable(Socket fd, Timeout timeout) noexcept {
  return netx_wait(this, fd, timeout, POLLOUT);
}

Err Client::netx_poll(std::vector<pollfd> *pfds, int timeout_msec) noexcept {
  if (pfds == nullptr) {
    EMIT_WARNING("netx_poll: passed a null vector of descriptors");
    return Err::invalid_argument;
  }
  for (auto &pfd : *pfds) {
    pfd.revents = 0;  // clear unconditionally
  }
  int rv = 0;
#ifndef _WIN32
again:
#endif
#ifdef _WIN64
  // When compiling for Windows 64 we have the issue that WSAPoll second
  // argument is unsigned long but pfds->size() is size_t.
  if (pfds->size() > ULONG_MAX) {
    EMIT_WARNING("netx_poll: avoiding overflow");
    return Err::value_too_large;
  }
  rv = sys_poll(pfds->data(), (unsigned long)pfds->size(), timeout_msec);
#else
  rv = sys_poll(pfds->data(), pfds->size(), timeout_msec);
#endif
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
  if (rv == 0) {
    return Err::timed_out;
  }
  return Err::none;
}

Err Client::netx_shutdown_both(Socket fd) noexcept {
#ifdef HAVE_OPENSSL
  if ((impl->settings.protocol_flags & protocol_flag_tls) != 0) {
    if (impl->fd_to_ssl.count(fd) != 1) {
      return Err::invalid_argument;
    }
    auto ssl = impl->fd_to_ssl.at(fd);
    auto err = ssl_retry_unary_op(  //
        "SSL_shutdown", this, ssl, fd, impl->settings.timeout,
        [](SSL *ssl) -> int {
          ERR_clear_error();
          return ::SSL_shutdown(ssl);
        });
    if (err != Err::none) {
      return err;
    }
  }
#endif
  if (sys_shutdown(fd, OS_SHUT_RDWR) != 0) {
    return netx_map_errno(sys_get_last_error());
  }
  return Err::none;
}

Err Client::netx_closesocket(Socket fd) noexcept {
#if HAVE_OPENSSL
  if ((impl->settings.protocol_flags & protocol_flag_tls) != 0) {
    if (impl->fd_to_ssl.count(fd) != 1) {
      return Err::invalid_argument;
    }
    ::SSL_free(impl->fd_to_ssl.at(fd));
    impl->fd_to_ssl.erase(fd);
  }
#endif
  if (sys_closesocket(fd) != 0) {
    return netx_map_errno(sys_get_last_error());
  }
  return Err::none;
}

// Dependencies (curl)

Verbosity Client::get_verbosity() const noexcept {
  return impl->settings.verbosity;
}

bool Client::query_mlabns_curl(const std::string &url, long timeout,
                               std::string *body) noexcept {
#ifdef HAVE_CURL
  Curl curl{this};
  if (!curl.method_get_maybe_socks5(  //
          impl->settings.socks5h_port, url, timeout, body)) {
    return false;
  }
  return true;
#else
  (void)url, (void)timeout, (void)body;
  EMIT_WARNING("cURL not compiled in; don't know how to get server");
  return false;
#endif
}

// Other helpers

std::mutex &Client::get_mutex() noexcept { return impl->mutex; }

// Dependencies (libc)

#ifdef _WIN32
#define AS_OS_BUFFER(b) ((char *)b)
#define AS_OS_BUFFER_LEN(n) ((int)n)
#define OS_SSIZE_MAX INT_MAX
#define AS_OS_OPTION_VALUE(x) ((char *)x)
#else
#define AS_OS_BUFFER(b) ((char *)b)
#define AS_OS_BUFFER_LEN(n) ((size_t)n)
#define OS_SSIZE_MAX SSIZE_MAX
#define AS_OS_OPTION_VALUE(x) ((void *)x)
#endif

int Client::sys_get_last_error() noexcept {
#ifdef _WIN32
  return GetLastError();
#else
  return errno;
#endif
}

void Client::sys_set_last_error(int err) noexcept {
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

Ssize Client::sys_recv(Socket fd, void *base, Size count) noexcept {
  if (count > OS_SSIZE_MAX) {
    sys_set_last_error(OS_EINVAL);
    return -1;
  }
  int flags = 0;
#ifdef HAVE_MSG_NOSIGNAL
  // On Linux systems this flag prevents socket ops from raising SIGPIPE.
  flags |= MSG_NOSIGNAL;
#endif
  return (Ssize)::recv(fd, AS_OS_BUFFER(base), AS_OS_BUFFER_LEN(count), flags);
}

Ssize Client::sys_send(Socket fd, const void *base, Size count) noexcept {
  if (count > OS_SSIZE_MAX) {
    sys_set_last_error(OS_EINVAL);
    return -1;
  }
  int flags = 0;
#ifdef HAVE_MSG_NOSIGNAL
  // On Linux systems this flag prevents socket ops from raising SIGPIPE.
  flags |= MSG_NOSIGNAL;
#endif
  return (Ssize)::send(fd, AS_OS_BUFFER(base), AS_OS_BUFFER_LEN(count), flags);
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
int Client::sys_poll(LPWSAPOLLFD fds, ULONG nfds, INT timeout) noexcept {
  return ::WSAPoll(fds, nfds, timeout);
}
#else
int Client::sys_poll(pollfd *fds, nfds_t nfds, int timeout) noexcept {
  return ::poll(fds, nfds, timeout);
}
#endif

long long Client::sys_strtonum(const char *s, long long minval,
                               long long maxval, const char **errp) noexcept {
  return ::strtonum(s, minval, maxval, errp);
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
  return ::getsockopt(socket, level, name, AS_OS_OPTION_VALUE(value), len);
}

}  // namespace libndt
