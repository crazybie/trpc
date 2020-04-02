#pragma once
#include "trpc.h"

#include <asio.hpp>
#include <functional>
#include <map>

namespace trpc {

using namespace std;
using namespace asio;
namespace ip = asio::ip;
using asio::ip::tcp;

template <typename... Args>
using Action = function<void(Args...)>;

//////////////////////////////////////////////////////////////////////////

class MemIStream {
 public:
  MemIStream() : m_buffer(make_shared<string>()), m_cursor(0) {}
  MemIStream(shared_ptr<string> buf) : m_buffer(buf), m_cursor(0) {}
  MemIStream(const string& s) : m_buffer(make_shared<string>(s)), m_cursor(0) {}

  void reset() {
    m_cursor = 0;
    resize(0);
  }
  void resize(size_t sz) { m_buffer->resize(sz); }
  char* data() const { return m_buffer->data(); }
  size_t getSize() const { return m_buffer->size(); }
  size_t getUnreadSize() const {
    return valid() ? m_buffer->length() - m_cursor : 0;
  }
  bool valid() const { return m_cursor < m_buffer->size(); }

  bool read(char* buf, int len) {
    auto oldPos = m_cursor;
    m_cursor += len;
    bool isValid = valid();
    m_cursor = oldPos;
    if (!isValid)
      return false;
    memcpy(buf, data() + m_cursor, len);
    m_cursor += len;
    return true;
  }

  bool skip(int len) {
    m_cursor += len;
    return valid();
  }

  template <typename T>
  bool operator>>(vector<T>& o) {
    int cnt;
    if (!(*this >> cnt))
      return false;
    o.resize(cnt);
    for (int i = 0; i < cnt; i++) {
      if (!(*this >> o[i]))
        return false;
    }
    return true;
  }

  template <typename K, typename V>
  bool operator>>(map<K, V>& o) {
    int cnt;
    if (!(*this >> cnt))
      return false;
    for (int i = 0; i < cnt; i++) {
      K key;
      V val;
      if (!(*this >> key) || !(*this >> val))
        return false;
      o[key] = val;
    }
    return true;
  }

  template <typename T>
  typename enable_if<!is_class<T>::value, bool>::type operator>>(T& o) {
    if (!valid())
      return false;
    o = *(T*)(data() + m_cursor);  // TODO: byte order
    m_cursor += sizeof(o);
    return true;
  }

  bool operator>>(string& o) {
    if (!valid())
      return false;
    size_t len;
    if (!(*this >> len))
      return false;
    o.assign(data() + m_cursor, len);
    m_cursor += len;
    return valid();
  }

 private:
  shared_ptr<string> m_buffer;
  size_t m_cursor;
};

//////////////////////////////////////////////////////////////////////////

class MemOStream {
 public:
  MemOStream(shared_ptr<string> buf) : m_buffer(buf), m_offset(0) {}
  MemOStream() : m_buffer(make_shared<string>()), m_offset(0) {}

  const char* data() const { return m_buffer->data(); }
  size_t getSize() const { return m_offset; }
  void reset() { m_offset = 0; }

  int write(const char* buf, int len) {
    if (m_offset >= m_buffer->length()) {
      m_buffer->append(buf, len);
    } else {
      m_buffer->replace(m_offset, len, buf, len);
    }
    m_offset += len;
    return len;
  }

  template <typename T>
  bool operator<<(const vector<T>& o) {
    *this << o.size();
    for (size_t i = 0; i < o.size(); i++) {
      *this << o[i];
    }
    return true;
  }

  template <typename K, typename V>
  bool operator<<(const map<K, V>& o) {
    *this << o.size();
    for (auto i = o.begin(); i != o.end(); ++i) {
      if (!(*this << i->first))
        return false;
      if (!(*this << i->second))
        return false;
    }
    return true;
  }

  template <typename T>
  typename enable_if<!is_class<T>::value, bool>::type operator<<(const T& o) {
    // TODO: byte order
    write((const char*)&o, sizeof(o));
    return true;
  }

  bool operator<<(const string& o) {
    *this << o.size();
    write(o.data(), o.size());
    return true;
  }

