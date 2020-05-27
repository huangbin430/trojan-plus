/*
 * This file is part of the trojan project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Copyright (C) 2017-2020  The Trojan Authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "serversession.h"

#include "core/service.h"
#include "core/utils.h"
#include "proto/trojanrequest.h"
#include "proto/udppacket.h"

using namespace std;
using namespace boost::asio::ip;
using namespace boost::asio::ssl;

ServerSession::ServerSession(Service* _service, const Config& config, boost::asio::ssl::context &ssl_context, Authenticator *auth, const std::string &plain_http_response) :
    SocketSession(_service, config),
    status(HANDSHAKE),
    in_socket(_service->get_io_context(), ssl_context),
    out_socket(_service->get_io_context()),
    udp_resolver(_service->get_io_context()),
    auth(auth),
    plain_http_response(plain_http_response),
    use_pipeline(false),
    has_queried_out(false) {}

tcp::socket& ServerSession::accept_socket() {
    return (tcp::socket&)in_socket.next_layer();
}

void ServerSession::start() {
    
    start_time = time(nullptr);

    if(!use_pipeline){
        boost::system::error_code ec;
        in_endpoint = in_socket.next_layer().remote_endpoint(ec);
        if (ec) {
            output_debug_info_ec(ec);
            destroy();
            return;
        }
        auto self = shared_from_this();
        in_socket.async_handshake(stream_base::server, [this, self](const boost::system::error_code error) {
            if (error) {
                _log_with_endpoint(in_endpoint, "SSL handshake failed: " + error.message(), Log::ERROR);
                if (error.message() == "http request" && plain_http_response.empty()) {
                    recv_len += plain_http_response.length();
                    boost::asio::async_write(accept_socket(), boost::asio::buffer(plain_http_response), [this, self](const boost::system::error_code, size_t) {
                        output_debug_info();
                        destroy();
                    });
                    return;
                }
                output_debug_info();
                destroy();
                return;
            }
            in_async_read();
        });
    }else{
        in_async_read();
    }
}

void ServerSession::pipeline_in_recv(string &&data) {
    if (!use_pipeline) {
        throw logic_error("cannot call pipeline_in_recv without pipeline!");
    }

    if (status != DESTROY) {
        pipeline_data_cache.push_data(std::move(data));
    }
}

void ServerSession::in_async_read() {
    if(use_pipeline){
        pipeline_data_cache.async_read([this](const string &data) {
            in_recv(data);
        });
    }else{
        auto self = shared_from_this();
        in_socket.async_read_some(boost::asio::buffer(in_read_buf, MAX_BUF_LENGTH), [this, self](const boost::system::error_code error, size_t length) {
            if (error) {
                output_debug_info_ec(error);
                destroy();
                return;
            }
            in_recv(string((const char*)in_read_buf, length));
        });
    }
}

void ServerSession::in_async_write(const string &data) {
    auto self = shared_from_this();
    if(use_pipeline){
        if(!pipeline.expired()){
            (static_cast<PipelineSession*>(pipeline.lock().get()))->session_write_data(*this, data, [this, self](const boost::system::error_code){
                in_sent();
            });            
        }else{
            output_debug_info();
            destroy();
        }
    }else{
        auto data_copy = make_shared<string>(data);
        boost::asio::async_write(in_socket, boost::asio::buffer(*data_copy), [this, self, data_copy](const boost::system::error_code error, size_t) {
            if (error) {
                output_debug_info_ec(error);
                destroy();
                return;
            }
            in_sent();
        });
    }
}

void ServerSession::out_async_read() {
    if(pipeline_com.is_using_pipeline()){
        if(!pipeline_com.pre_call_ack_func()){
            _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + " cannot ServerSession::out_async_read ! Is waiting for ack");
            return;
        }
        _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + 
            " permit to ServerSession::out_async_read aysnc! ack:" + to_string(pipeline_com.pipeline_ack_counter));
    }

    auto self = shared_from_this();
    out_socket.async_read_some(boost::asio::buffer(out_read_buf, MAX_BUF_LENGTH), [this, self](const boost::system::error_code error, size_t length) {
        if (error) {
            output_debug_info_ec(error);
            destroy();
            return;
        }
        out_recv(string((const char*)out_read_buf, length));
    });
}

void ServerSession::out_async_write(const string &data) {
    auto self = shared_from_this();
    auto data_copy = make_shared<string>(data);
    boost::asio::async_write(out_socket, boost::asio::buffer(*data_copy), [this, self, data_copy](const boost::system::error_code error, size_t) {
        if (error) {
            output_debug_info_ec(error);
            destroy();
            return;
        }
        
        if(use_pipeline && !pipeline.expired()){
            (static_cast<PipelineSession*>(pipeline.lock().get()))->session_write_ack(*this, [this, self](const boost::system::error_code){
                out_sent();
            });
        }else{
            out_sent();
        }        
    });
}

void ServerSession::udp_async_read() {
    auto self = shared_from_this();
    udp_socket.async_receive_from(boost::asio::buffer(udp_read_buf, MAX_BUF_LENGTH), udp_recv_endpoint, [this, self](const boost::system::error_code error, size_t length) {
        if (error) {
            output_debug_info_ec(error);
            destroy();
            return;
        }
        udp_recv(string((const char*)udp_read_buf, length), udp_recv_endpoint);
    });
}

void ServerSession::udp_async_write(const string &data, const udp::endpoint &endpoint) {
    auto self = shared_from_this();
    auto data_copy = make_shared<string>(data);
    udp_socket.async_send_to(boost::asio::buffer(*data_copy), endpoint, [this, self, data_copy](const boost::system::error_code error, size_t) {
        if (error) {
            output_debug_info_ec(error);
            destroy();
            return;
        }
        udp_sent();
    });
}

void ServerSession::in_recv(const string &data) {
    if (status == HANDSHAKE) {
        
        if(has_queried_out){
            // pipeline session will call this in_recv directly so that the HANDSHAKE status will remain for a while
            out_write_buf += data;
            sent_len += data.length();
            return;
        }

        TrojanRequest req;
        bool valid = req.parse(data) != -1;
        if (valid) {
            auto password_iterator = config.password.find(req.password);
            if (password_iterator == config.password.end()) {
                valid = false;
                if (auth && auth->auth(req.password)) {
                    valid = true;
                    auth_password = req.password;
                    _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + " authenticated by authenticator (" + req.password.substr(0, 7) + ')', Log::INFO);
                }
            } else {
                _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + " authenticated as " + password_iterator->second, Log::INFO);
            }
            if (!valid) {
                _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + " valid trojan request structure but possibly incorrect password (" + req.password + ')', Log::WARN);
            }
        }
        string query_addr = valid ? req.address.address : config.remote_addr;
        string query_port = to_string([&]() {
            if (valid) {
                return req.address.port;
            }
            const unsigned char *alpn_out;
            unsigned int alpn_len;
            SSL_get0_alpn_selected(in_socket.native_handle(), &alpn_out, &alpn_len);
            if (alpn_out == nullptr) {
                return config.remote_port;
            }
            auto it = config.ssl.alpn_port_override.find(string(alpn_out, alpn_out + alpn_len));
            return it == config.ssl.alpn_port_override.end() ? config.remote_port : it->second;
        }());
        if (valid) {
            out_write_buf = req.payload;
            if (req.command == TrojanRequest::UDP_ASSOCIATE) {
                out_udp_endpoint = udp::endpoint(make_address(req.address.address), req.address.port);
                _log_with_endpoint(out_udp_endpoint, "session_id: " + to_string(get_session_id()) + " requested UDP associate to " + req.address.address + ':' + to_string(req.address.port), Log::INFO);
                status = UDP_FORWARD;
                udp_data_buf = out_write_buf;
                udp_sent();
                return;
            } else {
                _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + " requested connection to " + req.address.address + ':' + to_string(req.address.port), Log::INFO);
            }
        } else {
            _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + " not trojan request, connecting to " + query_addr + ':' + query_port, Log::WARN);
            out_write_buf = data;
        }
        sent_len += out_write_buf.length();
        has_queried_out = true;

        auto self = shared_from_this();
        connect_out_socket(this, query_addr, query_port, resolver, out_socket, in_endpoint, [this, self](){
            status = FORWARD;
            out_async_read();
            if (!out_write_buf.empty()) {
                out_async_write(out_write_buf);
            } else {
                in_async_read();
            }
        });

    } else if (status == FORWARD) {
        sent_len += data.length();
        out_async_write(data);
    } else if (status == UDP_FORWARD) {
        udp_data_buf += data;
        udp_sent();
    }
}

void ServerSession::in_sent() {
    if (status == FORWARD) {
        out_async_read();
    } else if (status == UDP_FORWARD) {
        udp_async_read();
    }
}

void ServerSession::out_recv(const string &data) {
    if (status == FORWARD) {
        recv_len += data.length();
        in_async_write(data);
    }
}

void ServerSession::out_sent() {
    if (status == FORWARD) {
        in_async_read();        
    }
}

void ServerSession::udp_recv(const string &data, const udp::endpoint &endpoint) {
    if (status == UDP_FORWARD) {
        size_t length = data.length();
        _log_with_endpoint(out_udp_endpoint, "session_id: " + to_string(get_session_id()) + " received a UDP packet of length " + to_string(length) + " bytes from " + endpoint.address().to_string() + ':' + to_string(endpoint.port()));
        recv_len += length;
        in_async_write(UDPPacket::generate(endpoint, data));
    }
}

void ServerSession::udp_sent() {
    if (status == UDP_FORWARD) {
        UDPPacket packet;
        size_t packet_len;
        bool is_packet_valid = packet.parse(udp_data_buf, packet_len);
        if (!is_packet_valid) {
            if (udp_data_buf.length() > MAX_BUF_LENGTH) {
                _log_with_endpoint(out_udp_endpoint, "session_id: " + to_string(get_session_id()) + " UDP packet too long", Log::ERROR);
                destroy();
                return;
            }
            in_async_read();
            return;
        }
        _log_with_endpoint(out_udp_endpoint, "session_id: " + to_string(get_session_id()) + " sent a UDP packet of length " + to_string(packet.length) + " bytes to " + packet.address.address + ':' + to_string(packet.address.port));
        udp_data_buf = udp_data_buf.substr(packet_len);
        string query_addr = packet.address.address;
        auto self = shared_from_this();
        udp_resolver.async_resolve(query_addr, to_string(packet.address.port), [this, self, packet, query_addr](const boost::system::error_code error, udp::resolver::results_type results) {
            if (error || results.empty()) {
                _log_with_endpoint(out_udp_endpoint, "session_id: " + to_string(get_session_id()) + " cannot resolve remote server hostname " + query_addr + ": " + error.message(), Log::ERROR);
                destroy();
                return;
            }
            auto iterator = results.begin();
            if (config.tcp.prefer_ipv4) {
                for (auto it = results.begin(); it != results.end(); ++it) {
                    const auto &addr = it->endpoint().address();
                    if (addr.is_v4()) {
                        iterator = it;
                        break;
                    }
                }
            }
            _log_with_endpoint(out_udp_endpoint, "session_id: " + to_string(get_session_id()) + " " + query_addr + " is resolved to " + iterator->endpoint().address().to_string(), Log::ALL);
            if (!udp_socket.is_open()) {
                auto protocol = iterator->endpoint().protocol();
                boost::system::error_code ec;
                udp_socket.open(protocol, ec);
                if (ec) {
                    output_debug_info_ec(ec);
                    destroy();
                    return;
                }
                udp_socket.bind(udp::endpoint(protocol, 0));
                udp_async_read();
            }
            sent_len += packet.length;
            udp_async_write(packet.payload, *iterator);
        });
    }
}

void ServerSession::destroy(bool pipeline_call /*= false*/) {
    if (status == DESTROY) {
        return;
    }
    status = DESTROY;
    _log_with_endpoint(in_endpoint, "session_id: " + to_string(get_session_id()) + " disconnected, " + to_string(recv_len) + " bytes received, " + to_string(sent_len) + " bytes sent, lasted for " + to_string(time(nullptr) - start_time) + " seconds", Log::INFO);
    if (auth && !auth_password.empty()) {
        auth->record(auth_password, recv_len, sent_len);
    }
    boost::system::error_code ec;
    resolver.cancel();
    udp_resolver.cancel();
    if (out_socket.is_open()) {
        out_socket.cancel(ec);
        out_socket.shutdown(tcp::socket::shutdown_both, ec);
        out_socket.close(ec);
    }
    if (udp_socket.is_open()) {
        udp_socket.cancel(ec);
        udp_socket.close(ec);
    }

    shutdown_ssl_socket(this, in_socket);    

    if(!pipeline_call && use_pipeline && !pipeline.expired()){
        (static_cast<PipelineSession*>(pipeline.lock().get()))->remove_session_after_destroy(*this);
    }
}
