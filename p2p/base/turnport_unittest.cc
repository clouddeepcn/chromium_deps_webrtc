/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#if defined(WEBRTC_POSIX)
#include <dirent.h>
#endif

#include <memory>

#include "webrtc/p2p/base/basicpacketsocketfactory.h"
#include "webrtc/p2p/base/p2pconstants.h"
#include "webrtc/p2p/base/portallocator.h"
#include "webrtc/p2p/base/tcpport.h"
#include "webrtc/p2p/base/testturnserver.h"
#include "webrtc/p2p/base/turnport.h"
#include "webrtc/p2p/base/udpport.h"
#include "webrtc/base/asynctcpsocket.h"
#include "webrtc/base/buffer.h"
#include "webrtc/base/dscp.h"
#include "webrtc/base/fakeclock.h"
#include "webrtc/base/firewallsocketserver.h"
#include "webrtc/base/gunit.h"
#include "webrtc/base/helpers.h"
#include "webrtc/base/logging.h"
#include "webrtc/base/physicalsocketserver.h"
#include "webrtc/base/socketadapters.h"
#include "webrtc/base/socketaddress.h"
#include "webrtc/base/ssladapter.h"
#include "webrtc/base/thread.h"
#include "webrtc/base/virtualsocketserver.h"

using rtc::SocketAddress;

static const SocketAddress kLocalAddr1("11.11.11.11", 0);
static const SocketAddress kLocalAddr2("22.22.22.22", 0);
static const SocketAddress kLocalIPv6Addr(
    "2401:fa00:4:1000:be30:5bff:fee5:c3", 0);
static const SocketAddress kLocalIPv6Addr2(
    "2401:fa00:4:2000:be30:5bff:fee5:d4", 0);
