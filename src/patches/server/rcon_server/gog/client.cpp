#include "client.hpp"
#include "authentication.hpp"
#include "game.hpp"
#include "protocol.hpp"

#include <cstring>
#include <string_view>
#include <utility>

namespace fusioncutter::patches::rcon_server::gog {
namespace {

// The legacy login frame is 32 lowercase MD5 hexadecimal characters followed by this marker byte.
constexpr char kLoginMagic = 0x64;

} // namespace

Client::Client(SOCKET socket) noexcept : socket_(socket) {}

Client::~Client() {
    disconnect();
}

Client::Client(Client&& other) noexcept {
    move_from(std::move(other));
}

Client& Client::operator=(Client&& other) noexcept {
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
    return static_cast<short>(POLLRDNORM | (output_.empty() ? 0 : POLLWRNORM));
}

bool Client::valid() const noexcept {
    return socket_ != INVALID_SOCKET;
}

bool Client::authenticated() const noexcept {
    return authenticated_;
}

void Client::handle_events(short events, Game& game, Authentication& authentication) {
    if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        disconnect();
        return;
    }
    if ((events & POLLRDNORM) != 0) {
        receive(game, authentication);
    }
    if (valid() && (events & POLLWRNORM) != 0) {
        send_pending();
    }
}

void Client::queue_packet(std::vector<char> packet) {
    // A slow client must not turn server broadcasts into unbounded retained memory.
    if (packet.size() > kMaximumQueuedBytes || queued_bytes_ > kMaximumQueuedBytes - packet.size()) {
        disconnect();
        return;
    }
    queued_bytes_ += packet.size();
    output_.push_back(std::move(packet));
}

void Client::disconnect() noexcept {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    output_.clear();
    queued_bytes_ = 0;
    output_offset_ = 0;
}

void Client::receive(Game& game, Authentication& authentication) {
    if (input_size_ == input_.size()) {
        disconnect();
        return;
    }

    const auto received = recv(socket_, input_.data() + input_size_, static_cast<int>(input_.size() - input_size_), 0);
    if (received > 0) {
        input_size_ += static_cast<std::size_t>(received);
        process_input(game, authentication);
    } else if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
        disconnect();
    }
}

void Client::process_input(Game& game, Authentication& authentication) {
    while (valid()) {
        if (!authenticated_) {
            if (input_size_ < 33) {
                return;
            }

            const auto received_hash = std::string_view{input_.data(), 32};
            const auto accepted =
                input_[32] == kLoginMagic && authentication.matches(game.admin_password(), received_hash);
            queue_packet({static_cast<char>(accepted)});
            consume_input(33);
            if (!accepted) {
                close_after_send_ = true;
                return;
            }
            authenticated_ = true;
            continue;
        }

        const auto frame = protocol::decode_request({input_.data(), input_size_});
        if (frame.state == protocol::FrameState::Incomplete) {
            return;
        }
        if (frame.state == protocol::FrameState::Invalid) {
            disconnect();
            return;
        }
        queue_packet(protocol::encode_message(game.execute(frame.command)));
        consume_input(frame.consumed);
    }
}

void Client::consume_input(std::size_t size) noexcept {
    input_size_ -= size;
    std::memmove(input_.data(), input_.data() + size, input_size_);
}

void Client::send_pending() noexcept {
    while (!output_.empty()) {
        auto& packet = output_.front();
        const auto remaining = packet.size() - output_offset_;
        const auto sent = send(socket_, packet.data() + output_offset_, static_cast<int>(remaining), 0);
        if (sent > 0) {
            output_offset_ += static_cast<std::size_t>(sent);
            queued_bytes_ -= static_cast<std::size_t>(sent);
            if (output_offset_ == packet.size()) {
                output_.pop_front();
                output_offset_ = 0;
            }
            continue;
        }
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            return;
        }
        disconnect();
        return;
    }
    if (close_after_send_) {
        disconnect();
    }
}

void Client::move_from(Client&& other) noexcept {
    socket_ = std::exchange(other.socket_, INVALID_SOCKET);
    authenticated_ = std::exchange(other.authenticated_, false);
    close_after_send_ = std::exchange(other.close_after_send_, false);
    input_ = other.input_;
    input_size_ = std::exchange(other.input_size_, 0);
    output_ = std::move(other.output_);
    output_offset_ = std::exchange(other.output_offset_, 0);
    queued_bytes_ = std::exchange(other.queued_bytes_, 0);
}

} // namespace fusioncutter::patches::rcon_server::gog
