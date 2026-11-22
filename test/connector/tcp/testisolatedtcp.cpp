#include "isolated/options.h"
#include "trz/connector/tcpconnector.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using tredzone::Actor;

constexpr std::size_t frameSize = 128;
constexpr unsigned burstFrames = 256;
constexpr std::uint8_t echoRequest = 1;
constexpr std::uint8_t burstRequest = 2;
constexpr std::uint8_t closeRequest = 3;
constexpr std::uint8_t response = 4;
using Frame = std::array<std::uint8_t, frameSize>;

Frame frame(std::uint8_t kind, unsigned sequence) {
    Frame bytes{};
    bytes[0] = static_cast<std::uint8_t>(bytes.size() - 1);
    bytes[1] = kind;
    bytes[2] = static_cast<std::uint8_t>(sequence & 0xff);
    bytes[3] = static_cast<std::uint8_t>(sequence >> 8);
    for (std::size_t i = 4; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>((sequence * 17 + i * 11) & 0xff);
    }
    return bytes;
}

struct State {
    unsigned cpu;
    std::atomic<unsigned> port{0};
    std::atomic<unsigned> receivedBytes{0};
    std::atomic<unsigned> requests{0};
    std::atomic<unsigned> closes{0};
    std::atomic<unsigned> shortWrites{0};
    std::atomic<unsigned> wouldBlock{0};
    std::atomic<const char *> error{nullptr};
};

// Observe real syscalls without imposing a particular segmentation pattern.
struct ObservedCalls : NetworkCalls {
    static inline State *state = nullptr;

    static int recv(int fd, void *data, std::size_t size, int flags) noexcept {
        const auto result = NetworkCalls::recv(fd, data, size, flags);
        if (result > 0) {
            state->receivedBytes.fetch_add(static_cast<unsigned>(result));
        }
        return result;
    }

    static int send(int fd, const void *data, std::size_t size, int flags) noexcept {
        const auto result = NetworkCalls::send(fd, data, size, flags);
        const auto savedErrno = errno;
        if (result > 0 && static_cast<std::size_t>(result) < size) {
            state->shortWrites.fetch_add(1);
        } else if (result < 0 && (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)) {
            state->wouldBlock.fetch_add(1);
        }
        errno = savedErrno;
        return result;
    }
};

using TestNetwork = Network<16, 4, 4096, 8, ObservedCalls>;

class Peer : public TcpClient<TestNetwork, (burstFrames + 1) * frameSize, 8192> {
    using Base = TcpClient<TestNetwork, (burstFrames + 1) * frameSize, 8192>;

public:
    Peer() { setConnectParam("127.0.0.1", AF_INET, 0, 5'000'000, 1, ""); }

private:
    size_t getHeaderSize(const std::uint8_t *) noexcept override { return 1; }
    std::uint64_t getDataSize(const std::uint8_t *data, size_t) noexcept override { return data[0]; }

    void onConnect() noexcept override {
        int sendBufferBytes = 4096;
        if (::setsockopt(static_cast<int>(getFd()), SOL_SOCKET, SO_SNDBUF, &sendBufferBytes,
                         sizeof(sendBufferBytes)) != 0) {
            ObservedCalls::state->error.store("could not set the server socket send buffer");
        }
    }

    void onOverflowDataReceived(const std::uint8_t *, size_t) noexcept override {
        ObservedCalls::state->error.store("received an oversized frame");
    }

    void onDisconnect() noexcept override { ObservedCalls::state->closes.fetch_add(1); }

