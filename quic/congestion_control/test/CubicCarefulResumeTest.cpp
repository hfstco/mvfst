/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <quic/common/test/TestUtils.h>
#include <quic/congestion_control/QuicCubic.h>
#include <quic/congestion_control/test/Utils.h>
#include <quic/state/test/Mocks.h>

using namespace testing;

namespace quic::test {

class CubicCarefulResumeTest : public Test {
 protected:
  static constexpr uint64_t kPktLen = 1000;
  static constexpr uint64_t kIw = 10 * kPktLen;
  static constexpr uint64_t kSavedCwnd = 200 * kPktLen;

  void SetUp() override {
    conn_.udpSendPacketLen = kPktLen;
    conn_.transportSettings.initCwndInMss = 10;
    conn_.transportSettings.useCwndHintsInSessionTicket = true;
    setRtt(100ms);
    cubic_ = std::make_unique<Cubic>(conn_);
  }

  void setRtt(std::chrono::microseconds rtt) {
    conn_.lossState.maybeLrtt = rtt;
    conn_.lossState.lrtt = rtt;
    conn_.lossState.mrtt = rtt;
    conn_.lossState.srtt = rtt;
  }

  // Sends `count` packets, each sent 1us after the previous one.
  std::vector<OutstandingPacketWrapper> send(size_t count, TimePoint start) {
    std::vector<OutstandingPacketWrapper> packets;
    for (size_t i = 0; i < count; i++) {
      auto packet = makeTestingWritePacket(
          nextPacketNum_++,
          kPktLen,
          totalSent_ += kPktLen,
          start + std::chrono::microseconds(i));
      onPacketsSentWrapper(&conn_, cubic_.get(), packet);
      packets.push_back(std::move(packet));
    }
    return packets;
  }

  void ack(
      const std::vector<OutstandingPacketWrapper>& packets,
      size_t from,
      size_t to,
      TimePoint ackTime) {
    const auto& last = packets[to - 1];
    onPacketAckOrLossWrapper(
        &conn_,
        cubic_.get(),
        makeAck(
            last.packet.header.getPacketSequenceNum(),
            (to - from) * kPktLen,
            ackTime,
            last.metadata.time),
        std::nullopt);
  }

  void lose(const OutstandingPacketWrapper& packet) {
    LossEvent loss;
    loss.addLostPacket(packet);
    onPacketAckOrLossWrapper(&conn_, cubic_.get(), std::nullopt, loss);
  }

  // Sends and acks the IW while cwnd limited. Returns time of that ack.
  TimePoint completeReconnaissance() {
    auto packets = send(10, t0_);
    auto ackTime = t0_ + 100ms;
    ack(packets, 0, 10, ackTime);
    return ackTime;
  }

  QuicConnectionStateBase conn_{QuicNodeType::Server};
  std::unique_ptr<Cubic> cubic_;
  TimePoint t0_{Clock::now()};
  PacketNum nextPacketNum_{0};
  uint64_t totalSent_{0};
};

TEST_F(CubicCarefulResumeTest, DisabledBySetting) {
  conn_.transportSettings.useCwndHintsInSessionTicket = false;
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
}

TEST_F(CubicCarefulResumeTest, RequiresRttHint) {
  cubic_->setResumeHints(kSavedCwnd, std::nullopt);
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
}

TEST_F(CubicCarefulResumeTest, OnlyFirstHintIsUsed) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  cubic_->setResumeHints(4 * kSavedCwnd, 100ms);
  completeReconnaissance();
  EXPECT_EQ(CarefulResumePhase::Unvalidated, cubic_->carefulResumePhase());
  EXPECT_EQ(kSavedCwnd / 2, cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, JumpsToHalfSavedCwnd) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  EXPECT_EQ(CarefulResumePhase::Reconnaissance, cubic_->carefulResumePhase());
  completeReconnaissance();
  EXPECT_EQ(CarefulResumePhase::Unvalidated, cubic_->carefulResumePhase());
  EXPECT_EQ(kSavedCwnd / 2, cubic_->getCongestionWindow());
  EXPECT_EQ(CubicStates::Hystart, cubic_->state());
}

TEST_F(CubicCarefulResumeTest, RttTooSmallFallsBackToNormal) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  setRtt(50ms);
  completeReconnaissance();
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
  // Normal slow start: IW + acked bytes.
  EXPECT_EQ(2 * kIw, cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, SmallSavedCwndFallsBackToNormal) {
  cubic_->setResumeHints(2 * kIw, 100ms);
  completeReconnaissance();
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
  EXPECT_EQ(2 * kIw, cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, RateLimitedStaysInReconnaissance) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  auto packets = send(5, t0_);
  ack(packets, 0, 5, t0_ + 100ms);
  packets = send(5, t0_ + 101ms);
  ack(packets, 0, 5, t0_ + 200ms);
  EXPECT_EQ(CarefulResumePhase::Reconnaissance, cubic_->carefulResumePhase());
  EXPECT_EQ(2 * kIw, cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, LossInReconnaissanceFallsBackToNormal) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  auto packets = send(10, t0_);
  lose(packets[0]);
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
  EXPECT_EQ(CubicStates::FastRecovery, cubic_->state());
}

