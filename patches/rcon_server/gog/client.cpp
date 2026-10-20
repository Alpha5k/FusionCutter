#include "client.hpp"

#include "authentication.hpp"
#include "game.hpp"
#include "protocol.hpp"

#include <cstring>
#include <string_view>
#include <utility>

namespace fc::core_plugin::rcon_server::gog {
namespace {

// A legacy login frame is 32 lowercase MD5 hexadecimal characters followed by this marker byte.
constexpr char kLoginMarker = 0x64;

} // namespace

Client::Client(SOCKET socket) noexcept : socket_(socket) {}

Client::~Client() {
    disconnect();
}

Client::Client(Client &&other) noexcept {
    move_from(std::move(other));
}

Client &Client::operator=(Client &&other) noexcept {
    if (this != &other) {
        disconnect();
        move_from(std::move(other));
    }
    return *this;
}

SOCKET Client::socket() const noexcept {
    return socket_;
}

short Client::poll_events() const noexcept {
    const auto read = close_after_send_ ? 0 : POLLRDNORM;
    const auto write = output_.empty() ? 0 : POLLWRNORM;
    return static_cast<short>(read | write);
}

bool Client::valid() const noexcept {
    return socket_ != INVALID_SOCKET;
}

bool Client::authenticated() const noexcept {
    return authenticated_;
}

bool Client::handle_events(short events, GameBridge &game, Authentication &authentication) {
    if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        disconnect();
        return false;
    }

    std::size_t processed_frames{};
    // Buffered frames may remain after a previous budget was exhausted even when the socket has no new event.
    if (process_input(game, authentication, processed_frames)) {
        return true;
    }
    if (valid() && (events & POLLRDNORM) != 0) {
        receive();
        if (process_input(game, authentication, processed_frames)) {
            return true;
        }
    }

    if (valid() && (events & POLLWRNORM) != 0) {
        send_pending();
    }
    return false;
}

bool Client::queue_packet(std::vector<char> packet) {
    // Check the packet size first so subtracting it from the byte limit cannot underflow.
    if (packet.size() > kMaximumQueuedBytes || output_.size() - output_offset_ > kMaximumQueuedBytes - packet.size()) {
        disconnect();
        return false;
    }

    // Remove bytes already sent before appending to the ordered stream.
    if (output_offset_ != 0) {
        output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(output_offset_));
        output_offset_ = 0;
    }
    output_.insert(output_.end(), packet.begin(), packet.end());
    return true;
}

void Client::disconnect() noexcept {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    output_.clear();
    output_offset_ = 0;
}

void Client::receive() noexcept {
    if (input_size_ == input_.size()) {
        disconnect();
        return;
    }

    const auto received = recv(socket_, input_.data() + input_size_, static_cast<int>(input_.size() - input_size_), 0);
    if (received > 0) {
        input_size_ += static_cast<std::size_t>(received);
    } else if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
        disconnect();
    }
}

bool Client::process_input(GameBridge &game, Authentication &authentication, std::size_t &processed_frames) {
    if (close_after_send_) {
        return false;
    }

    while (valid() && processed_frames < kMaximumFramesPerPoll) {
        if (!authenticated_) {
            // Login is a fixed digest and marker followed by a single-byte acceptance response.
            if (input_size_ < 33) {
                return false;
            }

            const auto received_digest = std::string_view{input_.data(), 32};
            const auto accepted = input_[32] == kLoginMarker && authentication.matches(game.admin_password(), received_digest);
            if (!queue_packet({static_cast<char>(accepted)})) {
                return true;
            }

            consume_input(33);
            ++processed_frames;
            if (!accepted) {
                // Retain only the rejection reply for sending; later buffered frames cannot retry login.
                close_after_send_ = true;
                return false;
            }
            authenticated_ = true;
            continue;
        }

        // Authenticated traffic carries one NUL-terminated command per legacy row frame.
        const auto frame = protocol::decode_request({input_.data(), input_size_});
        if (frame.state == protocol::FrameState::Incomplete) {
            return false;
        }
        if (frame.state == protocol::FrameState::Invalid) {
            disconnect();
            return false;
        }

        // Execute while the decoded command still borrows the receive buffer, before compacting that buffer.
        if (!queue_packet(protocol::encode_message(game.execute(frame.command)))) {
            return true;
        }

        consume_input(frame.consumed);
        ++processed_frames;
    }
    return false;
}

void Client::consume_input(std::size_t size) noexcept {
    input_size_ -= size;
    std::memmove(input_.data(), input_.data() + size, input_size_);
}

void Client::send_pending() noexcept {
    // One nonblocking write gives other connections and game notifications a turn between polls.
    if (!output_.empty()) {
        const auto remaining = output_.size() - output_offset_;
        const auto sent = send(socket_, output_.data() + output_offset_, static_cast<int>(remaining), 0);
        // Resume at the accepted byte position, even when it falls inside an encoded protocol frame.
        if (sent > 0) {
            output_offset_ += static_cast<std::size_t>(sent);
            if (output_offset_ == output_.size()) {
                output_.clear();
                output_offset_ = 0;
            }
        } else if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            return;
        } else {
            disconnect();
            return;
        }
    }

    // A rejected login closes only after its response has been handed to the socket.
    if (output_.empty() && close_after_send_) {
        disconnect();
    }
}

void Client::move_from(Client &&other) noexcept {
    socket_ = std::exchange(other.socket_, INVALID_SOCKET);
    authenticated_ = std::exchange(other.authenticated_, false);
    close_after_send_ = std::exchange(other.close_after_send_, false);
    input_ = other.input_;
    input_size_ = std::exchange(other.input_size_, 0);

    output_ = std::move(other.output_);
    output_offset_ = std::exchange(other.output_offset_, 0);
}

} // namespace fc::core_plugin::rcon_server::gog