static const SocketAddress kTurnUdpIntAddr("99.99.99.3",
                                           cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnTcpIntAddr("99.99.99.4",
                                           cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnUdpExtAddr("99.99.99.5", 0);
static const SocketAddress kTurnAlternateIntAddr("99.99.99.6",
                                                 cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnIntAddr("99.99.99.7",
                                        cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnIPv6IntAddr(
    "2400:4030:2:2c00:be30:abcd:efab:cdef",
    cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnUdpIPv6IntAddr(
    "2400:4030:1:2c00:be30:abcd:efab:cdef", cricket::TURN_SERVER_PORT);

static const char kCandidateFoundation[] = "foundation";
static const char kIceUfrag1[] = "TESTICEUFRAG0001";
static const char kIceUfrag2[] = "TESTICEUFRAG0002";
static const char kIcePwd1[] = "TESTICEPWD00000000000001";
static const char kIcePwd2[] = "TESTICEPWD00000000000002";
static const char kTurnUsername[] = "test";
static const char kTurnPassword[] = "test";
static const char kTestOrigin[] = "http://example.com";
// This test configures the virtual socket server to simulate delay so that we
// can verify operations take no more than the expected number of round trips.
static constexpr unsigned int kSimulatedRtt = 50;
// Connection destruction may happen asynchronously, but it should only
// take one simulated clock tick.
static constexpr unsigned int kConnectionDestructionDelay = 1;
// This used to be 1 second, but that's not always enough for getaddrinfo().
// See: https://bugs.chromium.org/p/webrtc/issues/detail?id=5191
static constexpr unsigned int kResolverTimeout = 10000;

static const cricket::ProtocolAddress kTurnUdpProtoAddr(
    kTurnUdpIntAddr, cricket::PROTO_UDP);
static const cricket::ProtocolAddress kTurnTcpProtoAddr(
    kTurnTcpIntAddr, cricket::PROTO_TCP);
static const cricket::ProtocolAddress kTurnTlsProtoAddr(kTurnTcpIntAddr,
                                                        cricket::PROTO_TLS);
static const cricket::ProtocolAddress kTurnUdpIPv6ProtoAddr(
    kTurnUdpIPv6IntAddr, cricket::PROTO_UDP);

static const unsigned int MSG_TESTFINISH = 0;

#if defined(WEBRTC_LINUX) && !defined(WEBRTC_ANDROID)
static int GetFDCount() {
  struct dirent *dp;
  int fd_count = 0;
  DIR *dir = opendir("/proc/self/fd/");
  while ((dp = readdir(dir)) != NULL) {
    if (dp->d_name[0] == '.')
      continue;
    ++fd_count;
  }
  closedir(dir);
  return fd_count;
}
#endif

namespace cricket {

class TurnPortTestVirtualSocketServer : public rtc::VirtualSocketServer {
 public:
  explicit TurnPortTestVirtualSocketServer(SocketServer* ss)
      : VirtualSocketServer(ss) {
    // This configures the virtual socket server to always add a simulated
    // delay of exactly half of kSimulatedRtt.
    set_delay_mean(kSimulatedRtt / 2);
    UpdateDelayDistribution();
  }

  using rtc::VirtualSocketServer::LookupBinding;
};

class TestConnectionWrapper : public sigslot::has_slots<> {
 public:
  TestConnectionWrapper(Connection* conn) : connection_(conn) {
    conn->SignalDestroyed.connect(
        this, &TestConnectionWrapper::OnConnectionDestroyed);
  }

  Connection* connection() { return connection_; }

 private:
  void OnConnectionDestroyed(Connection* conn) {
    ASSERT_TRUE(conn == connection_);
    connection_ = nullptr;
  }

  Connection* connection_;
};

// Note: This test uses a fake clock with a simulated network round trip
// (between local port and TURN server) of kSimulatedRtt.
class TurnPortTest : public testing::Test,
                     public sigslot::has_slots<>,
                     public rtc::MessageHandler {
 public:
  TurnPortTest()
      : main_(rtc::Thread::Current()),
        pss_(new rtc::PhysicalSocketServer),
        ss_(new TurnPortTestVirtualSocketServer(pss_.get())),
        ss_scope_(ss_.get()),
        network_("unittest", "unittest", rtc::IPAddress(INADDR_ANY), 32),
        socket_factory_(rtc::Thread::Current()),
        turn_server_(main_, kTurnUdpIntAddr, kTurnUdpExtAddr),
        turn_ready_(false),
        turn_error_(false),
        turn_unknown_address_(false),
        turn_create_permission_success_(false),
        udp_ready_(false),
        test_finish_(false) {
    // Some code uses "last received time == 0" to represent "nothing received
    // so far", so we need to start the fake clock at a nonzero time...
    // TODO(deadbeef): Fix this.
    fake_clock_.AdvanceTime(rtc::TimeDelta::FromSeconds(1));
    network_.AddIP(rtc::IPAddress(INADDR_ANY));
  }

  virtual void OnMessage(rtc::Message* msg) {
    ASSERT(msg->message_id == MSG_TESTFINISH);
    if (msg->message_id == MSG_TESTFINISH)
      test_finish_ = true;
  }

  void ConnectSignalAddressReadyToSetLocalhostAsAltenertativeLocalAddress() {
    rtc::AsyncPacketSocket* socket = turn_port_->socket();
    rtc::VirtualSocket* virtual_socket =
        ss_->LookupBinding(socket->GetLocalAddress());
    virtual_socket->SignalAddressReady.connect(
        this, &TurnPortTest::SetLocalhostAsAltenertativeLocalAddress);
  }

  void SetLocalhostAsAltenertativeLocalAddress(
      rtc::VirtualSocket* socket,
      const rtc::SocketAddress& address) {
    SocketAddress local_address("127.0.0.1", 2000);
    socket->SetAlternativeLocalAddress(local_address);
  }

  void OnTurnPortComplete(Port* port) {
    turn_ready_ = true;
  }
  void OnTurnPortError(Port* port) {
    turn_error_ = true;
  }
  void OnTurnUnknownAddress(PortInterface* port,
                            const SocketAddress& addr,
                            ProtocolType proto,
                            IceMessage* msg,
                            const std::string& rf,
                            bool /*port_muxed*/) {
    turn_unknown_address_ = true;
  }
  void OnTurnCreatePermissionResult(TurnPort* port,
                                    const SocketAddress& addr,
                                    int code) {
    // Ignoring the address.
    turn_create_permission_success_ = (code == 0);
  }

  void OnTurnRefreshResult(TurnPort* port, int code) {
    turn_refresh_success_ = (code == 0);
  }
  void OnTurnReadPacket(Connection* conn, const char* data, size_t size,
                        const rtc::PacketTime& packet_time) {
    turn_packets_.push_back(rtc::Buffer(data, size));
  }
  void OnUdpPortComplete(Port* port) {
    udp_ready_ = true;
  }
  void OnUdpReadPacket(Connection* conn, const char* data, size_t size,
                       const rtc::PacketTime& packet_time) {
    udp_packets_.push_back(rtc::Buffer(data, size));
  }
  void OnSocketReadPacket(rtc::AsyncPacketSocket* socket,
                          const char* data, size_t size,
                          const rtc::SocketAddress& remote_addr,
                          const rtc::PacketTime& packet_time) {
    turn_port_->HandleIncomingPacket(socket, data, size, remote_addr,
                                     packet_time);
  }
  rtc::AsyncSocket* CreateServerSocket(const SocketAddress addr) {
    rtc::AsyncSocket* socket = ss_->CreateAsyncSocket(SOCK_STREAM);
    EXPECT_GE(socket->Bind(addr), 0);
    EXPECT_GE(socket->Listen(5), 0);
    return socket;
  }

  void CreateTurnPort(const std::string& username,
                      const std::string& password,
                      const ProtocolAddress& server_address) {
    CreateTurnPort(kLocalAddr1, username, password, server_address);
  }
  void CreateTurnPort(const rtc::SocketAddress& local_address,
                      const std::string& username,
                      const std::string& password,
                      const ProtocolAddress& server_address) {
    RelayCredentials credentials(username, password);
    turn_port_.reset(TurnPort::Create(main_, &socket_factory_, &network_,
                                 local_address.ipaddr(), 0, 0,
                                 kIceUfrag1, kIcePwd1,
                                 server_address, credentials, 0,
                                 std::string()));
    // This TURN port will be the controlling.
    turn_port_->SetIceRole(ICEROLE_CONTROLLING);
    ConnectSignals();
  }

  // Should be identical to CreateTurnPort but specifies an origin value
  // when creating the instance of TurnPort.
  void CreateTurnPortWithOrigin(const rtc::SocketAddress& local_address,
                                const std::string& username,
                                const std::string& password,
                                const ProtocolAddress& server_address,
                                const std::string& origin) {
    RelayCredentials credentials(username, password);
    turn_port_.reset(TurnPort::Create(main_, &socket_factory_, &network_,
                                 local_address.ipaddr(), 0, 0,
                                 kIceUfrag1, kIcePwd1,
                                 server_address, credentials, 0,
                                 origin));
    // This TURN port will be the controlling.
    turn_port_->SetIceRole(ICEROLE_CONTROLLING);
    ConnectSignals();
  }

  void CreateSharedTurnPort(const std::string& username,
                            const std::string& password,
                            const ProtocolAddress& server_address) {
    ASSERT(server_address.proto == PROTO_UDP);

    if (!socket_) {
      socket_.reset(socket_factory_.CreateUdpSocket(
          rtc::SocketAddress(kLocalAddr1.ipaddr(), 0), 0, 0));
      ASSERT_TRUE(socket_ != NULL);
      socket_->SignalReadPacket.connect(
          this, &TurnPortTest::OnSocketReadPacket);
    }

    RelayCredentials credentials(username, password);
    turn_port_.reset(TurnPort::Create(
        main_, &socket_factory_, &network_, socket_.get(), kIceUfrag1, kIcePwd1,
        server_address, credentials, 0, std::string()));
    // This TURN port will be the controlling.
    turn_port_->SetIceRole(ICEROLE_CONTROLLING);
    ConnectSignals();
  }

  void ConnectSignals() {
    turn_port_->SignalPortComplete.connect(this,
        &TurnPortTest::OnTurnPortComplete);
    turn_port_->SignalPortError.connect(this,
        &TurnPortTest::OnTurnPortError);
    turn_port_->SignalUnknownAddress.connect(this,
        &TurnPortTest::OnTurnUnknownAddress);
    turn_port_->SignalCreatePermissionResult.connect(this,
        &TurnPortTest::OnTurnCreatePermissionResult);
    turn_port_->SignalTurnRefreshResult.connect(
        this, &TurnPortTest::OnTurnRefreshResult);
  }

  void CreateUdpPort() { CreateUdpPort(kLocalAddr2); }

  void CreateUdpPort(const SocketAddress& address) {
    udp_port_.reset(UDPPort::Create(main_, &socket_factory_, &network_,
                                    address.ipaddr(), 0, 0, kIceUfrag2,
                                    kIcePwd2, std::string(), false));
    // UDP port will be controlled.
    udp_port_->SetIceRole(ICEROLE_CONTROLLED);
    udp_port_->SignalPortComplete.connect(
        this, &TurnPortTest::OnUdpPortComplete);
  }

  void PrepareTurnAndUdpPorts(ProtocolType protocol_type) {
    // turn_port_ should have been created.
    ASSERT_TRUE(turn_port_ != nullptr);
    turn_port_->PrepareAddress();
    // Two round trips are required to allocate a TURN candidate.
    // Plus, an extra round trip is needed for TCP.
    ASSERT_TRUE_SIMULATED_WAIT(
        turn_ready_,
        protocol_type == PROTO_TCP ? kSimulatedRtt * 3 : kSimulatedRtt * 2,
        fake_clock_);

    CreateUdpPort();
    udp_port_->PrepareAddress();
    ASSERT_TRUE_SIMULATED_WAIT(udp_ready_, kSimulatedRtt, fake_clock_);
  }

  bool CheckConnectionFailedAndPruned(Connection* conn) {
    return conn && !conn->active() &&
           conn->state() == IceCandidatePairState::FAILED;
  }

  // Checks that |turn_port_| has a nonempty set of connections and they are all
  // failed and pruned.
  bool CheckAllConnectionsFailedAndPruned() {
    auto& connections = turn_port_->connections();
    if (connections.empty()) {
      return false;
    }
    for (auto kv : connections) {
      if (!CheckConnectionFailedAndPruned(kv.second)) {
        return false;
      }
    }
    return true;
  }

  void TestTurnAlternateServer(ProtocolType protocol_type) {
    std::vector<rtc::SocketAddress> redirect_addresses;
    redirect_addresses.push_back(kTurnAlternateIntAddr);

    TestTurnRedirector redirector(redirect_addresses);

    turn_server_.AddInternalSocket(kTurnIntAddr, protocol_type);
    turn_server_.AddInternalSocket(kTurnAlternateIntAddr, protocol_type);
    turn_server_.set_redirect_hook(&redirector);
    CreateTurnPort(kTurnUsername, kTurnPassword,
                   ProtocolAddress(kTurnIntAddr, protocol_type));

    // Retrieve the address before we run the state machine.
    const SocketAddress old_addr = turn_port_->server_address().address;

    turn_port_->PrepareAddress();
    // Extra round trip due to "use alternate server" error response.
    EXPECT_TRUE_SIMULATED_WAIT(
        turn_ready_,
        (protocol_type == PROTO_TCP ? kSimulatedRtt * 4 : kSimulatedRtt * 3),
        fake_clock_);
    // Retrieve the address again, the turn port's address should be
    // changed.
    const SocketAddress new_addr = turn_port_->server_address().address;
    EXPECT_NE(old_addr, new_addr);
    ASSERT_EQ(1U, turn_port_->Candidates().size());
    EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
              turn_port_->Candidates()[0].address().ipaddr());
    EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
  }

  void TestTurnAlternateServerV4toV6(ProtocolType protocol_type) {
    std::vector<rtc::SocketAddress> redirect_addresses;
    redirect_addresses.push_back(kTurnIPv6IntAddr);

    TestTurnRedirector redirector(redirect_addresses);
    turn_server_.AddInternalSocket(kTurnIntAddr, protocol_type);
    turn_server_.set_redirect_hook(&redirector);
    CreateTurnPort(kTurnUsername, kTurnPassword,
                   ProtocolAddress(kTurnIntAddr, protocol_type));
    turn_port_->PrepareAddress();
    EXPECT_TRUE_SIMULATED_WAIT(turn_error_, kSimulatedRtt * 2, fake_clock_);
  }

  void TestTurnAlternateServerPingPong(ProtocolType protocol_type) {
    std::vector<rtc::SocketAddress> redirect_addresses;
    redirect_addresses.push_back(kTurnAlternateIntAddr);
    redirect_addresses.push_back(kTurnIntAddr);

    TestTurnRedirector redirector(redirect_addresses);

    turn_server_.AddInternalSocket(kTurnIntAddr, protocol_type);
    turn_server_.AddInternalSocket(kTurnAlternateIntAddr, protocol_type);
    turn_server_.set_redirect_hook(&redirector);
    CreateTurnPort(kTurnUsername, kTurnPassword,
                   ProtocolAddress(kTurnIntAddr, protocol_type));

    turn_port_->PrepareAddress();
    // Extra round trip due to "use alternate server" error response.
    EXPECT_TRUE_SIMULATED_WAIT(
        turn_error_,
        (protocol_type == PROTO_TCP ? kSimulatedRtt * 4 : kSimulatedRtt * 3),
        fake_clock_);
    ASSERT_EQ(0U, turn_port_->Candidates().size());
    rtc::SocketAddress address;
    // Verify that we have exhausted all alternate servers instead of
    // failure caused by other errors.
    EXPECT_FALSE(redirector.ShouldRedirect(address, &address));
  }

  void TestTurnAlternateServerDetectRepetition(ProtocolType protocol_type) {
    std::vector<rtc::SocketAddress> redirect_addresses;
    redirect_addresses.push_back(kTurnAlternateIntAddr);
    redirect_addresses.push_back(kTurnAlternateIntAddr);

    TestTurnRedirector redirector(redirect_addresses);

    turn_server_.AddInternalSocket(kTurnIntAddr, protocol_type);
    turn_server_.AddInternalSocket(kTurnAlternateIntAddr, protocol_type);
    turn_server_.set_redirect_hook(&redirector);
    CreateTurnPort(kTurnUsername, kTurnPassword,
                   ProtocolAddress(kTurnIntAddr, protocol_type));

    turn_port_->PrepareAddress();
    // Extra round trip due to "use alternate server" error response.
    EXPECT_TRUE_SIMULATED_WAIT(
        turn_error_,
        (protocol_type == PROTO_TCP ? kSimulatedRtt * 4 : kSimulatedRtt * 3),
        fake_clock_);
    ASSERT_EQ(0U, turn_port_->Candidates().size());
  }

  // A certain security exploit works by redirecting to a loopback address,
  // which doesn't ever actually make sense. So redirects to loopback should
  // be treated as errors.
  // See: https://bugs.chromium.org/p/chromium/issues/detail?id=649118
  void TestTurnAlternateServerLoopback(ProtocolType protocol_type, bool ipv6) {
    const SocketAddress& local_address = ipv6 ? kLocalIPv6Addr : kLocalAddr1;
    const SocketAddress& server_address =
        ipv6 ? kTurnIPv6IntAddr : kTurnIntAddr;

    std::vector<rtc::SocketAddress> redirect_addresses;
    // Pick an unusual address in the 127.0.0.0/8 range to make sure more than
    // 127.0.0.1 is covered.
    SocketAddress loopback_address(ipv6 ? "::1" : "127.1.2.3",
                                   TURN_SERVER_PORT);
    redirect_addresses.push_back(loopback_address);

    // Make a socket and bind it to the local port, to make extra sure no
    // packet is sent to this address.
    std::unique_ptr<rtc::Socket> loopback_socket(ss_->CreateSocket(
        protocol_type == PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM));
    ASSERT_NE(nullptr, loopback_socket.get());
    ASSERT_EQ(0, loopback_socket->Bind(loopback_address));
    if (protocol_type == PROTO_TCP) {
      ASSERT_EQ(0, loopback_socket->Listen(1));
    }

    TestTurnRedirector redirector(redirect_addresses);

    turn_server_.AddInternalSocket(server_address, protocol_type);
    turn_server_.set_redirect_hook(&redirector);
    CreateTurnPort(local_address, kTurnUsername, kTurnPassword,
                   ProtocolAddress(server_address, protocol_type));

    turn_port_->PrepareAddress();
    EXPECT_TRUE_SIMULATED_WAIT(
        turn_error_,
        (protocol_type == PROTO_TCP ? kSimulatedRtt * 3 : kSimulatedRtt * 2),
        fake_clock_);

    // Wait for some extra time, and make sure no packets were received on the
    // loopback port we created (or in the case of TCP, no connection attempt
    // occurred).
    SIMULATED_WAIT(false, kSimulatedRtt, fake_clock_);
    if (protocol_type == PROTO_UDP) {
      char buf[1];
      EXPECT_EQ(-1, loopback_socket->Recv(&buf, 1, nullptr));
    } else {
      std::unique_ptr<rtc::Socket> accepted_socket(
          loopback_socket->Accept(nullptr));
      EXPECT_EQ(nullptr, accepted_socket.get());
    }
  }

  void TestTurnConnection(ProtocolType protocol_type) {
    // Create ports and prepare addresses.
    PrepareTurnAndUdpPorts(protocol_type);

    // Send ping from UDP to TURN.
    Connection* conn1 = udp_port_->CreateConnection(
                    turn_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    ASSERT_TRUE(conn1 != NULL);
    conn1->Ping(0);
    SIMULATED_WAIT(!turn_unknown_address_, kSimulatedRtt * 2, fake_clock_);
    EXPECT_FALSE(turn_unknown_address_);
    EXPECT_FALSE(conn1->receiving());
    EXPECT_EQ(Connection::STATE_WRITE_INIT, conn1->write_state());

    // Send ping from TURN to UDP.
    Connection* conn2 = turn_port_->CreateConnection(
                    udp_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    ASSERT_TRUE(conn2 != NULL);
    ASSERT_TRUE_SIMULATED_WAIT(turn_create_permission_success_, kSimulatedRtt,
                               fake_clock_);
    conn2->Ping(0);

    // Two hops from TURN port to UDP port through TURN server, thus two RTTs.
    EXPECT_EQ_SIMULATED_WAIT(Connection::STATE_WRITABLE, conn2->write_state(),
                             kSimulatedRtt * 2, fake_clock_);
    EXPECT_TRUE(conn1->receiving());
    EXPECT_TRUE(conn2->receiving());
    EXPECT_EQ(Connection::STATE_WRITE_INIT, conn1->write_state());

    // Send another ping from UDP to TURN.
    conn1->Ping(0);
    EXPECT_EQ_SIMULATED_WAIT(Connection::STATE_WRITABLE, conn1->write_state(),
                             kSimulatedRtt * 2, fake_clock_);
    EXPECT_TRUE(conn2->receiving());
  }

  void TestDestroyTurnConnection() {
    PrepareTurnAndUdpPorts(PROTO_UDP);

    // Create connections on both ends.
    Connection* conn1 = udp_port_->CreateConnection(turn_port_->Candidates()[0],
                                                    Port::ORIGIN_MESSAGE);
    Connection* conn2 = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                                     Port::ORIGIN_MESSAGE);
    ASSERT_TRUE(conn2 != NULL);
    ASSERT_TRUE_SIMULATED_WAIT(turn_create_permission_success_, kSimulatedRtt,
                               fake_clock_);
    // Make sure turn connection can receive.
    conn1->Ping(0);
    EXPECT_EQ_SIMULATED_WAIT(Connection::STATE_WRITABLE, conn1->write_state(),
                             kSimulatedRtt * 2, fake_clock_);
    EXPECT_FALSE(turn_unknown_address_);

    // Destroy the connection on the TURN port. The TurnEntry still exists, so
    // the TURN port should still process a ping from an unknown address.
    conn2->Destroy();
    conn1->Ping(0);
    EXPECT_TRUE_SIMULATED_WAIT(turn_unknown_address_, kSimulatedRtt,
                               fake_clock_);

    // Flush all requests in the invoker to destroy the TurnEntry.
    // Expect that it still processes an incoming ping and signals the
    // unknown address.
    turn_unknown_address_ = false;
    turn_port_->invoker()->Flush(rtc::Thread::Current());
    conn1->Ping(0);
    EXPECT_TRUE_SIMULATED_WAIT(turn_unknown_address_, kSimulatedRtt,
                               fake_clock_);

    // If the connection is created again, it will start to receive pings.
    conn2 = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                         Port::ORIGIN_MESSAGE);
    conn1->Ping(0);
    EXPECT_TRUE_SIMULATED_WAIT(conn2->receiving(), kSimulatedRtt, fake_clock_);
  }

  void TestTurnSendData(ProtocolType protocol_type) {
    PrepareTurnAndUdpPorts(protocol_type);

    // Create connections and send pings.
    Connection* conn1 = turn_port_->CreateConnection(
        udp_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    Connection* conn2 = udp_port_->CreateConnection(
        turn_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    ASSERT_TRUE(conn1 != NULL);
    ASSERT_TRUE(conn2 != NULL);
    conn1->SignalReadPacket.connect(static_cast<TurnPortTest*>(this),
                                    &TurnPortTest::OnTurnReadPacket);
    conn2->SignalReadPacket.connect(static_cast<TurnPortTest*>(this),
                                    &TurnPortTest::OnUdpReadPacket);
    conn1->Ping(0);
    EXPECT_EQ_SIMULATED_WAIT(Connection::STATE_WRITABLE, conn1->write_state(),
                             kSimulatedRtt * 2, fake_clock_);
    conn2->Ping(0);
    EXPECT_EQ_SIMULATED_WAIT(Connection::STATE_WRITABLE, conn2->write_state(),
                             kSimulatedRtt * 2, fake_clock_);

    // Send some data.
    size_t num_packets = 256;
    for (size_t i = 0; i < num_packets; ++i) {
      unsigned char buf[256] = { 0 };
      for (size_t j = 0; j < i + 1; ++j) {
        buf[j] = 0xFF - static_cast<unsigned char>(j);
      }
      conn1->Send(buf, i + 1, options);
      conn2->Send(buf, i + 1, options);
      SIMULATED_WAIT(false, kSimulatedRtt, fake_clock_);
    }

    // Check the data.
    ASSERT_EQ(num_packets, turn_packets_.size());
    ASSERT_EQ(num_packets, udp_packets_.size());
    for (size_t i = 0; i < num_packets; ++i) {
      EXPECT_EQ(i + 1, turn_packets_[i].size());
      EXPECT_EQ(i + 1, udp_packets_[i].size());
      EXPECT_EQ(turn_packets_[i], udp_packets_[i]);
    }
  }

 protected:
  rtc::ScopedFakeClock fake_clock_;
  rtc::Thread* main_;
  std::unique_ptr<rtc::PhysicalSocketServer> pss_;
  std::unique_ptr<TurnPortTestVirtualSocketServer> ss_;
  rtc::SocketServerScope ss_scope_;
  rtc::Network network_;
  rtc::BasicPacketSocketFactory socket_factory_;
  std::unique_ptr<rtc::AsyncPacketSocket> socket_;
  TestTurnServer turn_server_;
  std::unique_ptr<TurnPort> turn_port_;
  std::unique_ptr<UDPPort> udp_port_;
  bool turn_ready_;
  bool turn_error_;
  bool turn_unknown_address_;
  bool turn_create_permission_success_;
  bool udp_ready_;
  bool test_finish_;
  bool turn_refresh_success_ = false;
  std::vector<rtc::Buffer> turn_packets_;
  std::vector<rtc::Buffer> udp_packets_;
  rtc::PacketOptions options;
};

TEST_F(TurnPortTest, TestTurnPortType) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  EXPECT_EQ(cricket::RELAY_PORT_TYPE, turn_port_->Type());
}

// Do a normal TURN allocation.
TEST_F(TurnPortTest, TestTurnAllocate) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  EXPECT_EQ(0, turn_port_->SetOption(rtc::Socket::OPT_SNDBUF, 10*1024));
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);
  ASSERT_EQ(1U, turn_port_->Candidates().size());
  EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
            turn_port_->Candidates()[0].address().ipaddr());
  EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
}

// Testing a normal UDP allocation using TCP connection.
TEST_F(TurnPortTest, TestTurnTcpAllocate) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  EXPECT_EQ(0, turn_port_->SetOption(rtc::Socket::OPT_SNDBUF, 10*1024));
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 3, fake_clock_);
  ASSERT_EQ(1U, turn_port_->Candidates().size());
  EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
            turn_port_->Candidates()[0].address().ipaddr());
  EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
}

