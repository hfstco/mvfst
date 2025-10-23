/*
 * Created by Matthias Hofstaetter on 19.09.25.
 */

#include <quic/congestion_control/CarefulResume.h>


namespace quic {

  void CarefulResume::onPacketSent(uint64_t& cwndBytes, uint64_t& ssthresh) {
    switch (state_) {
      case States::Unvalidated:
        if (cwndBytes < conn_.lossState.inflightBytes) {
          trigger_ = Trigger::LastUnvalidatedPacketSent;
          return EnterValidating(cwndBytes, ssthresh);
        }
        break;
      default:
        break;
    }
  }

  void CarefulResume::onPacketAcked(const AckEvent& ack, uint64_t& cwndBytes, uint64_t& ssthresh) {
    switch (state_) {
      case States::Reconnaisance:
        if (conn_.ackStates.appDataAckState.largestAckedByPeer.has_value() &&
        conn_.ackStates.appDataAckState.largestAckedByPeer.value() > 10 &&
          conn_.lossState.mrtt >= (savedRTT_ / 2) &&
          conn_.lossState.mrtt <= (savedRTT_ * 10)) {
          trigger_ = Trigger::CwndLimited;
          return EnterUnvalidated(cwndBytes, ssthresh);
          }
        break;
      case States::Unvalidated:
        pipeSize_ += ack.ackedBytes;
        if (ack.ackTime - startOfEpoch_.value() > conn_.lossState.mrtt) {
          trigger_ = Trigger::RTTExceeded;
          return EnterValidating(cwndBytes, ssthresh);
        }
        break;
      case States::Validating:
        pipeSize_ += ack.ackedBytes;
        if (conn_.ackStates.appDataAckState.largestAckedByPeer > lastUnvalidatedPacket_) {
          trigger_ = Trigger::LastUnvalidatedPacketAcknowledged;
          EnterNormal(cwndBytes, ssthresh);
        }
        break;
      case States::SafeRetreat:
        pipeSize_ += ack.ackedBytes;
        if (conn_.ackStates.appDataAckState.largestAckedByPeer > lastUnvalidatedPacket_) {
          ssthresh = pipeSize_ * kCarefulResumeBeta;
          trigger_ = Trigger::ExitRecovery;
          EnterNormal(cwndBytes, ssthresh);
        }
        break;
      default:
        break;;
    }
  }

  void CarefulResume::onPacketLoss(const CongestionController::LossEvent& loss, uint64_t& cwndBytes, uint64_t& ssthresh) {
    switch (state_) {
      case States::Reconnaisance:
        EnterNormal(cwndBytes, ssthresh);
        break;
      case States::Unvalidated:
      case States::Validating:
        return EnterSafeRetreat(cwndBytes, ssthresh);
        break;
      default:
        break;
    }
  }

  CarefulResume::States CarefulResume::state() const noexcept {
    return state_;
  }

  void CarefulResume::EnterReconnaissance(uint64_t& cwndBytes, uint64_t& ssthresh) {
    lastState_ = state_;
    state_ = States::Reconnaisance;

    startOfEpoch_ = Clock::now();

    LOG(INFO) << "Entering Reconnaissance savedCongestionWindow_=" << savedCongestionWindow_ <<
      "; savedRTT_=" << savedRTT_.count();
    if (conn_.qLogger) {
      conn_.qLogger->addCarefulResumePhaseUpdated(stateToString(lastState_),
        stateToString(state_), pipeSize_, firstUnvalidatedPacket_,
        lastUnvalidatedPacket_, cwndBytes, ssthresh, savedCongestionWindow_,
        savedRTT_, triggerToString(trigger_));
    }
  }

  void CarefulResume::EnterUnvalidated(uint64_t& cwndBytes, uint64_t& ssthresh) {
    lastState_ = state_;
    state_ = States::Unvalidated;

    startOfEpoch_ = Clock::now();

    firstUnvalidatedPacket_ = conn_.lossState.largestSent.value() + 1;
    pipeSize_ = conn_.lossState.inflightBytes;

    cwndBytes = savedCongestionWindow_ / 2;

    LOG(INFO) << "Entering Unvalidated cwnd=" << cwndBytes <<
      "; pipeSize_=" << pipeSize_ <<
      "; firstUnvalidatedPacket_=" << firstUnvalidatedPacket_ <<
        "; lastUnvalidatedPacket_=" << lastUnvalidatedPacket_;

    if (conn_.qLogger) {
      conn_.qLogger->addCarefulResumePhaseUpdated(stateToString(lastState_),
        stateToString(state_), pipeSize_, firstUnvalidatedPacket_,
        lastUnvalidatedPacket_, cwndBytes, ssthresh, savedCongestionWindow_,
        savedRTT_, triggerToString(trigger_));
    }
  }

