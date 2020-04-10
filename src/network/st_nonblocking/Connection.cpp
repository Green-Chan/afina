#include "Connection.h"

#include <iostream>
#include <unistd.h>

namespace Afina {
namespace Network {
namespace STnonblock {

// See Connection.h
void Connection::Start() {
    std::cout << "Start" << std::endl;
    is_alive = true;
    read_begin = read_end = write_begin = write_end = 0;
    _event.events = EPOLLIN;
}

// See Connection.h
void Connection::OnError() {
    std::cout << "OnError" << std::endl;
    _event.events = 0;
}

// See Connection.h
void Connection::OnClose() {
    std::cout << "OnClose" << std::endl;
    _event.events = 0;
}

// See Connection.h
void Connection::Close() {
    std::cout << "Close" << std::endl;
    is_alive = false;
}

// See Connection.h
void Connection::DoRead() {
    std::cout << "DoRead" << std::endl;
    try {
        if ((readed_bytes = read(_socket, read_buf + read_end, buf_size - read_end)) > 0) {
            std::cout << readed_bytes << std::endl;
            read_end += readed_bytes;
            while (read_end - read_begin > 0) {
                // There is no command yet
                if (!command_to_execute) {
                    std::size_t parsed = 0;
                    if (parser.Parse(read_buf + read_begin, read_end - read_begin, parsed)) {
                        // There is no command to be launched, continue to parse input stream
                        // Here we are, current chunk finished some command, process it
                        command_to_execute = parser.Build(arg_remains);
                        if (arg_remains > 0) {
                            arg_remains += 2;
                        }
                    }

                    // Parsed might fails to consume any bytes from input stream. In real life that could happens,
                    // for example, because we are working with UTF-16 chars and only 1 byte left in stream
                    if (parsed == 0) {
                        break;
                    } else {
                        read_begin += parsed;
                    }
                }
                // There is command, but we still wait for argument to arrive...
                if (command_to_execute && arg_remains > 0) {
                    // There is some parsed command, and now we are reading argument
                    std::size_t to_read = std::min(arg_remains, read_end - read_begin);
                    argument_for_command.append(read_buf + read_begin, to_read);

                    arg_remains -= to_read;
                    read_begin += to_read;
                }
                // There are command & argument - RUN!
                if (command_to_execute && arg_remains == 0) {
                    std::string result;
                    if (argument_for_command.size()) {
                        argument_for_command.resize(argument_for_command.size() - 2);
                    }
                    command_to_execute->Execute(*pStorage, argument_for_command, result);

                    // Put response in the queue
                    responses.push(result);
                    _event.events |= EPOLLOUT;

                    // Prepare for the next command
                    command_to_execute.reset();
                    argument_for_command.resize(0);
                    parser.Reset();
                }
            }
            if (read_begin == read_end) {
                read_begin = read_end = 0;
            } else if (read_end == buf_size) {
                std::memmove(read_buf, read_buf + read_begin, read_end - read_begin);
            }
        } else {
            is_alive = false;
        }
    } catch (std::runtime_error &ex) {
        responses.push("ERROR");
        _event.events |= EPOLLOUT;
    }
}

// See Connection.h
void Connection::DoWrite() {
    std::cout << "DoWrite" << std::endl;
    while (!responses.empty() && buf_size - write_end >= responses.front().size() + 2) {
        std::memmove(write_buf + write_end, responses.front().data(), responses.front().size());
        write_end += responses.front().size() + 2;
        write_buf[write_end - 2] = '\r';
        write_buf[write_end - 1] = '\n';
        responses.pop();
    }
    int writed;
    if ((writed = write(_socket, write_buf + write_begin, write_end - write_begin)) > 0) {
        write_begin += writed;
        if (write_begin == write_end) {
            write_begin = write_end = 0;
            if (responses.empty()) {
                _event.events &= ~EPOLLOUT;
            }
        }
    } else {
        is_alive = false;
    }
}

} // namespace STnonblock
} // namespace Network
} // namespace Afina