// Test case for WebRTC issue 3927 where a proxy binds to the local host address
// instead the address that TurnPort originally bound to. The candidate pair
// impacted by this behavior should still be used.
TEST_F(TurnPortTest, TestTurnTcpAllocationWhenProxyChangesAddressToLocalHost) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  EXPECT_EQ(0, turn_port_->SetOption(rtc::Socket::OPT_SNDBUF, 10 * 1024));
  turn_port_->PrepareAddress();
  ConnectSignalAddressReadyToSetLocalhostAsAltenertativeLocalAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 3, fake_clock_);
  ASSERT_EQ(1U, turn_port_->Candidates().size());
  EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
            turn_port_->Candidates()[0].address().ipaddr());
  EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
}

// Testing turn port will attempt to create TCP socket on address resolution
// failure.
TEST_F(TurnPortTest, TestTurnTcpOnAddressResolveFailure) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword,
                 ProtocolAddress(rtc::SocketAddress("www.google.invalid", 3478),
                                 PROTO_TCP));
  turn_port_->PrepareAddress();
  EXPECT_TRUE_WAIT(turn_error_, kResolverTimeout);
  // As VSS doesn't provide a DNS resolution, name resolve will fail. TurnPort
  // will proceed in creating a TCP socket which will fail as there is no
  // server on the above domain and error will be set to SOCKET_ERROR.
  EXPECT_EQ(SOCKET_ERROR, turn_port_->error());
}