  void CarefulResume::EnterValidating(uint64_t& cwndBytes, uint64_t& ssthresh) {
    lastState_ = state_;
    state_ = States::Validating;

    startOfEpoch_ = Clock::now();

    lastUnvalidatedPacket_ = conn_.lossState.largestSent.value();

    if (conn_.lossState.inflightBytes > pipeSize_) {
      cwndBytes = conn_.lossState.inflightBytes;
    } else {
      trigger_ = Trigger::RateLimited;
      EnterNormal(cwndBytes, ssthresh);
      cwndBytes = pipeSize_;
    }

    LOG(INFO) << "Entering Validating cwnd=" << cwndBytes <<
      "; pipeSize_=" << pipeSize_ <<
      "; firstUnvalidatedPacket_=" << firstUnvalidatedPacket_ <<
        "; lastUnvalidatedPacket_=" << lastUnvalidatedPacket_;

    if (conn_.qLogger) {
      conn_.qLogger->addCarefulResumePhaseUpdated(stateToString(lastState_),
        stateToString(state_), pipeSize_, firstUnvalidatedPacket_,
        lastUnvalidatedPacket_, cwndBytes, ssthresh, savedCongestionWindow_,
        savedRTT_, triggerToString(trigger_));
    }
  }

  void CarefulResume::EnterSafeRetreat(uint64_t& cwndBytes, uint64_t& ssthresh) {
    lastState_ = state_;
    state_ = States::SafeRetreat;

    startOfEpoch_ = Clock::now();

    auto minCwnd = conn_.transportSettings.minCwndInMss * conn_.udpSendPacketLen;

    cwndBytes = (pipeSize_ / 2 >= minCwnd) ? pipeSize_ / 2 : minCwnd;

    LOG(INFO) << "Entering Safe Retreat cwnd=" << cwndBytes <<
      "; pipeSize_=" << pipeSize_ <<
      "; firstUnvalidatedPacket_=" << firstUnvalidatedPacket_ <<
        "; lastUnvalidatedPacket_=" << lastUnvalidatedPacket_;
    if (conn_.qLogger) {
      conn_.qLogger->addCarefulResumePhaseUpdated(stateToString(lastState_),
        stateToString(state_), pipeSize_, firstUnvalidatedPacket_,
        lastUnvalidatedPacket_, cwndBytes, ssthresh, savedCongestionWindow_,
        savedRTT_, triggerToString(trigger_));
    }
  }

 void CarefulResume::EnterNormal(uint64_t& cwndBytes, uint64_t& ssthresh) {
    lastState_ = state_;
    state_ = States::Normal;

    startOfEpoch_ = Clock::now();

    LOG(INFO) << "Entering Normal cwnd=" << cwndBytes <<
      "; pipeSize_=" << pipeSize_ <<
      "; firstUnvalidatedPacket_=" << firstUnvalidatedPacket_ <<
        "; lastUnvalidatedPacket_=" << lastUnvalidatedPacket_;

    if (conn_.qLogger) {
      conn_.qLogger->addCarefulResumePhaseUpdated(stateToString(lastState_),
        stateToString(state_), pipeSize_, firstUnvalidatedPacket_,
        lastUnvalidatedPacket_, cwndBytes, ssthresh, savedCongestionWindow_,
        savedRTT_, triggerToString(trigger_));
    }
  }

  void CarefulResume::EnterObserve(uint64_t& cwndBytes, uint64_t& ssthresh) {
    lastState_ = state_;
    state_ = States::Observe;

    startOfEpoch_ = Clock::now();

    LOG(INFO) << "Entering Observe";

    if (conn_.qLogger) {
      conn_.qLogger->addCarefulResumePhaseUpdated(stateToString(lastState_),
        stateToString(state_), pipeSize_, firstUnvalidatedPacket_,
        lastUnvalidatedPacket_, cwndBytes, ssthresh, savedCongestionWindow_,
        savedRTT_, triggerToString(trigger_));
    }
  }
  } // namespace quic