/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <quic/QuicException.h>
#include <quic/congestion_control/CongestionControlFunctions.h>
#include <quic/congestion_control/CongestionController.h>
#include <quic/state/AckEvent.h>
#include <quic/state/StateData.h>

namespace quic {

constexpr float kCubicHystartPacingGain = 2.0f;
constexpr float kCubicRecoveryPacingGain = 1.25f;

enum class CubicStates : uint8_t {
  Hystart,
  Steady,
  FastRecovery,
};

// Careful Resume phases (draft-ietf-tsvwg-careful-resume).
enum class CarefulResumePhase : uint8_t {
  Normal,
  Reconnaissance,
  Unvalidated,
  Validating,
  SafeRetreat,
};

/**
 *
 *  |--------|                              |-----|
 *  |      [Ack]                          [Ack]   |
 *  |        |                              |     |
 *  -->Hystart------------[Ack]---------->Cubic<--|
 *        |                                 |     |
 *        |                                 |     |
 *        |                ->[ACK/Loss]     |     |
 *        |                |     |          |     |
 *        |                |     |          |     |
 *        -[Loss]---->Fast Recovery<--[Loss]-     |
 *                             |                  |
 *                             |                  |
 *                             |                  |
 *                             |->-----[Ack]------|
 *
 */

class Cubic : public CongestionController {
 public:
  static constexpr uint64_t INIT_SSTHRESH =
      std::numeric_limits<uint64_t>::max();
  /**
   * initSsthresh:      the initial value of ssthresh
   * ssreduction:       how should cwnd be reduced when loss happens during slow
   *                    start
   * tcpFriendly:       if cubic cwnd calculation should be friendly to Reno TCP
   * spreadacrossRtt:   if the pacing bursts should be spread across RTT or all
   *                    close to the beginning of an RTT round
   */
  explicit Cubic(
      QuicConnectionStateBase& conn,
      uint64_t initCwndBytes = 0,
      uint64_t initSsthresh = INIT_SSTHRESH,
      bool tcpFriendly = true,
      bool ackTrain = false);

  [[nodiscard]] CubicStates state() const noexcept;

  enum class ExitReason : uint8_t {
    SSTHRESH,
    EXITPOINT,
  };

  // if hybrid slow start exit point is found
  enum class HystartFound : uint8_t {
    No,
    FoundByAckTrainMethod,
    FoundByDelayIncreaseMethod
  };

  void onPacketAckOrLoss(
      const AckEvent* FOLLY_NULLABLE,
      const LossEvent* FOLLY_NULLABLE) override;

  void onPacketAckOrLoss(Optional<AckEvent> ack, Optional<LossEvent> loss) {
    onPacketAckOrLoss(
        ack.has_value() ? &ack.value() : nullptr,
        loss.has_value() ? &loss.value() : nullptr);
  }

  void onRemoveBytesFromInflight(uint64_t) override;
  void onPacketSent(const OutstandingPacketWrapper& packet) override;

  [[nodiscard]] uint64_t getWritableBytes() const noexcept override;
  [[nodiscard]] uint64_t getCongestionWindow() const noexcept override;
  void setAppIdle(bool idle, TimePoint eventTime) noexcept override;
  void setAppLimited() override;

  [[nodiscard]] bool isAppLimited() const noexcept override;

  void getStats(CongestionControllerStats& stats) const override;

  void handoff(
      uint64_t newCwnd,
      uint64_t ssthresh = INIT_SSTHRESH,
      TimePoint lastReductionTime = Clock::now()) noexcept;

  [[nodiscard]] CongestionControlType type() const noexcept override;

  void setExperimental(bool experimental) override {
    // This is a transitional change. Experimental setting will be removed.
    conn_.transportSettings.ccaConfig.additiveIncreaseAfterHystart =
        experimental;
  }

  void setResumeHints(
      uint64_t cwndHintBytes,
      const Optional<std::chrono::milliseconds>& rttHint =
          std::nullopt) override;

  [[nodiscard]] CarefulResumePhase carefulResumePhase() const noexcept {
    return crPhase_;
  }

 protected:
  CubicStates state_{CubicStates::Hystart};

 private:
  [[nodiscard]] bool isAppIdle() const noexcept;
  void onPacketAcked(const AckEvent& ack);
  void onPacketAckedInHystart(const AckEvent& ack);
  void onPacketAckedInSteady(const AckEvent& ack);
  void onPacketAckedInRecovery(const AckEvent& ack);

  void onPacketLoss(const LossEvent& loss);
  void onPacketLossInRecovery(const LossEvent& loss);
  void onPersistentCongestion();

  void onEcnCongestionEvent(const AckEvent& ack);