// In case of UDP on address resolve failure, TurnPort will not create socket
// and return allocate failure.
TEST_F(TurnPortTest, TestTurnUdpOnAddressResolveFailure) {
  CreateTurnPort(kTurnUsername, kTurnPassword,
                 ProtocolAddress(rtc::SocketAddress("www.google.invalid", 3478),
                                 PROTO_UDP));
  turn_port_->PrepareAddress();
  EXPECT_TRUE_WAIT(turn_error_, kResolverTimeout);
  // Error from turn port will not be socket error.
  EXPECT_NE(SOCKET_ERROR, turn_port_->error());
}

// Try to do a TURN allocation with an invalid password.
TEST_F(TurnPortTest, TestTurnAllocateBadPassword) {
  CreateTurnPort(kTurnUsername, "bad", kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_error_, kSimulatedRtt * 2, fake_clock_);
  ASSERT_EQ(0U, turn_port_->Candidates().size());
}

// Tests that TURN port nonce will be reset when receiving an ALLOCATE MISMATCH
// error.
TEST_F(TurnPortTest, TestTurnAllocateNonceResetAfterAllocateMismatch) {
  // Do a normal allocation first.
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);
  rtc::SocketAddress first_addr(turn_port_->socket()->GetLocalAddress());
  // Destroy the turnport while keeping the drop probability to 1 to
  // suppress the release of the allocation at the server.
  ss_->set_drop_probability(1.0);
  turn_port_.reset();
  SIMULATED_WAIT(false, kSimulatedRtt, fake_clock_);
  ss_->set_drop_probability(0.0);

  // Force the socket server to assign the same port.
  ss_->SetNextPortForTesting(first_addr.port());
  turn_ready_ = false;
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);

  // It is expected that the turn port will first get a nonce from the server
  // using timestamp |ts_before| but then get an allocate mismatch error and
  // receive an even newer nonce based on the system clock. |ts_before| is
  // chosen so that the two NONCEs generated by the server will be different.
  int64_t ts_before = rtc::TimeMillis() - 1;
  std::string first_nonce =
      turn_server_.server()->SetTimestampForNextNonce(ts_before);
  turn_port_->PrepareAddress();

  // Four round trips; first we'll get "stale nonce", then
  // "allocate mismatch", then "stale nonce" again, then finally it will
  // succeed.
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 4, fake_clock_);
  EXPECT_NE(first_nonce, turn_port_->nonce());
}

