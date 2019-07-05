// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <uxr/agent/transport/tcp/TCPv4AgentWindows.hpp>
#include <uxr/agent/utils/Conversion.hpp>
#include <uxr/agent/logger/Logger.hpp>

#include <string.h>

namespace eprosima {
namespace uxr {

const uint8_t max_attemps = 16;

TCPv4Agent::TCPv4Agent(
        uint16_t agent_port,
        Middleware::Kind middleware_kind)
    : Server<IPv4EndPoint>{middleware_kind}
    , TCPServerBase{}
    , connections_{}
    , active_connections_{}
    , free_connections_{}
    , listener_poll_{}
    , poll_fds_{}
    , buffer_{0}
    , listener_thread_{}
    , running_cond_{false}
    , messages_queue_{}
#ifdef UAGENT_DISCOVERY_PROFILE
    , discovery_server_(*processor_)
#endif
{
    dds::xrce::TransportAddressMedium medium_locator;
    medium_locator.port(agent_port);
    transport_address_.medium_locator(medium_locator);
}

TCPv4Agent::~TCPv4Agent()
{
    try
    {
        stop();
    }
    catch (std::exception& e)
    {
        UXR_AGENT_LOG_CRITICAL(
            UXR_DECORATE_RED("error stopping server"),
            "exception: {}",
            e.what());
    }
}

bool TCPv4Agent::init()
{
    bool rv = false;

    /* Socket initialization. */
    poll_fds_[0].fd = socket(PF_INET, SOCK_STREAM, 0);

    /* Listener socket initialization. */
    listener_poll_.fd = socket(PF_INET, SOCK_STREAM, 0);

    if (INVALID_SOCKET != listener_poll_.fd)
    {
        /* IP and Port setup. */
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(transport_address_.medium_locator().port());
        address.sin_addr.s_addr = INADDR_ANY;
        memset(address.sin_zero, '\0', sizeof(address.sin_zero));
        if (SOCKET_ERROR != bind(listener_poll_.fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)))
        {
            /* Log. */
            UXR_AGENT_LOG_DEBUG(
                UXR_DECORATE_GREEN("port opened"),
                "port: {}",
                transport_address_.medium_locator().port());

            /* Setup listener poll. */
            listener_poll_.events = POLLIN;

            /* Setup connections. */
            for (size_t i = 0; i < poll_fds_.size(); ++i)
            {
                poll_fds_[i].fd = INVALID_SOCKET;
                poll_fds_[i].events = POLLIN;
                connections_[i].poll_fd = &poll_fds_[i];
                connections_[i].id = uint32_t(i);
                connections_[i].active = false;
                init_input_buffer(connections_[i].input_buffer);
                free_connections_.push_back(connections_[i].id);
            }

            /* Init listener. */
            if (SOCKET_ERROR != listen(listener_poll_.fd, TCP_MAX_BACKLOG_CONNECTIONS))
            {
                running_cond_ = true;
                listener_thread_ = std::thread(&TCPv4Agent::listener_loop, this);

                /* Get local address. */
                SOCKET fd = socket(PF_INET, SOCK_DGRAM, 0);
                struct sockaddr_in temp_addr;
                temp_addr.sin_family = AF_INET;
                temp_addr.sin_port = htons(80);
                temp_addr.sin_addr.s_addr = inet_addr("1.2.3.4");
                int connected = connect(fd, (struct sockaddr *)&temp_addr, sizeof(temp_addr));
                if (0 == connected)
                {
                    struct sockaddr local_addr;
                    int local_addr_len = sizeof(local_addr);
                    if (SOCKET_ERROR != getsockname(fd, &local_addr, &local_addr_len))
                    {
                        transport_address_.medium_locator().address({uint8_t(local_addr.sa_data[2]),
                                                                     uint8_t(local_addr.sa_data[3]),
                                                                     uint8_t(local_addr.sa_data[4]),
                                                                     uint8_t(local_addr.sa_data[5])});
                        rv = true;
                        UXR_AGENT_LOG_INFO(
                            UXR_DECORATE_GREEN("running..."),
                            "port: {}",
                            transport_address_.medium_locator().port());
                    }
                    closesocket(fd);
                }
            }
            else
            {
                UXR_AGENT_LOG_ERROR(
                    UXR_DECORATE_RED("listen error"),
                    "port: {}",
                    transport_address_.medium_locator().port());
            }
        }
        else
        {
            UXR_AGENT_LOG_ERROR(
                UXR_DECORATE_RED("bind error"),
                "port: {}",
                transport_address_.medium_locator().port());
        }
    }
    else
    {
        UXR_AGENT_LOG_ERROR(
            UXR_DECORATE_RED("socket error"),
            "port: {}",
            transport_address_.medium_locator().port());
    }
    return rv;
}

bool TCPv4Agent::close()
{
    /* Stop listener thread. */
    running_cond_ = false;
    if (listener_thread_.joinable())
    {
        listener_thread_.join();
    }

    /* Close listener. */
    if (INVALID_SOCKET != listener_poll_.fd)
    {
        if (0 == closesocket(listener_poll_.fd))
        {
            listener_poll_.fd = INVALID_SOCKET;
        }
    }

    /* Disconnect clients. */
    for (auto& conn : connections_)
    {
        close_connection(conn);
    }

    std::lock_guard<std::mutex> lock(connections_mtx_);

    bool rv = true;
    if ((INVALID_SOCKET == listener_poll_.fd) && (active_connections_.empty()))
    {
        UXR_AGENT_LOG_INFO(
            UXR_DECORATE_GREEN("server stopped"),
            "port: {}",
            transport_address_.medium_locator().port());
    }
    else
    {
        UXR_AGENT_LOG_ERROR(
            UXR_DECORATE_RED("socket error"),
            "port: {}",
            transport_address_.medium_locator().port());
    }
    return rv;
}

#ifdef UAGENT_DISCOVERY_PROFILE
bool TCPv4Agent::init_discovery(uint16_t discovery_port)
{
    return discovery_server_.run(discovery_port);
}

bool TCPv4Agent::close_discovery()
{
    return discovery_server_.stop();
}
#endif

bool TCPv4Agent::recv_message(
        InputPacket<IPv4EndPoint>& input_packet,
        int timeout)
{
    bool rv = true;
    if (messages_queue_.empty() && !read_message(timeout))
    {
        rv = false;
    }
    else
    {
        input_packet = std::move(messages_queue_.front());
        messages_queue_.pop();

        uint32_t raw_client_key;
        if (Server<IPv4EndPoint>::get_client_key(input_packet.source, raw_client_key))
        {
            UXR_AGENT_LOG_MESSAGE(
                UXR_DECORATE_YELLOW("[==>> TCP <<==]"),
                raw_client_key,
                input_packet.message->get_buf(),
                input_packet.message->get_len());
        }
    }
    return rv;
}

bool TCPv4Agent::send_message(
        OutputPacket<IPv4EndPoint> output_packet)
{
    bool rv = false;
    uint8_t msg_size_buf[2];

    std::unique_lock<std::mutex> lock(connections_mtx_);
    auto it = endpoint_to_connection_map_.find(output_packet.destination);
    if (it != endpoint_to_connection_map_.end())
    {
        TCPv4ConnectionWindows& connection = connections_.at(it->second);
        lock.unlock();

        msg_size_buf[0] = uint8_t(0x00FF & output_packet.message->get_len());
        msg_size_buf[1] = uint8_t((0xFF00 & output_packet.message->get_len()) >> 8);
        uint8_t n_attemps = 0;
        uint16_t bytes_sent = 0;

        /* Send message size. */
        bool size_sent = false;
        do
        {
            uint8_t errcode;
            size_t send_rv = send_data(connection, msg_size_buf, size_t(2), errcode);
            if (0 < send_rv)
            {
                bytes_sent += uint16_t(send_rv);
                size_sent = (bytes_sent == 2);
            }
            else
            {
                if (0 < errcode)
                {
                    break;
                }
            }
            ++n_attemps;
        }
        while (!size_sent && n_attemps < max_attemps);

        /* Send message payload. */
        bool payload_sent = false;
        if (size_sent)
        {
            n_attemps = 0;
            bytes_sent = 0;
            do
            {
                uint8_t errcode;
                size_t send_rv =
                        send_data(connection,
                                  (output_packet.message->get_buf() + bytes_sent),
                                  size_t(output_packet.message->get_len() - bytes_sent),
                                  errcode);
                if (0 < send_rv)
                {
                    bytes_sent += uint16_t(send_rv);
                    payload_sent = (bytes_sent == uint16_t(output_packet.message->get_len()));
                }
                else
                {
                    if (0 < errcode)
                    {
                        break;
                    }
                }
                ++n_attemps;
            }
            while (!payload_sent && n_attemps < max_attemps);
        }

        if (payload_sent)
        {
            rv = true;

            uint32_t raw_client_key;
            if (Server<IPv4EndPoint>::get_client_key(output_packet.destination, raw_client_key))
            {
                UXR_AGENT_LOG_MESSAGE(
                    UXR_DECORATE_YELLOW("[** <<TCP>> **]"),
                    raw_client_key,
                    output_packet.message->get_buf(),
                    output_packet.message->get_len());
            }
        }
        else
        {
            close_connection(connection);
        }
    }

    return rv;
}

int TCPv4Agent::get_error()
{
    return WSAGetLastError();
}

bool TCPv4Agent::open_connection(
        SOCKET fd,
        struct sockaddr_in& sockaddr)
{
    bool rv = false;
    std::lock_guard<std::mutex> lock(connections_mtx_);
    if (!free_connections_.empty())
    {
        uint32_t id = free_connections_.front();
        TCPv4ConnectionWindows& connection = connections_[id];
        connection.poll_fd->fd = fd;
        connection.endpoint = IPv4EndPoint(sockaddr.sin_addr.s_addr, sockaddr.sin_port);
        connection.active = true;
        init_input_buffer(connection.input_buffer);

        endpoint_to_connection_map_[connection.endpoint] = connection.id;
        active_connections_.insert(id);
        free_connections_.pop_front();
        rv = true;
    }
    return rv;
}

bool TCPv4Agent::close_connection(
        TCPv4ConnectionWindows& connection)
{
    bool rv = false;
    std::unique_lock<std::mutex> lock(connections_mtx_);
    auto it_conn = active_connections_.find(connection.id);
    if (it_conn != active_connections_.end())
    {
        lock.unlock();
        /* Add lock for close. */
        std::unique_lock<std::mutex> conn_lock(connection.mtx);
        if (0 == closesocket(connection.poll_fd->fd))
        {
            connection.poll_fd->fd = INVALID_SOCKET;
            connection.active = false;
            conn_lock.unlock();

            /* Clear connections map and lists. */
            lock.lock();
            endpoint_to_connection_map_.erase(connection.endpoint);
            active_connections_.erase(it_conn);
            free_connections_.push_back(connection.id);
            lock.unlock();

            rv = true;
        }
    }
    return rv;
}

void TCPv4Agent::init_input_buffer(TCPInputBuffer& buffer)
{
    buffer.state = TCP_BUFFER_EMPTY;
    buffer.msg_size = 0;
}

bool TCPv4Agent::read_message(int timeout)
{
    bool rv = false;
    int poll_rv = WSAPoll(poll_fds_.data(), ULONG(poll_fds_.size()), timeout);
    if (0 < poll_rv)
    {
        for (auto& conn : connections_)
        {
            if (0 < (POLLIN & conn.poll_fd->revents))
            {
                bool read_error;
                uint16_t bytes_read = read_data(conn, read_error);
                if (!read_error)
                {
                    if (0 < bytes_read)
                    {
                        InputPacket<IPv4EndPoint> input_packet;
                        input_packet.message.reset(new InputMessage(conn.input_buffer.buffer.data(), bytes_read));
                        input_packet.source = conn.endpoint;
                        messages_queue_.push(std::move(input_packet));
                        rv = true;
                    }
                }
                else
                {
                    close_connection(conn);
                }
            }
        }
    }
    else
    {
        if (0 == poll_rv)
        {
            WSASetLastError(WAIT_TIMEOUT);
        }
    }
    return rv;
}

void TCPv4Agent::listener_loop()
{
    while (running_cond_)
    {
        int poll_rv = WSAPoll(&listener_poll_, 1, 100);
        if (0 < poll_rv)
        {
            if (0 < (POLLIN & listener_poll_.revents))
            {
                if (connection_available())
                {
                    /* New client connection. */
                    struct sockaddr_in client_addr;
                    int client_addr_len = sizeof(client_addr);
                    SOCKET incoming_fd = accept(listener_poll_.fd,
                                                reinterpret_cast<struct sockaddr*>(&client_addr),
                                                &client_addr_len);
                    if (INVALID_SOCKET != incoming_fd)
                    {
                        /* Open connection. */
                        open_connection(incoming_fd, client_addr);
                    }
                }
            }
        }
    }
}

bool TCPv4Agent::connection_available()
{
    std::lock_guard<std::mutex> lock(connections_mtx_);
    return !free_connections_.empty();
}

size_t TCPv4Agent::recv_data(
        TCPv4ConnectionWindows& connection,
        uint8_t* buffer,
        size_t len,
        uint8_t& errcode)
{
    size_t rv = 0;
    std::lock_guard<std::mutex> lock(connection.mtx);
    if (connection.active)
    {
        int poll_rv = WSAPoll(connection.poll_fd, 1, 0);
        if (0 < poll_rv)
        {
            int bytes_received = recv(connection.poll_fd->fd, reinterpret_cast<char*>(buffer), int(len), 0);
            if (SOCKET_ERROR != bytes_received)
            {
                rv = size_t(bytes_received);
                errcode = 0;
            }
            else
            {
                errcode = 1;
            }
        }
        else
        {
            errcode = (0 == poll_rv) ? 0 : 1;
        }
    }
    return rv;
}

size_t TCPv4Agent::send_data(
        TCPv4ConnectionWindows& connection,
        uint8_t* buffer,
        size_t len,
        uint8_t& errcode)
{
    size_t rv = 0;
    std::lock_guard<std::mutex> lock(connection.mtx);
    if (connection.active)
    {
        int bytes_sent = send(connection.poll_fd->fd, reinterpret_cast<char*>(buffer), int(len), 0);
        if (SOCKET_ERROR != bytes_sent)
        {
            rv = size_t(bytes_sent);
            errcode = 0;
        }
        else
        {
            errcode = 1;
        }
    }
    return rv;
}

} // namespace uxr
} // namespace eprosima