    void onDataReceived(const std::uint8_t *data, size_t size) noexcept override {
        auto &state = *ObservedCalls::state;
        if (size != frameSize || ::sched_getcpu() != static_cast<int>(state.cpu)) {
            state.error.store("invalid connector frame size or handler CPU");
            return;
        }
        const auto kind = data[1];
        const auto sequence = static_cast<unsigned>(data[2]) | (static_cast<unsigned>(data[3]) << 8);
        const auto expected = frame(kind, sequence);
        if (!std::equal(expected.begin(), expected.end(), data)) {
            state.error.store("connector received a corrupt frame");
            return;
        }
        state.requests.fetch_add(1);
        try {
            switch (kind) {
            case echoRequest:
                queueResponse(sequence);
                break;
            case burstRequest:
                for (unsigned i = 0; i < burstFrames; ++i) {
                    queueResponse(sequence + i);
                }
                break;
            case closeRequest:
                disconnect();
                break;
            default:
                state.error.store("unknown frame type");
            }
        } catch (...) {
            state.error.store("connector could not queue the response");
        }
    }

    void queueResponse(unsigned sequence) {
        const auto bytes = frame(response, sequence);
        Base::send(bytes.data(), bytes.size());
    }
};

class Server : public TcpServer<TestNetwork, Peer> {
public:
    explicit Server(State *state) : state_(*state) {
        setMessageHeaderSize(1);
        registerListen(AF_INET, htonl(INADDR_LOOPBACK), 0);
    }

private:
    void onListenSucceed() noexcept override {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (::getsockname(static_cast<int>(m_fd), reinterpret_cast<sockaddr *>(&address), &length) != 0) {
            state_.error.store("could not read the listener's ephemeral port");
            return;
        }
        state_.port.store(ntohs(address.sin_port));
    }
    void onListenFailed() noexcept override { state_.error.store("TCP listen failed"); }
    void onAcceptFailed() noexcept override { state_.error.store("TCP accept failed"); }
    State &state_;
};

[[noreturn]] void socketError(const char *operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

class Client {
public:
    explicit Client(unsigned port) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
        if (fd_ < 0) {
            socketError("socket");
        }
        try {
            setup(port);
        } catch (...) {
            ::close(fd_);
            throw;
        }
    }

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    ~Client() { ::close(fd_); }

    void write(std::span<const std::uint8_t> bytes) const {
        const auto deadline = Clock::now() + 5s;
        while (!bytes.empty()) {
            wait(POLLOUT, deadline);
            const auto result = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (result > 0) {
                bytes = bytes.subspan(static_cast<std::size_t>(result));
            } else if (result == 0 || (errno != EINTR && errno != EAGAIN)) {
                socketError("send");
            }
        }
    }

    void expect(unsigned first, unsigned count) const {
        const auto deadline = Clock::now() + 5s;
        for (unsigned i = 0; i < count; ++i) {
            Frame bytes{};
            std::span remaining(bytes.data(), bytes.size());
            while (!remaining.empty()) {
                wait(POLLIN, deadline);
                const auto result = ::recv(fd_, remaining.data(), remaining.size(), 0);
                if (result > 0) {
                    remaining = remaining.subspan(static_cast<std::size_t>(result));
                } else if (result == 0) {
                    throw std::runtime_error("TCP closed before the expected frames arrived");
                } else if (errno != EINTR && errno != EAGAIN) {
                    socketError("recv");
                }
            }
            if (bytes != frame(response, first + i)) {
                throw std::runtime_error("TCP response corrupted, duplicated, or reordered");
            }
        }
    }

    void expectEnd() const {
        wait(POLLIN, Clock::now() + 5s);
        std::uint8_t extra = 0;
        if (::recv(fd_, &extra, 1, 0) != 0) {
            throw std::runtime_error("TCP stream contained more data after its expected responses");
        }
    }

private:
    void setup(unsigned port) const {
        int receiveBufferBytes = 4096;
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes, sizeof(receiveBufferBytes)) != 0) {
            socketError("setsockopt(SO_RCVBUF)");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::connect(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
            if (errno != EINPROGRESS) {
                socketError("connect");
            }
            wait(POLLOUT, Clock::now() + 5s);
            int error = 0;
            socklen_t length = sizeof(error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
                socketError("getsockopt(SO_ERROR)");
            }
            if (error != 0) {
                throw std::system_error(error, std::generic_category(), "connect");
            }
        }
    }

    void wait(short events, Clock::time_point deadline) const {
        pollfd descriptor{fd_, events, 0};
        while (Clock::now() < deadline) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            const auto ready = ::poll(&descriptor, 1, static_cast<int>(std::max(remaining, 0ms).count()));
            if (ready > 0) {
                return;
            }
            if (ready < 0 && errno != EINTR) {
                socketError("poll");
            }
        }
        throw std::runtime_error("TCP socket operation exceeded its five-second deadline");
    }

    int fd_;
};

