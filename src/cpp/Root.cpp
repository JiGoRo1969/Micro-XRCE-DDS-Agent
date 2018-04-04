// Copyright 2017 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

#include <agent/Root.h>

#include <libdev/MessageDebugger.h>
#include <libdev/MessageOutput.h>
#include <fastcdr/Cdr.h>
#include <memory>

#ifdef WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace eprosima {
namespace micrortps {

Agent* root()
{
    static Agent xrce_agent;
    return &xrce_agent;
}

Agent::Agent() :
    locator_{},
    input_buffer_{},
    response_thread_{},
    reply_cond_()
{
    running_ = false;
    reply_cond_ = false;
}

void Agent::init(const std::string& device)
{
    std::cout << "Serial agent initialization..." << std::endl;
    locator_id_t id = add_serial_locator(device.data(), &locator_);
    if (id != MICRORTPS_TRANSPORT_OK)
    {
        std::cout << "Agent::init() -> error" << std::endl;
    }
}

void Agent::init(const uint16_t local_port)
{
    std::cout << "UDP agent initialization..." << std::endl;
    locator_id_t id = add_udp_locator_agent(local_port, &locator_);
    if (id == MICRORTPS_TRANSPORT_ERROR)
    {
        std::cout << "Agent::init() -> error" << std::endl;
    }
}

dds::xrce::ResultStatus Agent::create_client(const dds::xrce::CREATE_CLIENT_Payload& payload)
{
    dds::xrce::ResultStatus result_status;
    result_status.status(dds::xrce::STATUS_OK);

    if (payload.client_representation().xrce_cookie() == dds::xrce::XrceCookie{XRCE_COOKIE})
    {
        if (payload.client_representation().xrce_version()[0] == XRCE_VERSION_MAJOR)
        {
            std::lock_guard<std::mutex> lock(clientsmtx_);

            bool create_result;
            dds::xrce::ClientKey client_key = payload.client_representation().client_key();
            dds::xrce::SessionId session_id = payload.client_representation().session_id();
            auto it = clients_.find(client_key);
            if (it == clients_.end())
            {
                create_result = clients_.emplace(std::piecewise_construct,
                                                 std::forward_as_tuple(client_key),
                                                 std::forward_as_tuple(payload.client_representation(),
                                                                       client_key, session_id)).second;
                if (!create_result)
                {
                    result_status.status(dds::xrce::STATUS_ERR_RESOURCES);
                }
            }
            else
            {
                ProxyClient& client = clients_.at(client_key);
                if (session_id != client.get_session_id())
                {
                    clients_.erase(it);
                    create_result = clients_.emplace(std::piecewise_construct,
                                                     std::forward_as_tuple(client_key),
                                                     std::forward_as_tuple(payload.client_representation(),
                                                                           client_key, session_id)).second;
                    if (!create_result)
                    {
                        result_status.status(dds::xrce::STATUS_ERR_RESOURCES);
                    }
                }
            }
        }
        else
        {
            result_status.status(dds::xrce::STATUS_ERR_INCOMPATIBLE);
        }
    }
    else
    {
        result_status.status(dds::xrce::STATUS_ERR_INVALID_DATA);
    }
    return result_status;
}

dds::xrce::ResultStatus Agent::delete_client(dds::xrce::ClientKey client_key)
{
    dds::xrce::ResultStatus result_status;
    std::lock_guard<std::mutex> lock(clientsmtx_);
    if (0 ==clients_.erase(client_key))
    {
        result_status.status(dds::xrce::STATUS_ERR_INVALID_DATA);
    }
    else
    {
        result_status.status(dds::xrce::STATUS_OK);
    }
    return result_status;
}

void Agent::run()
{
    std::cout << "Running DDS-XRCE Agent..." << std::endl;
    int ret = 0;
    running_ = true;
    while(running_)
    {
        if (0 < (ret = receive_data(static_cast<octet*>(input_buffer_), buffer_len_, locator_.locator_id)))
        {
            dds::xrce::XrceMessage input_message = {reinterpret_cast<char*>(input_buffer_), static_cast<size_t>(ret)};
            handle_input_message(input_message);
        }
        #ifdef WIN32
            Sleep(10);
        #else
            usleep(10000);
        #endif

    };
    std::cout << "Execution stopped" << std::endl;

}

void Agent::stop()
{
    running_ = false;
}

void Agent::abort_execution()
{
    reply_cond_ = false;
    messages_.abort();
    if (response_thread_ && response_thread_->joinable())
        response_thread_->join();
}

void Agent::add_reply(const Message& message)
{
    messages_.push(message);
    if(response_thread_ == nullptr)
    {
        reply_cond_ = true;
        response_thread_.reset(new std::thread(std::bind(&Agent::reply, this)));
    }
}


void Agent::add_reply(const dds::xrce::MessageHeader& header,
                      const dds::xrce::STATUS_Payload& status_reply)
{
#ifdef VERBOSE_OUTPUT
    std::cout << "<== ";
    eprosima::micrortps::debug::printl_status_submessage(status_reply);
#endif

    Message message{};
    XRCEFactory message_creator{message.get_buffer().data(), message.get_buffer().max_size() };
    message_creator.header(header);
    message_creator.status(status_reply);
    message.set_real_size(message_creator.get_total_size());
    add_reply(message);
}

void Agent::reply()
{
    while(reply_cond_)
    {
        Message message = messages_.pop();
        if (!message.get_buffer().empty())
        {
            int ret = 0;
            if (0 < (ret = send_data(reinterpret_cast<octet*>(message.get_buffer().data()), message.get_real_size(),
                     locator_.locator_id)))
            {
                printf("%d bytes response sent\n", ret);
            }
        }
    }
}

eprosima::micrortps::ProxyClient* Agent::get_client(dds::xrce::ClientKey client_key)
{
    try
    {
        std::lock_guard<std::mutex> lock(clientsmtx_);
        return &clients_.at(client_key);
    }
    catch (const std::out_of_range& e)
    {
        unsigned int key = client_key[0] + (client_key[1] << 8) + (client_key[2] << 16) + (client_key[3] << 24);
        std::cerr << "Client 0x" << std::hex << key << " not found" << std::endl;
        return nullptr;
    }
}

void Agent::handle_input_message(const dds::xrce::XrceMessage& input_message)
{
    Serializer deserializer(input_message.buf, input_message.len);

    dds::xrce::MessageHeader header;
    if (deserializer.deserialize(header))
    {
        /* Create client message. */
        if ((header.session_id() == dds::xrce::SESSIONID_NONE_WITHOUT_CLIENT_KEY)
                || (header.session_id() == dds::xrce::SESSIONID_NONE_WITH_CLIENT_KEY))
        {
            dds::xrce::SubmessageHeader sub_header;
            deserializer.force_new_submessage_align();
            if (deserializer.deserialize(sub_header))
            {
                if (sub_header.submessage_id() == dds::xrce::CREATE_CLIENT)
                {
                    process_create_client(header, deserializer);
                }
            }
        }
        /* Process the rest of the messages. */
        else if (ProxyClient* client = get_client(header.client_key()))
        {
            StreamsManager& stream_manager = client->get_stream_manager();
            dds::xrce::StreamId stream_id = header.stream_id();
            uint16_t seq_num = header.sequence_nr();
            if (stream_manager.is_valid_message(stream_id, seq_num))
            {
                if (stream_manager.is_next_message(stream_id, seq_num))
                {
                    /* Process message. */
                    process_message(header, deserializer, *client);

                    /* Update sequence number. */
                    stream_manager.update_stream(stream_id, seq_num);

                    /* Process next messages. */
                    while (stream_manager.message_available(stream_id))
                    {
                        /* Get and process next message. */
                        dds::xrce::XrceMessage next_message = stream_manager.get_next_message(stream_id);
                        Serializer temp_deserializer(next_message.buf, next_message.len);
                        process_message(header, temp_deserializer, *client);

                        /* Update sequence number. */
                        seq_num++;
                        stream_manager.update_stream(stream_id, seq_num);
                    }
                }
                else
                {
                    /* Store message. */
                    stream_manager.store_input_message(stream_id, seq_num,
                                                       deserializer.get_current_position(),
                                                       deserializer.get_remainder_size());
                }
            }
        }
        else
        {
            std::cerr << "Error client unknown." << std::endl;
        }
    }
    else
    {
        std::cerr << "Error reading message header." << std::endl;
    }
}

void Agent::process_message(const dds::xrce::MessageHeader& header, Serializer& deserializer, ProxyClient& client)
{

    dds::xrce::SubmessageHeader sub_header;
    deserializer.force_new_submessage_align();
    bool valid_submessage = deserializer.deserialize(sub_header);
    if (!valid_submessage)
    {
        std::cerr << "Error reading submessage header." << std::endl;
    }

    /* Process submessages. */
    while (valid_submessage)
    {
        switch (sub_header.submessage_id())
        {
            case dds::xrce::CREATE:
                process_create(header, sub_header, deserializer, client);
                break;
            case dds::xrce::GET_INFO:
                /* TODO (Julian). */
                break;
            case dds::xrce::DELETE:
                process_delete(header, sub_header, deserializer, client);
                break;
            case dds::xrce::WRITE_DATA:
                process_write_data(header, sub_header, deserializer, client);
                break;
            case dds::xrce::READ_DATA:
                process_read_data(header, sub_header, deserializer, client);
                break;
            case dds::xrce::HEARTBEAT:
                process_heartbeat(header, sub_header, deserializer, client);
                break;
            case dds::xrce::ACKNACK:
                process_acknack(header, sub_header, deserializer, client);
                break;
            default:
                break;
        }

        /* Get next submessage. */
        if (!deserializer.bufferEnd())
        {
            deserializer.force_new_submessage_align();
            valid_submessage = deserializer.deserialize(sub_header);
            if (!valid_submessage)
            {
                std::cerr << "Error reading submessage header." << std::endl;
            }
        }
        else
        {
            valid_submessage = false;
        }
    }
}

void Agent::process_create_client(const dds::xrce::MessageHeader& header, Serializer& deserializer)
{
    dds::xrce::CREATE_CLIENT_Payload payload;
    if (deserializer.deserialize(payload))
    {
        dds::xrce::STATUS_Payload status;
        status.related_request().request_id(payload.request_id());
        status.related_request().object_id(payload.object_id());
        status.result(create_client(payload));
        add_reply(header, status);
    }
    else
    {
        std::cerr << "Error processing CREATE_CLIENT submessage." << std::endl;
    }
}

void Agent::process_create(const dds::xrce::MessageHeader& header,
                           const dds::xrce::SubmessageHeader& sub_header,
                           Serializer& deserializer,
                           ProxyClient& client)
{
    dds::xrce::CreationMode creation_mode;
    bool reuse = ((sub_header.flags() & dds::xrce::FLAG_REUSE) == dds::xrce::FLAG_REUSE);
    bool replace = ((sub_header.flags() & dds::xrce::FLAG_REPLACE) == dds::xrce::FLAG_REPLACE);
    creation_mode.reuse(reuse);
    creation_mode.replace(replace);

    dds::xrce::CREATE_Payload payload;
    if (deserializer.deserialize(payload))
    {
        dds::xrce::MessageHeader status_header;
        status_header.session_id(header.session_id());
        status_header.stream_id(0x00);
        status_header.sequence_nr(0);
        status_header.client_key(header.client_key());
        dds::xrce::STATUS_Payload status;
        status.related_request().request_id(payload.request_id());
        status.related_request().object_id(payload.object_id());
        status.result(client.create(creation_mode, payload));
        add_reply(status_header, status);
    }
    else
    {
        std::cerr << "Error processing CREATE submessage." << std::endl;
    }
}

void Agent::process_delete(const dds::xrce::MessageHeader& header,
                           const dds::xrce::SubmessageHeader& /*sub_header*/,
                           Serializer& deserializer, ProxyClient& client)
{
    dds::xrce::DELETE_Payload payload;
    if (deserializer.deserialize(payload))
    {
        dds::xrce::MessageHeader status_header;
        status_header.session_id(header.session_id());
        status_header.stream_id(0x00);
        status_header.sequence_nr(0);
        status_header.client_key(header.client_key());
        if (payload.object_id() == dds::xrce::ObjectId{OBJECTID_CLIENT})
        {
            dds::xrce::STATUS_Payload status;
            status.related_request().request_id(payload.request_id());
            status.related_request().object_id(payload.object_id());
            status.result(delete_client(header.client_key()));
            add_reply(status_header, status);
        }
        else
        {
            dds::xrce::STATUS_Payload status;
            status.related_request().request_id(payload.request_id());
            status.related_request().object_id(payload.object_id());
            status.result(client.delete_object(payload));
            add_reply(status_header, status);
        }
    }
    else
    {
        std::cerr << "Error processing DELETE submessage." << std::endl;
    }
}

void Agent::process_write_data(const dds::xrce::MessageHeader& /*header*/,
                               const dds::xrce::SubmessageHeader& sub_header,
                               Serializer& deserializer, ProxyClient& client)
{
    uint8_t flags = sub_header.flags() & 0x0E;
    switch (flags)
    {
        case dds::xrce::FORMAT_DATA_F:
        {
            dds::xrce::WRITE_DATA_Payload_Data payload;
            if (deserializer.deserialize(payload))
            {
                dds::xrce::STATUS_Payload status;
                status.related_request().request_id(payload.request_id());
                status.related_request().object_id(payload.object_id());
                status.result(client.write(payload.object_id(), payload));
            }
            break;
        }
        default:
            break;
    }
}

void Agent::process_read_data(const dds::xrce::MessageHeader& header,
                              const dds::xrce::SubmessageHeader& /*sub_header*/,
                              Serializer& deserializer, ProxyClient& client)
{
    dds::xrce::READ_DATA_Payload payload;
    if (deserializer.deserialize(payload))
    {
        dds::xrce::MessageHeader status_header;
        status_header.session_id(header.session_id());
        status_header.stream_id(0x00);
        status_header.sequence_nr(0);
        status_header.client_key(header.client_key());
        dds::xrce::STATUS_Payload status;
        status.related_request().request_id(payload.request_id());
        status.related_request().object_id(payload.object_id());
        status.result(client.read(payload.object_id(), payload, header.stream_id()));
        add_reply(status_header, status);
    }
    else
    {
        std::cerr << "Error processing READ_DATA submessage." << std::endl;
    }
}

void Agent::process_acknack(const dds::xrce::MessageHeader& header,
                            const dds::xrce::SubmessageHeader& /*sub_header*/,
                            Serializer& deserializer, ProxyClient& client)
{
    dds::xrce::ACKNACK_Payload payload;
    if (deserializer.deserialize(payload))
    {
        /* Send missing messages again. */
        uint16_t first_message = payload.first_unacked_seq_num();
        std::array<uint8_t, 2> nack_bitmap = payload.nack_bitmap();
        for (int i = 0; i < 8; ++i)
        {
            dds::xrce::XrceMessage message;
            uint8_t mask = 0x01 << i;
            if ((nack_bitmap.at(1) & mask) == mask)
            {
                message = client.get_stream_manager().get_output_message((uint8_t) header.sequence_nr(), first_message + i);
                if (message.len != 0)
                {
                    Message output_message(message.buf, message.len);
                    add_reply(output_message);
                }
            }
            if ((nack_bitmap.at(0) & mask) == mask)
            {
                message = client.get_stream_manager().get_output_message((uint8_t) header.sequence_nr(), first_message + i + 8);
                if (message.len != 0)
                {
                    Message output_message(message.buf, message.len);
                    add_reply(output_message);
                }
            }
        }

        /* Update output stream. */
        client.get_stream_manager().update_from_acknack((uint8_t) header.sequence_nr(), first_message, nack_bitmap);
    }
    else
    {
        std::cerr << "Error processing ACKNACK submessage." << std::endl;
    }
}

void Agent::process_heartbeat(const dds::xrce::MessageHeader& header,
                              const dds::xrce::SubmessageHeader& /*sub_header*/,
                              Serializer& deserializer, ProxyClient& client)
{
    dds::xrce::HEARTBEAT_Payload payload;
    if (deserializer.deserialize(payload))
    {
        dds::xrce::StreamId stream_id = (dds::xrce::StreamId)(header.sequence_nr());
        /* Update input stream. */
        client.get_stream_manager().update_from_heartbeat((dds::xrce::StreamId)header.sequence_nr(),
                                                          payload.first_unacked_seq_nr(),
                                                          payload.last_unacked_seq_nr());

        /* Send ACKNACK message. */
        dds::xrce::MessageHeader acknack_header;
        acknack_header.session_id() = header.session_id();
        acknack_header.stream_id() = 0x00;
        acknack_header.sequence_nr() = header.stream_id();
        acknack_header.client_key() = header.client_key();

        dds::xrce::ACKNACK_Payload payload;
        payload.first_unacked_seq_num(client.get_stream_manager().get_first_unacked_seq_num(stream_id));
        payload.nack_bitmap(client.get_stream_manager().get_nack_bitmap(stream_id));

        Message message{};
        XRCEFactory message_creator{message.get_buffer().data(), message.get_buffer().max_size()};
        message_creator.header(acknack_header);
        message_creator.acknack(payload);
        message.set_real_size(message_creator.get_total_size());
        add_reply(message);
    }
    else
    {
        std::cerr << "Error procession HEARTBEAT submessage." << std::endl;
    }
}

} // namespace micrortps
} // namespace eprosima