 private:
  shared_ptr<string> m_buffer;
  size_t m_offset;
};

//////////////////////////////////////////////////////////////////////////

class AsioPeer {
 public:
  virtual ~AsioPeer() {}
  virtual tcp::socket* getSocket() = 0;
  virtual void onError(const error_code& err) {
    printf("error: %s\n", err.message().c_str());
  }

  void send(MemOStream& body) {
    auto head = make_shared<MemOStream>();
    *head << (decltype(packageSize))body.getSize();
    std::array<const_buffer, 2> data{
        asio::buffer(head->data(), head->getSize()),
        asio::buffer(body.data(), body.getSize()),
    };
    body.reset();
    async_write(*getSocket(), data,
                [this, head](const error_code& err, int len) {
                  if (err) {
                    onError(err);
                    return;
                  }
                });
  }

  void receive(const Action<MemIStream&>& onReceived) {
    getSocket()->async_receive(
        buffer(inputBuffer),
        [this, onReceived](const error_code& err, int len) {
          if (err) {
            onError(err);
            return;
          }

          auto oldSize = input.getSize();
          input.resize(oldSize + len);
          memcpy(input.data() + oldSize, inputBuffer, len);

          while (input.valid()) {
            if (packageSize) {
              if (input.getUnreadSize() >= packageSize) {
                onReceived(input);
                packageSize = 0;
              } else {
                break;
              }
            } else {
              if (input.getUnreadSize() >= sizeof(packageSize)) {
                input >> packageSize;
              } else {
                break;
              }
            }
          }
          if (!input.valid())
            input.reset();

          receive(onReceived);
        });
  }

 protected:
  MemIStream input;
  char inputBuffer[1024 * 4];
  size_t packageSize = 0;
};

class AsioClient : public RpcClient<MemIStream, MemOStream>, public AsioPeer {
 public:
  AsioClient() : RpcClient(output) {}

  void connect(string host, int port, Action<bool> cb) {
    tcp::endpoint ep(ip::address_v4::from_string(host), port);
    sock.async_connect(ep, [=](const error_code& err) {
      if (err) {
        cb(!err);
        return;
      }
      cb(true);
      receive([this](MemIStream& in) { onReceive(in); });
    });

    flush = [this] { send(output); };
  }

  tcp::socket* getSocket() override { return &sock; }
  void update() { ctx.poll(); }

 private:
  asio::io_context ctx;
  tcp::socket sock{ctx};
  MemOStream output;
};

//////////////////////////////////////////////////////////////////////////

class AsioServer : public RpcServer<MemIStream, MemOStream> {
 public:
  void start(int port, Action<bool> cb) {
    acc = make_unique<tcp::acceptor>(ctx, tcp::endpoint(tcp::v4(), port));
    acc->set_option(tcp::acceptor::reuse_address(true));
    cb(true);

    auto sock = make_shared<tcp::socket>(ctx);
    acc->async_accept(*sock, [this, sock](const error_code& err) {
      auto& s = sessions[sessionID];
      s.sock = sock;
      s.sid = sessionID;
      s.server = this;
      sessionID++;
      addSession(s.sid, s.os);
      s.receive([this, sid = s.sid](MemIStream& in) { onReceive(sid, in); });

      flush = [this](SessionID sid) {
        if (sessions.find(sid) == sessions.end())
          return;
        auto& s = sessions[sid];
        s.send(s.os);
      };
    });
  }

  struct Session : AsioPeer {
    SessionID sid;
    shared_ptr<tcp::socket> sock;
    MemOStream os;
    AsioServer* server;

    tcp::socket* getSocket() override { return sock.get(); }
    void onError(const error_code& err) override { server->onError(err, this); }
  };

  void onError(const error_code& err, Session* s) {
    printf("%s\n", err.message().c_str());
  }
  void update() { ctx.poll(); }

 private:
  asio::io_context ctx;
  map<SessionID, Session> sessions;
  SessionID sessionID = 100;
  unique_ptr<tcp::acceptor> acc;
};

template <typename Handler>
using AsioRpcHandler = RpcHandler<Handler, MemIStream, MemOStream>;

}  // namespace trpc