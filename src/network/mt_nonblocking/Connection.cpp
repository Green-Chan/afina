#include "Connection.h"

#include <cassert>
#include <iostream>
#include <unistd.h>

namespace Afina {
namespace Network {
namespace MTnonblock {

// See Connection.h
void Connection::Start() {
    is_alive = true;
    read_begin = read_end = 0;
    write_vec_v = 0;
    _event.events = EPOLLIN;
}

// See Connection.h
void Connection::OnError() { _event.events = 0; }

// See Connection.h
void Connection::OnClose() { _event.events = 0; }

// See Connection.h
void Connection::Close() { is_alive = false; }

// See Connection.h
void Connection::DoRead() {
    try {
        if ((readed_bytes = read(_socket, read_buf + read_end, buf_size - read_end)) > 0) {
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
                    result += "\r\n";
                    responses.push(std::move(result));
                    if (!(_event.events & EPOLLOUT)) {
                        _event.events |= EPOLLOUT;
                    }

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
        responses.push("ERROR\r\n");
        if (!(_event.events & EPOLLOUT)) {
            _event.events |= EPOLLOUT;
        }
    }
}

// See Connection.h
void Connection::DoWrite() {
    while (!responses.empty() && write_vec_v < write_vec_size) {
        written_responeses.push(std::move(responses.front()));
        responses.pop();
        write_vec[write_vec_v].iov_base = &(written_responeses.back()[0]);
        write_vec[write_vec_v].iov_len = written_responeses.back().size();
        write_vec_v++;
    }

    int writed;
    if ((writed = writev(_socket, write_vec, write_vec_v)) > 0) {
        size_t i = 0;
        while (i < write_vec_v && writed >= write_vec[i].iov_len) {
            assert(written_responeses.front().c_str() <= write_vec[i].iov_base &&
                   write_vec[i].iov_base < written_responeses.front().c_str() + written_responeses.front().size());
            written_responeses.pop();
            writed -= write_vec[i].iov_len;
            i++;
        }
        if (i < write_vec_size && writed > 0) {
            write_vec[i].iov_base = static_cast<char *>(write_vec[i].iov_base) + writed;
            write_vec[i].iov_len -= writed;
        }
        if (i > 0 && i < write_vec_v) {
            std::memmove(write_vec, &write_vec[i], (write_vec_v - i) * sizeof(write_vec[0]));
        }
        write_vec_v -= i;
    } else {
        is_alive = false;
    }

    if (responses.empty()) {
        _event.events &= ~EPOLLOUT;
    }
}

} // namespace MTnonblock
} // namespace Network
} // namespace Afina
