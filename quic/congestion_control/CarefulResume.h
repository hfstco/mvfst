/*
 * Created by Matthias Hofstaetter on 19.09.25.
 */

#pragma once

#include <quic/QuicException.h>
#include <quic/congestion_control/CongestionControlFunctions.h>
#include <quic/congestion_control/CongestionController.h>
#include <quic/state/AckEvent.h>
#include <quic/state/StateData.h>
#include <chrono>

namespace quic {

constexpr float kCarefulResumeBeta = 0.5;

struct CarefulResume {
  enum class States : uint8_t {
    Normal = 0,
    Reconnaisance = 1,
    Unvalidated = 2,
    Validating = 3,
    SafeRetreat = 4,
    Observe = 100
  };

  std::string stateToString(States state) {
    switch (state) {
      case States::Normal:
        return "normal";
      case States::Reconnaisance:
        return "reconnaisance";
      case States::Unvalidated:
        return "unvalidated";
      case States::Validating:
        return "validating";
      case States::SafeRetreat:
        return "safe_retreat";
      case States::Observe:
        return "observe";
    }
    folly::assume_unreachable();
  }

  enum class Trigger : uint8_t {
    CwndLimited,
    RTTNotValidated,
    LastUnvalidatedPacketSent,
    FirstUnvalidatedPacketAcknowledged,
    RTTExceeded,
    RateLimited,
    LastUnvalidatedPacketAcknowledged,
    PacketLoss,
    ECN_CE,
    ExitRecovery,
    PathChanged
  };

  std::string triggerToString(Trigger trigger) {
    switch (trigger) {
      case Trigger::CwndLimited:
        return "congestion_window_limited";
      case Trigger::RTTNotValidated:
        return "rtt_not_validated";
      case Trigger::LastUnvalidatedPacketSent:
        return "first_unvalidated_packet_acknowledged";
      case Trigger::FirstUnvalidatedPacketAcknowledged:
        return "last_unvalidated_packet_acknowledged";
      case Trigger::RTTExceeded:
        return "rtt_exceeded";
      case Trigger::RateLimited:
        return "rate_limited";
      case Trigger::LastUnvalidatedPacketAcknowledged:
        return "last_unvalidated_packet_acknowledged";
      case Trigger::PacketLoss:
        return "packet_loss";
      case Trigger::ECN_CE:
        return "ECN_CE";
      case Trigger::ExitRecovery:
        return "exit_recovery";
      case Trigger::PathChanged:
        return "path_changed";
    }
    folly::assume_unreachable();
  }

  [[nodiscard]] States state() const noexcept;

  void onPacketSent(uint64_t& cwndBytes, uint64_t& ssthresh);
  void onPacketAcked(const AckEvent& ack, uint64_t& cwndBytes, uint64_t& ssthresh);
  void onPacketLoss(const CongestionController::LossEvent& loss, uint64_t& cwndBytes, uint64_t& ssthresh);

  explicit CarefulResume(
      QuicConnectionStateBase& conn
      ) : conn_(conn) {
    /* TODO Load previous CC parameters for connection.
     * Verify endpoint matches. */

    savedCongestionWindow_ = 3750000;

    /* Set Careful Resume parameters forcefully. */
    if (getenv("PREVIOUS_RTT") && getenv("PREVIOUS_CWND_BYTES")) {
      savedCongestionWindow_ = strtoull(getenv("PREVIOUS_CWND_BYTES"), NULL, 10);
      savedRTT_ = strtoull(getenv("PREVIOUS_RTT"), NULL, 10);
    }
  };

protected:
  States state_{States::Reconnaisance};

private:
  QuicConnectionStateBase& conn_;

  Optional<TimePoint> startOfEpoch_;
  States lastState_{States::Reconnaisance};

  uint64_t pipeSize_{0};
  uint64_t firstUnvalidatedPacket_{0};
  uint64_t lastUnvalidatedPacket_{0};

  uint64_t savedRTT_{0};
  uint64_t savedCongestionWindow_{0};

  Trigger trigger_{Trigger::RTTNotValidated};

  void EnterReconnaissance(uint64_t& cwndBytes, uint64_t& ssthresh);
  void EnterUnvalidated(uint64_t& cwndBytes, uint64_t& ssthresh);
  void EnterValidating(uint64_t& cwndBytes, uint64_t& ssthresh);
  void EnterSafeRetreat(uint64_t& cwndBytes, uint64_t& ssthresh);
  void EnterNormal(uint64_t& cwndBytes, uint64_t& ssthresh);
  void EnterObserve(uint64_t& cwndBytes, uint64_t& ssthresh);
};
}