template<class Predicate>
void waitFor(const State &state, Predicate ready, const char *message) {
    const auto deadline = Clock::now() + 5s;
    while (!ready() && Clock::now() < deadline && state.error.load() == nullptr) {
        std::this_thread::sleep_for(1ms);
    }
    if (const auto *error = state.error.load()) {
        throw std::runtime_error(error);
    }
    if (!ready()) {
        throw std::runtime_error(message);
    }
}

void exerciseTransport(State &state) {
    waitFor(state, [&] { return state.port.load() != 0; }, "TCP listener never became ready");
    const auto port = state.port.load();
    {
        Client client(port);
        const auto first = frame(echoRequest, 1);
        const auto rest = std::span(first).subspan(1);
        client.write(std::span(first).first(1));
        waitFor(state, [&] { return state.receivedBytes.load() == 1; }, "TCP header fragment was not read");
        if (state.requests.load() != 0) {
            throw std::runtime_error("connector dispatched an incomplete frame");
        }
        client.write(rest);
        client.expect(1, 1);

        const auto second = frame(echoRequest, 2);
        const auto third = frame(echoRequest, 3);
        std::array<std::uint8_t, 2 * frameSize> coalesced{};
        std::copy(second.begin(), second.end(), coalesced.begin());
        std::copy(third.begin(), third.end(), coalesced.begin() + frameSize);
        client.write(coalesced);
        client.expect(2, 2);

        client.write(frame(burstRequest, 100));
        // Keep this socket unread until Linux reports genuine write pressure.
        waitFor(state, [&] { return state.wouldBlock.load() != 0; }, "slow reader did not exercise EAGAIN");
        if (state.shortWrites.load() == 0) {
            throw std::runtime_error("slow reader did not exercise a partial TCP write");
        }
        client.expect(100, burstFrames);
        client.write(frame(closeRequest, 0));
        client.expectEnd();
    }
    {
        Client reconnected(port);
        reconnected.write(frame(echoRequest, 11));
        reconnected.expect(11, 1);
        reconnected.write(frame(closeRequest, 0));
        reconnected.expectEnd();
    }
    waitFor(state, [&] { return state.closes.load() == 2; }, "TCP streams did not close cleanly");
    if (state.requests.load() != 7) {
        throw std::runtime_error("TCP connector dispatched an unexpected number of requests");
    }
}

void run(unsigned cpu, int priority) {
    State state{cpu};
    ObservedCalls::state = &state;
    const auto core = static_cast<Actor::CoreId>(cpu);
    Engine::CoreSet cores;
    cores.set(core);
    Engine::StartSequence start(cores);
    if (priority != 0) {
        tredzone::ThreadRealTimeParam scheduling;
        scheduling.sched_priority = priority;
        start.setRedZoneParam(scheduling);
        start.setRedZoneCore(core);
    }
    start.addActor<Server>(core, &state);
    try {
        Engine engine(start);
        exerciseTransport(state);
    } catch (...) {
        ObservedCalls::state = nullptr;
        throw;
    }
    ObservedCalls::state = nullptr;
    std::cout << "cpu=" << cpu << " requests=" << state.requests.load() << " responses=" << (4 + burstFrames)
              << " short_writes=" << state.shortWrites.load() << " would_block=" << state.wouldBlock.load() << '\n';
}
}

int main(int argc, char **argv) {
    try {
        const auto config = isolated_test::parseOptions(argc, argv);
        tredzone::threadSetAffinity(config.housekeeping);
        run(config.cpuA, config.fifoPriority);
        run(config.cpuB, config.fifoPriority);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "TCP check: " << error.what() << '\n';
        return 1;
    }
}