// Tests that a new local address is created after
// STUN_ERROR_ALLOCATION_MISMATCH.
TEST_F(TurnPortTest, TestTurnAllocateMismatch) {
  // Do a normal allocation first.
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);
  rtc::SocketAddress first_addr(turn_port_->socket()->GetLocalAddress());

  // Clear connected_ flag on turnport to suppress the release of
  // the allocation.
  turn_port_->OnSocketClose(turn_port_->socket(), 0);

  // Forces the socket server to assign the same port.
  ss_->SetNextPortForTesting(first_addr.port());

  turn_ready_ = false;
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();

  // Verifies that the new port has the same address.
  EXPECT_EQ(first_addr, turn_port_->socket()->GetLocalAddress());

  // Four round trips; first we'll get "stale nonce", then
  // "allocate mismatch", then "stale nonce" again, then finally it will
  // succeed.
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 4, fake_clock_);

  // Verifies that the new port has a different address now.
  EXPECT_NE(first_addr, turn_port_->socket()->GetLocalAddress());

  // Verify that all packets received from the shared socket are ignored.
  std::string test_packet = "Test packet";
  EXPECT_FALSE(turn_port_->HandleIncomingPacket(
      socket_.get(), test_packet.data(), test_packet.size(),
      rtc::SocketAddress(kTurnUdpExtAddr.ipaddr(), 0),
      rtc::CreatePacketTime(0)));
}