TEST_F(CubicCarefulResumeTest, ValidatesAndReturnsToNormal) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  auto t1 = completeReconnaissance() + 1ms;
  auto jump = kSavedCwnd / 2;

  // Filling the jump cwnd moves to Validating with cwnd = flight_size.
  auto packets = send(jump / kPktLen - 1, t1);
  EXPECT_EQ(CarefulResumePhase::Unvalidated, cubic_->carefulResumePhase());
  auto last = send(1, t1 + 1ms);
  packets.push_back(std::move(last[0]));
  EXPECT_EQ(CarefulResumePhase::Validating, cubic_->carefulResumePhase());
  EXPECT_EQ(jump, cubic_->getCongestionWindow());

  // Acking all but the last unvalidated packet: cwnd grows by slow start.
  ack(packets, 0, packets.size() - 1, t1 + 100ms);
  EXPECT_EQ(CarefulResumePhase::Validating, cubic_->carefulResumePhase());
  EXPECT_EQ(2 * jump - kPktLen, cubic_->getCongestionWindow());

  // Acking the last unvalidated packet ends Careful Resume.
  ack(packets, packets.size() - 1, packets.size(), t1 + 101ms);
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
  EXPECT_EQ(CubicStates::Hystart, cubic_->state());
  EXPECT_EQ(2 * jump, cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, AckOfUnvalidatedPacketWhileRateLimited) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  auto t1 = completeReconnaissance() + 1ms;
  auto packets = send(5, t1);
  EXPECT_EQ(CarefulResumePhase::Unvalidated, cubic_->carefulResumePhase());
  // Ack of the first unvalidated packet leaves flight_size < IW, so cwnd is
  // reset to max(PipeSize, IW) and CR stops.
  ack(packets, 0, 1, t1 + 100ms);
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
  EXPECT_EQ(kIw, cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, RttElapsedInUnvalidated) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  auto t1 = completeReconnaissance() + 1ms;
  send(30, t1);
  EXPECT_EQ(CarefulResumePhase::Unvalidated, cubic_->carefulResumePhase());
  // A send more than one RTT after entering Unvalidated moves to Validating.
  send(1, t1 + 101ms);
  EXPECT_EQ(CarefulResumePhase::Validating, cubic_->carefulResumePhase());
  EXPECT_EQ(31 * kPktLen, cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, LossInUnvalidatedEntersSafeRetreat) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  auto t1 = completeReconnaissance() + 1ms;
  auto packets = send(50, t1);
  lose(packets[0]);
  EXPECT_EQ(CarefulResumePhase::SafeRetreat, cubic_->carefulResumePhase());
  EXPECT_EQ(CubicStates::FastRecovery, cubic_->state());
  // PipeSize is 0 (nothing in flight on entry), so cwnd drops to minimum.
  EXPECT_EQ(
      conn_.transportSettings.minCwndInMss * kPktLen,
      cubic_->getCongestionWindow());
}

TEST_F(CubicCarefulResumeTest, LossInValidatingSafeRetreatAndExit) {
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  auto t1 = completeReconnaissance() + 1ms;
  auto packets = send(50, t1);

  // First unvalidated packets acked -> Validating (PipeSize = 20 packets).
  ack(packets, 0, 20, t1 + 100ms);
  EXPECT_EQ(CarefulResumePhase::Validating, cubic_->carefulResumePhase());
  EXPECT_EQ(30 * kPktLen, cubic_->getCongestionWindow());

  // Loss -> Safe Retreat with cwnd = PipeSize / 2.
  lose(packets[20]);
  EXPECT_EQ(CarefulResumePhase::SafeRetreat, cubic_->carefulResumePhase());
  EXPECT_EQ(CubicStates::FastRecovery, cubic_->state());
  EXPECT_EQ(10 * kPktLen, cubic_->getCongestionWindow());

  // Further losses and acks do not change cwnd in Safe Retreat.
  lose(packets[21]);
  ack(packets, 22, 40, t1 + 110ms);
  EXPECT_EQ(CarefulResumePhase::SafeRetreat, cubic_->carefulResumePhase());
  EXPECT_EQ(10 * kPktLen, cubic_->getCongestionWindow());

  // Ack of the last unvalidated packet exits: ssthresh = PipeSize * beta.
  ack(packets, 40, 50, t1 + 120ms);
  EXPECT_EQ(CarefulResumePhase::Normal, cubic_->carefulResumePhase());
  EXPECT_EQ(CubicStates::Hystart, cubic_->state());
  EXPECT_EQ(10 * kPktLen, cubic_->getCongestionWindow());
  CongestionControllerStats stats;
  cubic_->getStats(stats);
  EXPECT_EQ(
      static_cast<uint64_t>(48 * kPktLen * kDefaultCubicReductionFactor),
      stats.cubicStats.ssthresh);
}

TEST_F(CubicCarefulResumeTest, PacesJumpOverOneRtt) {
  auto mockPacer = std::make_unique<NiceMock<MockPacer>>();
  auto* rawPacer = mockPacer.get();
  conn_.pacer = std::move(mockPacer);
  cubic_->setResumeHints(kSavedCwnd, 100ms);
  // Unvalidated packets are paced at jump_cwnd per current RTT (gain 1), not
  // with the 2x Hystart gain.
  EXPECT_CALL(*rawPacer, refreshPacingRate(
          kSavedCwnd / 2, std::chrono::microseconds(100ms), _));
  completeReconnaissance();
  EXPECT_EQ(CarefulResumePhase::Unvalidated, cubic_->carefulResumePhase());
}

} // namespace quic::test
