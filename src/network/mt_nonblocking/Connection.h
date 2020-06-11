#ifndef AFINA_NETWORK_MT_NONBLOCKING_CONNECTION_H
#define AFINA_NETWORK_MT_NONBLOCKING_CONNECTION_H

#include <cstring>
#include <deque>
#include <mutex>

#include <sys/epoll.h>
#include <sys/uio.h>

#include <afina/Storage.h>
#include <afina/execute/Command.h>

#include "protocol/Parser.h"

namespace Afina {
namespace Network {
namespace MTnonblock {

class Connection {
public:
    Connection(int s, std::shared_ptr<Afina::Storage> ps)
        : _socket(s), pStorage(ps), is_alive(false), is_started(false) {
        std::memset(&_event, 0, sizeof(struct epoll_event));
        _event.data.ptr = this;
    }

    inline bool isAlive() const { return is_alive; }

    void Start();

protected:
    void OnError();
    void OnClose();
    void Close();
    void DoRead();
    void DoWrite();

private:
    friend class Worker;
    friend class ServerImpl;

    int _socket;
    struct epoll_event _event;

    static constexpr size_t buf_size = 4096;
    char read_buf[buf_size];

    size_t read_begin, read_end;

    std::size_t arg_remains;
    Protocol::Parser parser;
    std::string argument_for_command;
    std::unique_ptr<Execute::Command> command_to_execute;
    int readed_bytes;

    std::deque<std::string> responses;
    size_t shift;

    bool is_alive, is_started;

    std::shared_ptr<Afina::Storage> pStorage;

    std::mutex con_mutex;
};

} // namespace MTnonblock
} // namespace Network
} // namespace Afina

#endif // AFINA_NETWORK_MT_NONBLOCKING_CONNECTION_H
