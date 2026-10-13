/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GTest.h>
#include <quic/common/test/TestUtils.h>
#include <quic/congestion_control/QuicCubic.h>
#include <quic/congestion_control/test/Utils.h>

using namespace quic;
using namespace quic::test;
using namespace testing;

class CubicSteadyTest : public Test {};

TEST_F(CubicSteadyTest, CubicReduction) {
  QuicConnectionStateBase conn(QuicNodeType::Client);
  // initCwnd > initSsthresh: an ack will immediately make the state machine
  // transit to Steady state:
  conn.udpSendPacketLen = 200; // initCwnd = 2000, minCwnd = 400
  Cubic cubic(conn, 0, 1000);

  // Send one and get acked, this moves the state machine to steady. Cwnd will
  // be 3000, inflight will be 0
  auto packet0 = makeTestingWritePacket(0, 1000, 1000);
  conn.lossState.largestSent = 0;
  cubic.onPacketSent(packet0);
  quic::test::onPacketAckOrLossWrapper(
      &conn,
      &cubic,
      makeAck(0, 1000, Clock::now(), packet0.metadata.time),
      std::nullopt);
  EXPECT_EQ(3000, cubic.getWritableBytes());
  EXPECT_EQ(CubicStates::Steady, cubic.state());

  // Send one and get lost, this moves the state machine to FastRecovery. Cwnd
  // will be reduced, inflight will be 0
  auto packet1 = makeTestingWritePacket(1, 1000, 2000);
  conn.lossState.largestSent = 1;
  cubic.onPacketSent(packet1);
  LossEvent loss;
  loss.addLostPacket(packet1);
  quic::test::onPacketAckOrLossWrapper(
      &conn, &cubic, std::nullopt, std::move(loss));
  EXPECT_EQ(2100, cubic.getWritableBytes());
  EXPECT_EQ(CubicStates::FastRecovery, cubic.state());
}

// RFC 9002 Section 7.8: cwnd SHOULD NOT grow in congestion avoidance while
// the sender is not utilizing the congestion window (app-limited).
TEST_F(CubicSteadyTest, NoGrowthWhenNotCwndLimited) {
  QuicConnectionStateBase conn(QuicNodeType::Client);
  conn.udpSendPacketLen = 200; // initCwnd = 2000
  Cubic cubic(conn, 0, 1000);
  auto now = Clock::now();

  // Fill the cwnd and ack: moves to Steady, cwnd = 4000, inflight = 0.
  auto packet0 = makeTestingWritePacket(0, 2000, 2000, now);
  conn.lossState.largestSent = 0;
  quic::test::onPacketsSentWrapper(&conn, &cubic, packet0);
  quic::test::onPacketAckOrLossWrapper(
      &conn,
      &cubic,
      makeAck(0, 2000, now + 10ms, packet0.metadata.time),
      std::nullopt);
  ASSERT_EQ(CubicStates::Steady, cubic.state());
  auto cwnd = cubic.getWritableBytes();
  ASSERT_EQ(4000, cwnd);

  // Send a quarter of the cwnd: not cwnd limited -> no growth even after 1s.
  auto packet1 = makeTestingWritePacket(1, 1000, 3000, now + 1s);
  conn.lossState.largestSent = 1;
  quic::test::onPacketsSentWrapper(&conn, &cubic, packet1);
  quic::test::onPacketAckOrLossWrapper(
      &conn,
      &cubic,
      makeAck(1, 1000, now + 1s + 10ms, packet1.metadata.time),
      std::nullopt);
  EXPECT_EQ(cwnd, cubic.getWritableBytes());
  EXPECT_EQ(CubicStates::Steady, cubic.state());

  // Fill the cwnd: cwnd limited -> grows.
  auto packet2 = makeTestingWritePacket(2, cwnd, 3000 + cwnd, now + 2s);
  conn.lossState.largestSent = 2;
  quic::test::onPacketsSentWrapper(&conn, &cubic, packet2);
  quic::test::onPacketAckOrLossWrapper(
      &conn,
      &cubic,
      makeAck(2, cwnd, now + 2s + 10ms, packet2.metadata.time),
      std::nullopt);
  EXPECT_GT(cubic.getWritableBytes(), cwnd);
  EXPECT_EQ(CubicStates::Steady, cubic.state());
}
