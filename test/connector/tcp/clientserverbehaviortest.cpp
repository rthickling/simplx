/**
 * @author Valerian Vives <valerian.vives@tredzone.com>
 * @file clientserverbehaviortest.cpp
 * @brief client and server test initializer: run the client and server tests
 * (without mocking the network)
 * @copyright 2013-2019 Tredzone (www.tredzone.com). All rights reserved.
 * Please see accompanying LICENSE file for licensing terms.
 */

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

#define VERBOSE_TEST 0

#include "clientserver/connectionsuccessclient.hpp"
#include "clientserver/connectionsuccessserver.hpp"
#include "clientserver/messagepassingclient.hpp"
#include "clientserver/messagepassingserver.hpp"
#include "clientserver/servertestactor.hpp"
#include "clientserver/validatoractor.hpp"

using namespace std;
using namespace tredzone;
using namespace tredzone::connector::tcp;

atomic_bool engineStartedFlag;

template <typename _ServerTestClassType, typename _ClientTestClassType>
void runTest(const int64_t timeoutMs, const int64_t shouldTimeOut = false)
{
    g_finished         = false;
    g_expectingTimeout = shouldTimeOut;
    g_failOnTimeout    = false;

    typename _ServerTestClassType::ServerParam serverParam;
    serverParam.m_messageHeaderSize = 1;
    serverParam.m_addressFamily     = AF_INET;
    serverParam.m_addressType       = INADDR_ANY;
    serverParam.m_port              = 8080;

    typename _ClientTestClassType::ConnectParam clientParam;
    clientParam.m_messageHeaderSize = 1;
    clientParam.m_ipAddress         = "127.0.0.1";
    clientParam.m_addressFamily     = AF_INET;
    clientParam.m_port              = 8080;
    clientParam.m_timeoutUSec       = 5000000;
    clientParam.m_ipAddressSource   = "";

    Engine::StartSequence startSequence;
    startSequence.addServiceActor<ValidatorService, ValidatorActor>(3, timeoutMs);
    startSequence.addActor<_ServerTestClassType>(1, serverParam);
    startSequence.addActor<_ClientTestClassType>(2, clientParam);
    Engine             engine(startSequence);
    unique_lock<mutex> lock(g_mutex);
    g_conditionVariable.wait(lock, [] { return g_finished; });
    lock.unlock();

    if (g_failOnTimeout)
    {
        ASSERT_FALSE("Timed Out");
    }
}

constexpr int64_t defaultTimeoutMs = 30;  // 30 ms
constexpr int64_t longTimeoutMs    = 300; // 300 ms => 3 sec
constexpr int64_t shortTimeoutMs   = 3;   // 3 ms
constexpr bool    shouldTimeOut    = true;

TEST(tcp_clientserver, ConnectionSuccess)
{
    runTest<tredzone::connector::tcp::ConnectionSuccess::ServerActor,
            tredzone::connector::tcp::ConnectionSuccess::ClientActor>(defaultTimeoutMs);
}

TEST(tcp_clientserver, MessagePassing)
{
    runTest<tredzone::connector::tcp::MessagePassing::ServerActor,
            tredzone::connector::tcp::MessagePassing::ClientActor>(shortTimeoutMs, shouldTimeOut);
}


namespace
{
using ExceptionTestNetwork = Network<255, 255, 255, 255>;
using ExceptionTestClient = IClient<ExceptionTestNetwork>;
using ExceptionTestServer = IServer<ExceptionTestNetwork>;

// Copy after the original and its constructor arguments have been destroyed.
// Long messages also exercise heap-backed string storage.
template <typename Exception> void checkExceptionMessage(const Exception &original, const std::string &expected)
{
    const Exception copied(original);
    const char *message = copied.what();
    EXPECT_STREQ(expected.c_str(), message);
    EXPECT_EQ(message, copied.what());
}
} // namespace

TEST(tcp_clientserver, ExceptionMessagesOwnTheirData)
{
    const std::string address(512, 'x');
    const auto connected = [&] {
        ExceptionTestClient::ConnectParam param{};
        param.m_ipAddress = address;
        param.m_addressFamily = AF_INET;
        param.m_port = 8080;
        param.m_timeoutUSec = 5000;
        const Actor::ActorId actorId{};
        const ExceptionTestClient::AlreadyConnectedException original(actorId, param);
        return ExceptionTestClient::AlreadyConnectedException(original);
    }();
    std::ostringstream actor;
    actor << Actor::ActorId{};
    checkExceptionMessage(connected, "TcpClient (" + actor.str() + ") already connected ( ipAddress:" + address
        + " addressFamily:" + std::to_string(AF_INET) + " port:8080 timeoutUSec:5000)");

    const auto notConnected = [] {
        const Actor::ActorId actorId{};
        return ExceptionTestClient::NotConnectedException(actorId);
    }();
    checkExceptionMessage(notConnected, "TcpClient (" + actor.str() + "): TcpClient not connected");

    const auto bufferFull = [] {
        const Actor::ActorId actorId{};
        return ExceptionTestClient::SendBufferFullException(actorId);
    }();
    checkExceptionMessage(bufferFull, "TcpClient (" + actor.str() + "): Fail to bufferize data: buffer full");

    const auto listening = [] {
        const ExceptionTestServer::ListenParam param{AF_INET, SOCK_STREAM, 8080};
        return ExceptionTestServer::AlreadyListeningException(param);
    }();
    checkExceptionMessage(listening, "TcpServer already listening ( addressFamily:" + std::to_string(AF_INET)
        + " addressType:" + std::to_string(static_cast<int>(SOCK_STREAM)) + " port:8080)");

    const auto directSend = [] {
        const std::string error(512, 'e');
        return ExceptionTestNetwork::DirectSendException(error);
    }();
    checkExceptionMessage(directSend, "Failed to direct send message: " + std::string(512, 'e'));
}