  [[nodiscard]] float pacingGain() const noexcept;

  void startHystartRttRound(TimePoint time) noexcept;

  void cubicReduction(TimePoint lossTime) noexcept;
  void updateTimeToOrigin() noexcept;
  int64_t calculateCubicCwndDelta(TimePoint timePoint) noexcept;
  uint64_t calculateCubicCwnd(int64_t delta) noexcept;

  bool isRecovered(TimePoint packetSentTime) noexcept;

  // Careful Resume. Returns true if the ack was fully handled by CR and the
  // normal Cubic state machine must not grow cwnd for it.
  bool crOnPacketAcked(const AckEvent& ack);
  void crMaybeEnterUnvalidated(TimePoint now);
  void crExitUnvalidated();
  void crEnterSafeRetreat(TimePoint lossTime);
  void crExitSafeRetreat();
  void crSetPhase(CarefulResumePhase phase);
  [[nodiscard]] uint64_t crInitCwndBytes() const noexcept;

  QuicConnectionStateBase& conn_;
  uint64_t cwndBytes_;
  // the value of cwndBytes_ at last loss event
  Optional<uint64_t> lossCwndBytes_;
  // the value of ssthresh_ at the last loss event
  Optional<uint64_t> lossSsthresh_;
  uint64_t ssthresh_;

  struct HystartState {
    // If AckTrain method will be used to exit SlowStart
    bool ackTrain{false};
    // If we are currently in a RTT round
    bool inRttRound{false};
    // If we have found the exit point
    HystartFound found{HystartFound::No};
    // The starting timestamp of a RTT round
    TimePoint roundStart;
    // Last timestamp when closed space Ack happens
    TimePoint lastJiffy;
    // The minimal of sampled RTT in current RTT round. Hystart only samples
    // first a few RTTs in a round
    OptionalMicros currSampledRtt;
    // End value of currSampledRtt at the end of a RTT round:
    OptionalMicros lastSampledRtt;
    // Estimated minimal delay of a path
    OptionalMicros delayMin;
    // Ack sampling count
    uint8_t ackCount{0};
    // When a packet with sent time >= rttRoundEndTarget is acked, end the
    // current RTT round
    TimePoint rttRoundEndTarget;
  };

  struct SteadyState {
    // time takes for cwnd to increase to lastMaxCwndBytes
    double timeToOrigin{0.0};
    // The cwnd value that timeToOrigin is calculated based on
    Optional<uint64_t> originPoint;
    bool tcpFriendly{true};
    Optional<TimePoint> lastReductionTime;
    // This is Wmax, it could be different from lossCwndBytes if cwnd never
    // reaches last lastMaxCwndBytes before loss event:
    Optional<uint64_t> lastMaxCwndBytes;
    uint64_t estRenoCwnd;
    // cache reduction/increase factors based on numEmulatedConnections_
    float reductionFactor{kDefaultCubicReductionFactor};
    float lastMaxReductionFactor{kDefaultLastMaxReductionFactor};
    float tcpEstimationIncreaseFactor{kCubicTCPFriendlyEstimateIncreaseFactor};
  };

  struct RecoveryState {
    // The time point after which Quic will no longer be in current recovery
    Optional<TimePoint> endOfRecovery;
  };

  // if quiescenceStart_ has a value, then the connection is app limited
  Optional<TimePoint> quiescenceStart_;

  HystartState hystartState_;
  SteadyState steadyState_;
  RecoveryState recoveryState_;
  bool isCwndBlocked_{true};

  TimePoint l4sCwndReducedTimestamp_;
  uint64_t lastCECount_{0};

  // Careful Resume state
  CarefulResumePhase crPhase_{CarefulResumePhase::Normal};
  bool crHintsSet_{false};
  uint64_t crSavedCwndBytes_{0};
  std::chrono::microseconds crSavedRtt_{0};
  // Bytes acked while in Reconnaissance (to confirm the IW was delivered)
  uint64_t crReconAckedBytes_{0};
  // Validated capacity measured from acked data
  uint64_t crPipeSize_{0};
  // Time the Unvalidated phase was entered. Packets sent at or after this are
  // unvalidated.
  Optional<TimePoint> crUnvalidatedStart_;
  // Sent time of the last packet sent in the Unvalidated phase.
  Optional<TimePoint> crLastUnvalidatedSentTime_;
  // Sent time of the last packet sent in the Unvalidated or Validating phase.
  Optional<TimePoint> crLastSentTime_;
};

folly::StringPiece cubicStateToString(CubicStates state);
folly::StringPiece carefulResumePhaseToString(CarefulResumePhase phase);

} // namespace quic