// Tests that a shared-socket-TurnPort creates its own socket after
// STUN_ERROR_ALLOCATION_MISMATCH.
TEST_F(TurnPortTest, TestSharedSocketAllocateMismatch) {
  // Do a normal allocation first.
  CreateSharedTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);
  rtc::SocketAddress first_addr(turn_port_->socket()->GetLocalAddress());

  // Clear connected_ flag on turnport to suppress the release of
  // the allocation.
  turn_port_->OnSocketClose(turn_port_->socket(), 0);

  turn_ready_ = false;
  CreateSharedTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);

  // Verifies that the new port has the same address.
  EXPECT_EQ(first_addr, turn_port_->socket()->GetLocalAddress());
  EXPECT_TRUE(turn_port_->SharedSocket());

  turn_port_->PrepareAddress();
  // Extra 2 round trips due to allocate mismatch.
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 4, fake_clock_);

  // Verifies that the new port has a different address now.
  EXPECT_NE(first_addr, turn_port_->socket()->GetLocalAddress());
  EXPECT_FALSE(turn_port_->SharedSocket());
}

TEST_F(TurnPortTest, TestTurnTcpAllocateMismatch) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);

  // Do a normal allocation first.
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 3, fake_clock_);
  rtc::SocketAddress first_addr(turn_port_->socket()->GetLocalAddress());

  // Clear connected_ flag on turnport to suppress the release of
  // the allocation.
  turn_port_->OnSocketClose(turn_port_->socket(), 0);

  // Forces the socket server to assign the same port.
  ss_->SetNextPortForTesting(first_addr.port());

  turn_ready_ = false;
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  turn_port_->PrepareAddress();

  // Verifies that the new port has the same address.
  EXPECT_EQ(first_addr, turn_port_->socket()->GetLocalAddress());

  // Extra 2 round trips due to allocate mismatch.
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 5, fake_clock_);

  // Verifies that the new port has a different address now.
  EXPECT_NE(first_addr, turn_port_->socket()->GetLocalAddress());
}

TEST_F(TurnPortTest, TestRefreshRequestGetsErrorResponse) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  PrepareTurnAndUdpPorts(PROTO_UDP);
  turn_port_->CreateConnection(udp_port_->Candidates()[0],
                               Port::ORIGIN_MESSAGE);
  // Set bad credentials.
  RelayCredentials bad_credentials("bad_user", "bad_pwd");
  turn_port_->set_credentials(bad_credentials);
  turn_refresh_success_ = false;
  // This sends out the first RefreshRequest with correct credentials.
  // When this succeeds, it will schedule a new RefreshRequest with the bad
  // credential.
  turn_port_->FlushRequests(TURN_REFRESH_REQUEST);
  EXPECT_TRUE_SIMULATED_WAIT(turn_refresh_success_, kSimulatedRtt, fake_clock_);
  // Flush it again, it will receive a bad response.
  turn_port_->FlushRequests(TURN_REFRESH_REQUEST);
  EXPECT_TRUE_SIMULATED_WAIT(!turn_refresh_success_, kSimulatedRtt,
                             fake_clock_);
  EXPECT_FALSE(turn_port_->connected());
  EXPECT_TRUE(CheckAllConnectionsFailedAndPruned());
  EXPECT_FALSE(turn_port_->HasRequests());
}

// Test that TurnPort will not handle any incoming packets once it has been
// closed.
TEST_F(TurnPortTest, TestStopProcessingPacketsAfterClosed) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  PrepareTurnAndUdpPorts(PROTO_UDP);
  Connection* conn1 = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                                   Port::ORIGIN_MESSAGE);
  Connection* conn2 = udp_port_->CreateConnection(turn_port_->Candidates()[0],
                                                  Port::ORIGIN_MESSAGE);
  ASSERT_TRUE(conn1 != NULL);
  ASSERT_TRUE(conn2 != NULL);
  // Make sure conn2 is writable.
  conn2->Ping(0);
  EXPECT_EQ_SIMULATED_WAIT(Connection::STATE_WRITABLE, conn2->write_state(),
                           kSimulatedRtt * 2, fake_clock_);

  turn_port_->Close();
  SIMULATED_WAIT(false, kSimulatedRtt, fake_clock_);
  turn_unknown_address_ = false;
  conn2->Ping(0);
  SIMULATED_WAIT(false, kSimulatedRtt, fake_clock_);
  // Since the turn port does not handle packets any more, it should not
  // SignalUnknownAddress.
  EXPECT_FALSE(turn_unknown_address_);
}

// Test that CreateConnection will return null if port becomes disconnected.
TEST_F(TurnPortTest, TestCreateConnectionWhenSocketClosed) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  PrepareTurnAndUdpPorts(PROTO_TCP);
  // Create a connection.
  Connection* conn1 = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                                   Port::ORIGIN_MESSAGE);
  ASSERT_TRUE(conn1 != NULL);

  // Close the socket and create a connection again.
  turn_port_->OnSocketClose(turn_port_->socket(), 1);
  conn1 = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                       Port::ORIGIN_MESSAGE);
  ASSERT_TRUE(conn1 == NULL);
}

// Tests that when a TCP socket is closed, the respective TURN connection will
// be destroyed.
TEST_F(TurnPortTest, TestSocketCloseWillDestroyConnection) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  PrepareTurnAndUdpPorts(PROTO_TCP);
  Connection* conn = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                                  Port::ORIGIN_MESSAGE);
  EXPECT_NE(nullptr, conn);
  EXPECT_TRUE(!turn_port_->connections().empty());
  turn_port_->socket()->SignalClose(turn_port_->socket(), 1);
  EXPECT_TRUE_SIMULATED_WAIT(turn_port_->connections().empty(),
                             kConnectionDestructionDelay, fake_clock_);
}

// Test try-alternate-server feature.
TEST_F(TurnPortTest, TestTurnAlternateServerUDP) {
  TestTurnAlternateServer(PROTO_UDP);
}

TEST_F(TurnPortTest, TestTurnAlternateServerTCP) {
  TestTurnAlternateServer(PROTO_TCP);
}

// Test that we fail when we redirect to an address different from
// current IP family.
TEST_F(TurnPortTest, TestTurnAlternateServerV4toV6UDP) {
  TestTurnAlternateServerV4toV6(PROTO_UDP);
}

TEST_F(TurnPortTest, TestTurnAlternateServerV4toV6TCP) {
  TestTurnAlternateServerV4toV6(PROTO_TCP);
}

// Test try-alternate-server catches the case of pingpong.
TEST_F(TurnPortTest, TestTurnAlternateServerPingPongUDP) {
  TestTurnAlternateServerPingPong(PROTO_UDP);
}

TEST_F(TurnPortTest, TestTurnAlternateServerPingPongTCP) {
  TestTurnAlternateServerPingPong(PROTO_TCP);
}

