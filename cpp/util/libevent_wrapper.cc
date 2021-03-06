#include "util/libevent_wrapper.h"

#include <event2/thread.h>
#include <glog/logging.h>
#include <math.h>
#include <signal.h>

#include "base/time_support.h"

using std::lock_guard;
using std::mutex;
using std::shared_ptr;
using std::string;
using std::vector;

namespace {


static void Handler_ExitLoop(evutil_socket_t sig, short events, void* base) {
  event_base_loopexit((event_base*)base, NULL);
}

void SetExitLoopHandler(event_base* base, int signum) {
  struct event* signal_event;
  signal_event = evsignal_new(base, signum, Handler_ExitLoop, base);
  CHECK_NOTNULL(signal_event);
  CHECK_GE(event_add(signal_event, NULL), 0);
}


unsigned short GetPortFromUri(const evhttp_uri* uri) {
  int retval(evhttp_uri_get_port(uri));

  if (retval < 1 || retval > 65535) {
    retval = 0;

    if (!strcmp("http", CHECK_NOTNULL(evhttp_uri_get_scheme(uri)))) {
      retval = 80;
    }
  }

  return retval;
}


}  // namespace

namespace cert_trans {
namespace libevent {


struct HttpServer::Handler {
  Handler(const string& _path, const HandlerCallback& _cb)
      : path(_path), cb(_cb) {
  }

  const string path;
  const HandlerCallback cb;
};


Base::Base() : base_(event_base_new()), dns_(NULL) {
  evthread_make_base_notifiable(base_);
}


Base::~Base() {
  if (dns_)
    evdns_base_free(dns_, true);

  event_base_free(base_);
}


void Base::Dispatch() {
  SetExitLoopHandler(base_, SIGHUP);
  SetExitLoopHandler(base_, SIGINT);
  SetExitLoopHandler(base_, SIGTERM);

  CHECK_EQ(event_base_dispatch(base_), 0);
}


void Base::DispatchOnce() {
  CHECK_EQ(event_base_loop(base_, EVLOOP_ONCE), 0);
}


event* Base::EventNew(evutil_socket_t& sock, short events,
                      Event* event) const {
  return CHECK_NOTNULL(
      event_new(base_, sock, events, &Event::Dispatch, event));
}


evhttp* Base::HttpNew() const {
  return CHECK_NOTNULL(evhttp_new(base_));
}


evdns_base* Base::GetDns() {
  lock_guard<mutex> lock(dns_lock_);

  if (!dns_) {
    dns_ = CHECK_NOTNULL(evdns_base_new(base_, 1));
  }

  return dns_;
}


evhttp_connection* Base::HttpConnectionNew(const string& host,
                                           unsigned short port) {
  return CHECK_NOTNULL(
      evhttp_connection_base_new(base_, GetDns(), host.c_str(), port));
}


Event::Event(const Base& base, evutil_socket_t sock, short events,
             const Callback& cb)
    : cb_(cb), ev_(base.EventNew(sock, events, this)) {
}


Event::~Event() {
  event_free(ev_);
}


void Event::Add(double timeout) const {
  timeval tv;
  timeval* tvp(NULL);

  if (timeout >= 0) {
    tv.tv_sec = trunc(timeout);
    timeout -= tv.tv_sec;
    tv.tv_usec = timeout * kNumMicrosPerSecond;
    tvp = &tv;
  }
  CHECK_EQ(event_add(ev_, tvp), 0);
}


void Event::Dispatch(evutil_socket_t sock, short events, void* userdata) {
  static_cast<Event*>(userdata)->cb_(sock, events);
}


HttpServer::HttpServer(const Base& base) : http_(base.HttpNew()) {
}


HttpServer::~HttpServer() {
  evhttp_free(http_);
  for (std::vector<Handler*>::iterator it = handlers_.begin();
       it != handlers_.end(); ++it) {
    delete *it;
  }
}


void HttpServer::Bind(const char* address, ev_uint16_t port) {
  CHECK_EQ(evhttp_bind_socket(http_, address, port), 0);
}


bool HttpServer::AddHandler(const string& path, const HandlerCallback& cb) {
  Handler* handler(new Handler(path, cb));
  handlers_.push_back(handler);

  return evhttp_set_cb(http_, path.c_str(), &HandleRequest, handler) == 0;
}


void HttpServer::HandleRequest(evhttp_request* req, void* userdata) {
  static_cast<Handler*>(userdata)->cb(req);
}


HttpRequest::HttpRequest(const Callback& callback)
    : callback_(callback),
      req_(CHECK_NOTNULL(evhttp_request_new(&HttpRequest::Done, this))) {
}


HttpRequest::~HttpRequest() {
  // If HttpRequest::Done has been called, req_ will have been freed
  // by libevent itself.
  if (req_)
    evhttp_request_free(req_);
}


// static
void HttpRequest::Done(evhttp_request* req, void* userdata) {
  HttpRequest* const self(static_cast<HttpRequest*>(CHECK_NOTNULL(userdata)));
  CHECK_EQ(self->req_, CHECK_NOTNULL(req));

  self->callback_(self);

  // Once we return from this function, libevent will free "req_" for
  // us, and we should make ourselves disappear as well.
  self->req_ = NULL;
  delete self;
}


HttpConnection::HttpConnection(const shared_ptr<Base>& base,
                               const evhttp_uri* uri)
    : conn_(base->HttpConnectionNew(evhttp_uri_get_host(uri),
                                    GetPortFromUri(uri))) {
}


HttpConnection::~HttpConnection() {
  evhttp_connection_free(conn_);
}


void HttpConnection::MakeRequest(HttpRequest* req, evhttp_cmd_type type,
                                 const string& uri) {
  CHECK_EQ(evhttp_make_request(conn_, req->get(), type, uri.c_str()), 0);
}


}  // namespace libevent
}  // namespace cert_trans
