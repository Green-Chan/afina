#include "ServerImpl.h"

#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/logger.h>

#include <afina/Storage.h>
#include <afina/logging/Service.h>

#include "Connection.h"
#include "Utils.h"

namespace Afina {
namespace Network {
namespace STcoroutine {

// See Server.h
ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps, std::shared_ptr<Logging::Service> pl) : Server(ps, pl) {
    // engine = std::make_shared<Afina::Coroutine::Engine>(&ServerImpl::Unblock);
    engine = std::make_shared<Afina::Coroutine::Engine>(static_cast<std::function<void(Afina::Coroutine::Engine &)>>(
        std::bind(&ServerImpl::Unblock, this, std::placeholders::_1)));
}

// See Server.h
ServerImpl::~ServerImpl() {}

// See Server.h
void ServerImpl::Start(uint16_t port, uint32_t n_acceptors, uint32_t n_workers) {
    _logger = pLogging->select("network");
    _logger->info("Start st_coroutine network service");

    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, NULL) != 0) {
        throw std::runtime_error("Unable to mask SIGPIPE");
    }

    // Create server socket
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_port = htons(port);       // TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any address

    _server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_server_socket == -1) {
        throw std::runtime_error("Failed to open socket: " + std::string(strerror(errno)));
    }

    int opts = 1;
    if (setsockopt(_server_socket, SOL_SOCKET, (SO_KEEPALIVE), &opts, sizeof(opts)) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket setsockopt() failed: " + std::string(strerror(errno)));
    }

    if (bind(_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket bind() failed: " + std::string(strerror(errno)));
    }

    make_socket_non_blocking(_server_socket);
    if (listen(_server_socket, 5) == -1) {
        close(_server_socket);
        throw std::runtime_error("Socket listen() failed: " + std::string(strerror(errno)));
    }

    _event_fd = eventfd(0, EFD_NONBLOCK);
    if (_event_fd == -1) {
        throw std::runtime_error("Failed to create epoll file descriptor: " + std::string(strerror(errno)));
    }

    _work_thread = std::thread(&ServerImpl::OnRun, this);
}

// See Server.h
void ServerImpl::Stop() {
    _logger->warn("Stop network service");

    // Wakeup threads that are sleep on epoll_wait
    if (eventfd_write(_event_fd, 1)) {
        throw std::runtime_error("Failed to wakeup workers");
    }
}

// See Server.h
void ServerImpl::Unblock(Afina::Coroutine::Engine &eng) {
    static bool run = true;
    static std::unordered_map<Connection *, uint32_t> old_connections{};

    for (auto it = old_connections.begin(); it != old_connections.end(); it++) {
        auto pc = it->first;
        // Does it alive?
        if (!pc->isAlive()) {
            if (epoll_ctl(epoll_descr, EPOLL_CTL_DEL, pc->_socket, &pc->_event)) {
                _logger->error("Failed to delete connection from epoll");
            }

            close(pc->_socket);
            pc->Unblock(Connection::EventType::on_close);
        } else if (pc->_event.events != it->second) {
            if (epoll_ctl(epoll_descr, EPOLL_CTL_MOD, pc->_socket, &pc->_event)) {
                _logger->error("Failed to change connection event mask");

                close(pc->_socket);
                pc->Unblock(Connection::EventType::on_close);
            }
        }
    }
    old_connections.clear();

    std::array<struct epoll_event, 64> mod_list;
    if (run) {
        int nmod = epoll_wait(epoll_descr, &mod_list[0], mod_list.size(), -1);
        _logger->debug("Acceptor wokeup: {} events", nmod);

        for (int i = 0; i < nmod; i++) {
            struct epoll_event &current_event = mod_list[i];
            if (current_event.data.fd == _event_fd) {
                _logger->debug("Break acceptor due to stop signal");
                run = false;
                continue;
            } else if (current_event.data.fd == _server_socket) {
                OnNewConnection(epoll_descr);
                continue;
            }

            // That is some connection!
            Connection *pc = static_cast<Connection *>(current_event.data.ptr);

            if ((current_event.events & EPOLLERR) || (current_event.events & EPOLLHUP)) {
                _logger->error("Error on socket {}", pc->_socket);
                if (epoll_ctl(epoll_descr, EPOLL_CTL_DEL, pc->_socket, &pc->_event)) {
                    _logger->error("Failed to delete connection from epoll");
                }

                close(pc->_socket);
                pc->Unblock(Connection::EventType::on_error);
                continue;
            } else {
                old_connections[pc] = pc->_event.events;
                if (current_event.events & EPOLLRDHUP) {
                    _logger->debug("Epollhub");
                    pc->Unblock(Connection::EventType::close);
                } else {
                    // Depends on what connection wants...
                    if (current_event.events & EPOLLIN) {
                        pc->Unblock(Connection::EventType::do_read);
                    }
                    if (current_event.events & EPOLLOUT) {
                        pc->Unblock(Connection::EventType::do_write);
                    }
                }
            }
        }
    } else {
        _logger->warn("Acceptor stopped");
    }
}

// See Server.h
void ServerImpl::Join() {
    // Wait for work to be complete
    _work_thread.join();
}

// See ServerImpl.h
void ServerImpl::OnRun() {
    engine->start(&ServerImpl::Run, this);
    // engine->start(static_cast<void (*)(ServerImpl *)>([](ServerImpl *s) -> void { s->Run(); }), this);
}

// See ServerImpl.h
void ServerImpl::Run() {
    _logger->info("Start acceptor");
    epoll_descr = epoll_create1(0);
    if (epoll_descr == -1) {
        throw std::runtime_error("Failed to create epoll file descriptor: " + std::string(strerror(errno)));
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = _server_socket;
    if (epoll_ctl(epoll_descr, EPOLL_CTL_ADD, _server_socket, &event)) {
        throw std::runtime_error("Failed to add file descriptor to epoll");
    }

    struct epoll_event event2;
    event2.events = EPOLLIN;
    event2.data.fd = _event_fd;
    if (epoll_ctl(epoll_descr, EPOLL_CTL_ADD, _event_fd, &event2)) {
        throw std::runtime_error("Failed to add file descriptor to epoll");
    }
}

void ServerImpl::OnNewConnection(int epoll_descr) {
    for (;;) {
        struct sockaddr in_addr;
        socklen_t in_len;

        // No need to make these sockets non blocking since accept4() takes care of it.
        in_len = sizeof in_addr;
        int infd = accept4(_server_socket, &in_addr, &in_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (infd == -1) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                break; // We have processed all incoming connections.
            } else {
                _logger->error("Failed to accept socket");
                break;
            }
        }

        // Print host and service info.
        char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
        int retval =
            getnameinfo(&in_addr, in_len, hbuf, sizeof hbuf, sbuf, sizeof sbuf, NI_NUMERICHOST | NI_NUMERICSERV);
        if (retval == 0) {
            _logger->info("Accepted connection on descriptor {} (host={}, port={})\n", infd, hbuf, sbuf);
        }

        // Register the new FD to be monitored by epoll.
        Connection *pc = new (std::nothrow) Connection(infd, pStorage, engine);
        if (pc == nullptr) {
            throw std::runtime_error("Failed to allocate connection");
        }

        // Register connection in worker's epoll
        pc->Start();
        if (pc->isAlive()) {
            if (epoll_ctl(epoll_descr, EPOLL_CTL_ADD, pc->_socket, &pc->_event)) {
                pc->Unblock(Connection::EventType::on_error);
            }
        }
    }
}

} // namespace STcoroutine
} // namespace Network
} // namespace Afina