// Test try-alternate-server catch the case of repeated server.
TEST_F(TurnPortTest, TestTurnAlternateServerDetectRepetitionUDP) {
  TestTurnAlternateServerDetectRepetition(PROTO_UDP);
}

TEST_F(TurnPortTest, TestTurnAlternateServerDetectRepetitionTCP) {
  TestTurnAlternateServerDetectRepetition(PROTO_TCP);
}

// Test catching the case of a redirect to loopback.
TEST_F(TurnPortTest, TestTurnAlternateServerLoopbackUdpIpv4) {
  TestTurnAlternateServerLoopback(PROTO_UDP, false);
}

TEST_F(TurnPortTest, TestTurnAlternateServerLoopbackUdpIpv6) {
  TestTurnAlternateServerLoopback(PROTO_UDP, true);
}

TEST_F(TurnPortTest, TestTurnAlternateServerLoopbackTcpIpv4) {
  TestTurnAlternateServerLoopback(PROTO_TCP, false);
}

TEST_F(TurnPortTest, TestTurnAlternateServerLoopbackTcpIpv6) {
  TestTurnAlternateServerLoopback(PROTO_TCP, true);
}

// Do a TURN allocation and try to send a packet to it from the outside.
// The packet should be dropped. Then, try to send a packet from TURN to the
// outside. It should reach its destination. Finally, try again from the
// outside. It should now work as well.
TEST_F(TurnPortTest, TestTurnConnection) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnConnection(PROTO_UDP);
}

// Similar to above, except that this test will use the shared socket.
TEST_F(TurnPortTest, TestTurnConnectionUsingSharedSocket) {
  CreateSharedTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnConnection(PROTO_UDP);
}

// Test that we can establish a TCP connection with TURN server.
TEST_F(TurnPortTest, TestTurnTcpConnection) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  TestTurnConnection(PROTO_TCP);
}

// Test that if a connection on a TURN port is destroyed, the TURN port can
// still receive ping on that connection as if it is from an unknown address.
// If the connection is created again, it will be used to receive ping.
TEST_F(TurnPortTest, TestDestroyTurnConnection) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestDestroyTurnConnection();
}

// Similar to above, except that this test will use the shared socket.
TEST_F(TurnPortTest, TestDestroyTurnConnectionUsingSharedSocket) {
  CreateSharedTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestDestroyTurnConnection();
}

// Test that we fail to create a connection when we want to use TLS over TCP.
// This test should be removed once we have TLS support.
TEST_F(TurnPortTest, TestTurnTlsTcpConnectionFails) {
  ProtocolAddress secure_addr(kTurnTlsProtoAddr.address,
                              kTurnTlsProtoAddr.proto);
  CreateTurnPort(kTurnUsername, kTurnPassword, secure_addr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_error_, kSimulatedRtt * 2, fake_clock_);
  ASSERT_EQ(0U, turn_port_->Candidates().size());
}

// Run TurnConnectionTest with one-time-use nonce feature.
// Here server will send a 438 STALE_NONCE error message for
// every TURN transaction.
TEST_F(TurnPortTest, TestTurnConnectionUsingOTUNonce) {
  turn_server_.set_enable_otu_nonce(true);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnConnection(PROTO_UDP);
}

// Test that CreatePermissionRequest will be scheduled after the success
// of the first create permission request and the request will get an
// ErrorResponse if the ufrag and pwd are incorrect.
TEST_F(TurnPortTest, TestRefreshCreatePermissionRequest) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  PrepareTurnAndUdpPorts(PROTO_UDP);

  Connection* conn = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                                  Port::ORIGIN_MESSAGE);
  ASSERT_TRUE(conn != NULL);
  EXPECT_TRUE_SIMULATED_WAIT(turn_create_permission_success_, kSimulatedRtt,
                             fake_clock_);
  turn_create_permission_success_ = false;
  // A create-permission-request should be pending.
  // After the next create-permission-response is received, it will schedule
  // another request with bad_ufrag and bad_pwd.
  RelayCredentials bad_credentials("bad_user", "bad_pwd");
  turn_port_->set_credentials(bad_credentials);
  turn_port_->FlushRequests(kAllRequests);
  EXPECT_TRUE_SIMULATED_WAIT(turn_create_permission_success_, kSimulatedRtt,
                             fake_clock_);
  // Flush the requests again; the create-permission-request will fail.
  turn_port_->FlushRequests(kAllRequests);
  EXPECT_TRUE_SIMULATED_WAIT(!turn_create_permission_success_, kSimulatedRtt,
                             fake_clock_);
  EXPECT_TRUE(CheckConnectionFailedAndPruned(conn));
}

TEST_F(TurnPortTest, TestChannelBindGetErrorResponse) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  PrepareTurnAndUdpPorts(PROTO_UDP);
  Connection* conn1 = turn_port_->CreateConnection(udp_port_->Candidates()[0],
                                                   Port::ORIGIN_MESSAGE);
  ASSERT_TRUE(conn1 != nullptr);
  Connection* conn2 = udp_port_->CreateConnection(turn_port_->Candidates()[0],
                                                  Port::ORIGIN_MESSAGE);

  ASSERT_TRUE(conn2 != nullptr);
  conn1->Ping(0);
  EXPECT_TRUE_SIMULATED_WAIT(conn1->writable(), kSimulatedRtt * 2, fake_clock_);
  // TODO(deadbeef): SetEntryChannelId should not be a public method.
  // Instead we should set an option on the fake TURN server to force it to
  // send a channel bind errors.
  ASSERT_TRUE(
      turn_port_->SetEntryChannelId(udp_port_->Candidates()[0].address(), -1));

  std::string data = "ABC";
  conn1->Send(data.data(), data.length(), options);

  EXPECT_TRUE_SIMULATED_WAIT(CheckConnectionFailedAndPruned(conn1),
                             kSimulatedRtt, fake_clock_);
  // Verify that packets are allowed to be sent after a bind request error.
  // They'll just use a send indication instead.
  conn2->SignalReadPacket.connect(static_cast<TurnPortTest*>(this),
                                  &TurnPortTest::OnUdpReadPacket);
  conn1->Send(data.data(), data.length(), options);
  EXPECT_TRUE_SIMULATED_WAIT(!udp_packets_.empty(), kSimulatedRtt, fake_clock_);
}

// Do a TURN allocation, establish a UDP connection, and send some data.
TEST_F(TurnPortTest, TestTurnSendDataTurnUdpToUdp) {
  // Create ports and prepare addresses.
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnSendData(PROTO_UDP);
  EXPECT_EQ(UDP_PROTOCOL_NAME, turn_port_->Candidates()[0].relay_protocol());
}

// Do a TURN allocation, establish a TCP connection, and send some data.
TEST_F(TurnPortTest, TestTurnSendDataTurnTcpToUdp) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  // Create ports and prepare addresses.
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  TestTurnSendData(PROTO_TCP);
  EXPECT_EQ(TCP_PROTOCOL_NAME, turn_port_->Candidates()[0].relay_protocol());
}

