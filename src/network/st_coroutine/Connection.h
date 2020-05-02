#ifndef AFINA_NETWORK_ST_COROUTINE_CONNECTION_H
#define AFINA_NETWORK_ST_COROUTINE_CONNECTION_H

#include <cstring>
#include <queue>

#include <sys/epoll.h>

#include <afina/Storage.h>
#include <afina/coroutine/Engine.h>
#include <afina/execute/Command.h>

#include "protocol/Parser.h"

namespace Afina {
namespace Network {
namespace STcoroutine {

class Connection {
    ~Connection();

public:
    Connection(int s, std::shared_ptr<Afina::Storage> ps, std::shared_ptr<Afina::Coroutine::Engine> eng);

    inline bool isAlive() const { return is_alive; }

    enum class EventType { on_error, on_close, close, do_read, do_write, finish, none };

    void Start();

protected:
    void OnError();
    void OnClose();
    void Close();
    void DoRead();
    void DoWrite();
    void Run();
    void Unblock(Connection::EventType et);

private:
    friend class ServerImpl;

    int _socket;
    struct epoll_event _event;

    EventType event_type;
    void *coroutine;

    static constexpr size_t buf_size = 4096;
    char read_buf[buf_size], write_buf[buf_size];
    size_t read_begin, read_end, write_begin, write_end;

    std::size_t arg_remains;
    Protocol::Parser parser;
    std::string argument_for_command;
    std::unique_ptr<Execute::Command> command_to_execute;
    int readed_bytes;

    std::queue<std::string> responses;

    bool is_alive;

    std::shared_ptr<Afina::Storage> pStorage;

    std::shared_ptr<Afina::Coroutine::Engine> engine;
};

} // namespace STcoroutine
} // namespace Network
} // namespace Afina

#endif // AFINA_NETWORK_ST_COROUTINE_CONNECTION_H