// Test TURN fails to make a connection from IPv6 address to a server which has
// IPv4 address.
TEST_F(TurnPortTest, TestTurnLocalIPv6AddressServerIPv4) {
  turn_server_.AddInternalSocket(kTurnUdpIPv6IntAddr, PROTO_UDP);
  CreateTurnPort(kLocalIPv6Addr, kTurnUsername, kTurnPassword,
                 kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  ASSERT_TRUE_SIMULATED_WAIT(turn_error_, kSimulatedRtt, fake_clock_);
  EXPECT_TRUE(turn_port_->Candidates().empty());
}

// Test TURN make a connection from IPv6 address to a server which has
// IPv6 intenal address. But in this test external address is a IPv4 address,
// hence allocated address will be a IPv4 address.
TEST_F(TurnPortTest, TestTurnLocalIPv6AddressServerIPv6ExtenalIPv4) {
  turn_server_.AddInternalSocket(kTurnUdpIPv6IntAddr, PROTO_UDP);
  CreateTurnPort(kLocalIPv6Addr, kTurnUsername, kTurnPassword,
                 kTurnUdpIPv6ProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);
  ASSERT_EQ(1U, turn_port_->Candidates().size());
  EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
            turn_port_->Candidates()[0].address().ipaddr());
  EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
}

// Tests that the local and remote candidate address families should match when
// a connection is created. Specifically, if a TURN port has an IPv6 address,
// its local candidate will still be an IPv4 address and it can only create
// connections with IPv4 remote candidates.
TEST_F(TurnPortTest, TestCandidateAddressFamilyMatch) {
  turn_server_.AddInternalSocket(kTurnUdpIPv6IntAddr, PROTO_UDP);

  CreateTurnPort(kLocalIPv6Addr, kTurnUsername, kTurnPassword,
                 kTurnUdpIPv6ProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);
  ASSERT_EQ(1U, turn_port_->Candidates().size());

  // Create an IPv4 candidate. It will match the TURN candidate.
  Candidate remote_candidate(ICE_CANDIDATE_COMPONENT_RTP, "udp", kLocalAddr2, 0,
                             "", "", "local", 0, kCandidateFoundation);
  remote_candidate.set_address(kLocalAddr2);
  Connection* conn =
      turn_port_->CreateConnection(remote_candidate, Port::ORIGIN_MESSAGE);
  EXPECT_NE(nullptr, conn);

  // Set the candidate address family to IPv6. It won't match the TURN
  // candidate.
  remote_candidate.set_address(kLocalIPv6Addr2);
  conn = turn_port_->CreateConnection(remote_candidate, Port::ORIGIN_MESSAGE);
  EXPECT_EQ(nullptr, conn);
}

TEST_F(TurnPortTest, TestOriginHeader) {
  CreateTurnPortWithOrigin(kLocalAddr1, kTurnUsername, kTurnPassword,
                           kTurnUdpProtoAddr, kTestOrigin);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);
  ASSERT_GT(turn_server_.server()->allocations().size(), 0U);
  SocketAddress local_address = turn_port_->GetLocalAddress();
  ASSERT_TRUE(turn_server_.FindAllocation(local_address) != NULL);
  EXPECT_EQ(kTestOrigin, turn_server_.FindAllocation(local_address)->origin());
}

// Test that a CreatePermission failure will result in the connection being
// pruned and failed.
TEST_F(TurnPortTest, TestConnectionFailedAndPrunedOnCreatePermissionFailure) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  turn_server_.server()->set_reject_private_addresses(true);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 3, fake_clock_);

  CreateUdpPort(SocketAddress("10.0.0.10", 0));
  udp_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(udp_ready_, kSimulatedRtt, fake_clock_);
  // Create a connection.
  TestConnectionWrapper conn(turn_port_->CreateConnection(
      udp_port_->Candidates()[0], Port::ORIGIN_MESSAGE));
  EXPECT_TRUE(conn.connection() != nullptr);

  // Asynchronously, CreatePermission request should be sent and fail, which
  // will make the connection pruned and failed.
  EXPECT_TRUE_SIMULATED_WAIT(CheckConnectionFailedAndPruned(conn.connection()),
                             kSimulatedRtt, fake_clock_);
  EXPECT_TRUE_SIMULATED_WAIT(!turn_create_permission_success_, kSimulatedRtt,
                             fake_clock_);
  // Check that the connection is not deleted asynchronously.
  SIMULATED_WAIT(conn.connection() == nullptr, kConnectionDestructionDelay,
                 fake_clock_);
  EXPECT_NE(nullptr, conn.connection());
}

// Test that a TURN allocation is released when the port is closed.
TEST_F(TurnPortTest, TestTurnReleaseAllocation) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 2, fake_clock_);

  ASSERT_GT(turn_server_.server()->allocations().size(), 0U);
  turn_port_.reset();
  EXPECT_EQ_SIMULATED_WAIT(0U, turn_server_.server()->allocations().size(),
                           kSimulatedRtt, fake_clock_);
}

// Test that a TURN TCP allocation is released when the port is closed.
TEST_F(TurnPortTest, TestTurnTCPReleaseAllocation) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_SIMULATED_WAIT(turn_ready_, kSimulatedRtt * 3, fake_clock_);

  ASSERT_GT(turn_server_.server()->allocations().size(), 0U);
  turn_port_.reset();
  EXPECT_EQ_SIMULATED_WAIT(0U, turn_server_.server()->allocations().size(),
                           kSimulatedRtt, fake_clock_);
}

// This test verifies any FD's are not leaked after TurnPort is destroyed.
// https://code.google.com/p/webrtc/issues/detail?id=2651
#if defined(WEBRTC_LINUX) && !defined(WEBRTC_ANDROID)

TEST_F(TurnPortTest, TestResolverShutdown) {
  turn_server_.AddInternalSocket(kTurnUdpIPv6IntAddr, PROTO_UDP);
  int last_fd_count = GetFDCount();
  // Need to supply unresolved address to kick off resolver.
  CreateTurnPort(kLocalIPv6Addr, kTurnUsername, kTurnPassword,
                 ProtocolAddress(rtc::SocketAddress("www.google.invalid", 3478),
                                 PROTO_UDP));
  turn_port_->PrepareAddress();
  ASSERT_TRUE_WAIT(turn_error_, kResolverTimeout);
  EXPECT_TRUE(turn_port_->Candidates().empty());
  turn_port_.reset();
  rtc::Thread::Current()->Post(RTC_FROM_HERE, this, MSG_TESTFINISH);
  // Waiting for above message to be processed.
  ASSERT_TRUE_SIMULATED_WAIT(test_finish_, 1, fake_clock_);
  EXPECT_EQ(last_fd_count, GetFDCount());
}
#endif

}  // namespace cricket
