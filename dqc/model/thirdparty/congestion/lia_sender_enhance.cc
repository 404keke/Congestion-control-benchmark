#include "lia_sender_enhance.h"
#include "rtt_stats.h"
#include "unacked_packet_map.h"
#include "flag_impl.h"
#include "flag_util_impl.h"
#include "logging.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <iostream>
#include "couple_cc_manager.h"
namespace dqc{
namespace {
// Maximum window to allow when doing bandwidth resumption.
const QuicPacketCount kMaxResumptionCongestionWindow = 200;
// Constants based on TCP defaults.
const QuicByteCount kMaxBurstBytes = 3 * kDefaultTCPMSS;
const float kWestwoodDelayBeta = 0.8f;
const float kWestwoodLossBeta = 0.8f;  // add to reduce loss for westwood
const float kRenoBeta = 0.5f;
const TimeDelta kMinRttExpiry = TimeDelta::FromMilliseconds(2500);
// The minimum cwnd based on RFC 3782 (TCP NewReno) for cwnd reductions on a
// fast retransmission.
const QuicByteCount kDefaultMinimumCongestionWindow = 4* kDefaultTCPMSS;
const float kDerivedHighCWNDGain = 2.0f;
const float kBandwidthWestwoodAlpha = 0.125f;
const QuicRoundTripCount kBandwidthWindowSize=5;//10;
const float kSimilarMinRttThreshold = 1.125;
const float kStartupGrowthTarget = 1.5;
const float kStartupAfterLossGain = 1.5f;
const QuicRoundTripCount kRoundTripsWithoutGrowthBeforeExitingStartup = 3;
const float kLatencyFactor=0.7;
const QuicRoundTripCount kMaxRttObservationWindow=20;
static const int alpha_scale_den = 10;
static const int alpha_scale_num = 32;
static const int alpha_scale = 12;
}  // namespace
static inline uint64_t mptcp_ccc_scale(uint32_t val, int scale)
{
	return (uint64_t) val << scale;
}
LiaSenderEnhance::LiaSenderEnhance(
    const ProtoClock* clock,
    const RttStats* rtt_stats,
    const UnackedPacketMap* unacked_packets,
    QuicPacketCount initial_tcp_congestion_window,
    QuicPacketCount max_congestion_window,
    QuicConnectionStats* stats,bool loss_diff)
    :rtt_stats_(rtt_stats),
    unacked_packets_(unacked_packets),
    stats_(stats),
    num_connections_(kDefaultNumConnections),
    min4_mode_(false),
    last_cutback_exited_slowstart_(false),
    slow_start_large_reduction_(false),
    no_prr_(false),
    num_acked_packets_(0),
    congestion_window_(initial_tcp_congestion_window * kDefaultTCPMSS),
    min_congestion_window_(kDefaultMinimumCongestionWindow),
    max_congestion_window_(max_congestion_window * kDefaultTCPMSS),
    slowstart_threshold_(max_congestion_window * kDefaultTCPMSS),
    initial_tcp_congestion_window_(initial_tcp_congestion_window *
                                   kDefaultTCPMSS),
    initial_max_tcp_congestion_window_(max_congestion_window *
                                       kDefaultTCPMSS),
    min_slow_start_exit_window_(min_congestion_window_),
    reset_rtt_min_(true),
    mode_(STARTUP),
    round_trip_count_(0),
    max_bandwidth_(kBandwidthWindowSize, QuicBandwidth::Zero(), 0),
    high_gain_(kDerivedHighCWNDGain/*kDefaultHighGain*/),
    high_cwnd_gain_(kDerivedHighCWNDGain/*kDefaultHighGain*/),
    drain_gain_(1.f /kDerivedHighCWNDGain/*kDefaultHighGain*/),
    pacing_rate_(QuicBandwidth::Zero()),
    pacing_gain_(1),
    congestion_window_gain_(1),
    num_startup_rtts_(kRoundTripsWithoutGrowthBeforeExitingStartup),
    is_at_full_bandwidth_(false),
    rounds_without_bandwidth_gain_(0),
    bandwidth_at_last_round_(QuicBandwidth::Zero()),
    min_rtt_since_last_probe_rtt_(TimeDelta::Infinite()),
    startup_rate_reduction_multiplier_(0),
    startup_bytes_lost_(0),
    always_get_bw_sample_when_acked_(
    GetQuicReloadableFlag(quic_always_get_bw_sample_when_acked)),
    min_rtt_(TimeDelta::Zero()),
    min_rtt_timestamp_(ProtoTime::Zero()),
    probe_rtt_skipped_if_similar_rtt_(false),
    exit_startup_on_loss_(false),
    max_rtt_(kMaxRttObservationWindow,TimeDelta::Zero(),0),
    loss_diff_(loss_diff){
        EnterStartupMode(clock->Now());
    }

LiaSenderEnhance::~LiaSenderEnhance() {}
void LiaSenderEnhance::AdjustNetworkParameters(
    QuicBandwidth bandwidth,
    TimeDelta rtt,
    bool /*allow_cwnd_to_decrease*/) {
  if (bandwidth.IsZero() || rtt.IsZero()) {
    return;
  }
  SetCongestionWindowFromBandwidthAndRtt(bandwidth, rtt);
}
void LiaSenderEnhance::OnCongestionEvent(
    bool rtt_updated,
    QuicByteCount prior_in_flight,
    ProtoTime event_time,
    const AckedPacketVector& acked_packets,
    const LostPacketVector& lost_packets) {
    bool is_round_start = false;
    bool min_rtt_expired = false;
    QuicBandwidth bandwidth=QuicBandwidth::Zero();
    DiscardLostPackets(lost_packets);
    TimeDelta srtt = rtt_stats_->smoothed_rtt();
    TimeDelta rtt=rtt_stats_->latest_rtt();
    if (!acked_packets.empty()) {
	    QuicPacketNumber last_acked_packet = acked_packets.rbegin()->packet_number;
	    is_round_start = UpdateRoundTripCounter(last_acked_packet);
	    min_rtt_expired = UpdateBandwidthAndMinRtt(event_time, acked_packets,bandwidth);
        max_rtt_.Update(srtt,round_trip_count_);
    }
    if(mode_==AIMD){
        if(min_rtt_expired){
            if(!acked_packets.empty()){
                QuicPacketNumber last_acked_packet = acked_packets.rbegin()->packet_number;
                CongestionWindowBackoff(last_acked_packet,prior_in_flight,kWestwoodDelayBeta);
            }
        }
        bool congestion_loss=true;
        for (const LostPacket& lost_packet : lost_packets) {
            if(loss_diff_&&(rtt<GetDelayThreshold())){
                congestion_loss=false;
            }
            if(congestion_loss){
            OnPacketLost(lost_packet.packet_number, lost_packet.bytes_lost,
                        prior_in_flight);            
        }
        }
        for (const AckedPacket acked_packet : acked_packets) {
        OnPacketAcked(acked_packet.packet_number, acked_packet.bytes_acked,
                        prior_in_flight, event_time);
        }

    }else{
        if (is_round_start && !is_at_full_bandwidth_) {
            CheckIfFullBandwidthReached();
        }
        MaybeExitStartupOrDrain(event_time);
        CalculatePacingRate();
        CalculateCongestionWindow();
    }
    sampler_.RemoveObsoletePackets(unacked_packets_->GetLeastUnacked());
}
void LiaSenderEnhance::OnPacketAcked(QuicPacketNumber acked_packet_number,
                                        QuicByteCount acked_bytes,
                                        QuicByteCount prior_in_flight,
                                        ProtoTime event_time) {
  largest_acked_packet_number_.UpdateMax(acked_packet_number);
  if (InRecovery()) {
    if (!no_prr_) {
      // PRR is used when in recovery.
      prr_.OnPacketAcked(acked_bytes);
    }
    return;
  }
  MaybeIncreaseCwnd(acked_packet_number, acked_bytes, prior_in_flight,
                    event_time);
}

void LiaSenderEnhance::OnPacketSent(
    ProtoTime sent_time,
    QuicByteCount bytes_in_flight,
    QuicPacketNumber packet_number,
    QuicByteCount bytes,
    HasRetransmittableData is_retransmittable){
  if (InSlowStart()) {
    ++(stats_->slowstart_packets_sent);
  }

  if (is_retransmittable != HAS_RETRANSMITTABLE_DATA) {
    return;
  }
  if (InRecovery()) {
    // PRR is used when in recovery.
    prr_.OnPacketSent(bytes);
  }
  DCHECK(!largest_sent_packet_number_.IsInitialized() ||
         largest_sent_packet_number_ < packet_number);
  largest_sent_packet_number_ = packet_number;
  sampler_.OnPacketSent(sent_time, packet_number, bytes, bytes_in_flight,
                        is_retransmittable);
}

bool LiaSenderEnhance::CanSend(QuicByteCount bytes_in_flight) {
  if (!no_prr_ && InRecovery()) {
    // PRR is used when in recovery.
    return prr_.CanSend(GetCongestionWindow(), bytes_in_flight,
                        GetSlowStartThreshold());
  }
  if (GetCongestionWindow() > bytes_in_flight) {
    return true;
  }
  if (min4_mode_ && bytes_in_flight < 4 * kDefaultTCPMSS) {
    return true;
  }
  return false;
}

QuicBandwidth LiaSenderEnhance::PacingRate(
    QuicByteCount /* bytes_in_flight */) const {
  if (pacing_rate_.IsZero()) {
    return high_gain_ * QuicBandwidth::FromBytesAndTimeDelta(
                            initial_tcp_congestion_window_, GetMinRtt());
  }
  if(mode_==AIMD){
  TimeDelta srtt = rtt_stats_->SmoothedOrInitialRtt();
  const QuicBandwidth bandwidth =
      QuicBandwidth::FromBytesAndTimeDelta(GetCongestionWindow(), srtt);
  return bandwidth * ((no_prr_ && InRecovery() ? 1 : 1.25));
  }else{
      return pacing_rate_;
  }
}
QuicBandwidth LiaSenderEnhance::BandwidthEstimate() const {
  TimeDelta srtt = rtt_stats_->smoothed_rtt();
  if(mode_==STARTUP||mode_==DRAIN){
      return BandwidthEstimateBest();
  }
  return QuicBandwidth::FromBytesAndTimeDelta(GetCongestionWindow(), srtt);
}
QuicBandwidth LiaSenderEnhance::BandwidthEstimateBest() const {
    return max_bandwidth_.GetBest();
}
bool LiaSenderEnhance::InSlowStart() const {
  return mode_ == STARTUP||mode_==DRAIN;
}

bool LiaSenderEnhance::IsCwndLimited(QuicByteCount bytes_in_flight) const {
  const QuicByteCount congestion_window = GetCongestionWindow();
  if (bytes_in_flight >= congestion_window) {
    return true;
  }
  const QuicByteCount available_bytes = congestion_window - bytes_in_flight;
  const bool slow_start_limited =
      InSlowStart() && bytes_in_flight > congestion_window / 2;
  return slow_start_limited || available_bytes <= kMaxBurstBytes;
}

bool LiaSenderEnhance::InRecovery() const {
  return largest_acked_packet_number_.IsInitialized() &&
         largest_sent_at_last_cutback_.IsInitialized() &&
         largest_acked_packet_number_ <= largest_sent_at_last_cutback_;
}

bool LiaSenderEnhance::ShouldSendProbingPacket() const {
  return false;
}

void LiaSenderEnhance::OnRetransmissionTimeout(bool packets_retransmitted) {
  largest_sent_at_last_cutback_.Clear();
  if (!packets_retransmitted) {
    return;
  }
  HandleRetransmissionTimeout();
}

std::string LiaSenderEnhance::GetDebugState() const {
  return "";
}

void LiaSenderEnhance::OnApplicationLimited(QuicByteCount bytes_in_flight) {}

void LiaSenderEnhance::SetCongestionWindowFromBandwidthAndRtt(
    QuicBandwidth bandwidth,
    TimeDelta rtt) {
  QuicByteCount new_congestion_window = bandwidth.ToBytesPerPeriod(rtt);
  // Limit new CWND if needed.
  congestion_window_ =
      std::max(min_congestion_window_,
               std::min(new_congestion_window,
                        kMaxResumptionCongestionWindow * kDefaultTCPMSS));
}

void LiaSenderEnhance::SetInitialCongestionWindowInPackets(
    QuicPacketCount congestion_window) {
  congestion_window_ = congestion_window * kDefaultTCPMSS;
}

void LiaSenderEnhance::SetMinCongestionWindowInPackets(
    QuicPacketCount congestion_window) {
  min_congestion_window_ = congestion_window * kDefaultTCPMSS;
}

void LiaSenderEnhance::SetNumEmulatedConnections(int num_connections) {
  num_connections_ = std::max(1, num_connections);
}
void LiaSenderEnhance::CongestionWindowBackoff(QuicPacketNumber packet_number,QuicByteCount prior_in_flight,float gain){
  if (largest_sent_at_last_cutback_.IsInitialized() &&
      packet_number <= largest_sent_at_last_cutback_) {
          return ;
    }
  if (!no_prr_) {
    prr_.OnPacketLost(prior_in_flight);
  }
  QuicBandwidth bw_est=BandwidthEstimateBest();
  if(bw_est.IsZero()){
      congestion_window_=congestion_window_*RenoBeta();
  }else{
      congestion_window_=gain*bw_est*min_rtt_;
  }
  if (congestion_window_ < min_congestion_window_) {
    congestion_window_ = min_congestion_window_;
  }
  slowstart_threshold_ = congestion_window_;
  largest_sent_at_last_cutback_ = largest_sent_packet_number_;
  reset_rtt_min_=true;
  num_acked_packets_ = 0;
}
void LiaSenderEnhance::OnPacketLost(QuicPacketNumber packet_number,
                                       QuicByteCount lost_bytes,
                                       QuicByteCount prior_in_flight) {
  // TCP NewReno (RFC6582) says that once a loss occurs, any losses in packets
  // already sent should be treated as a single loss event, since it's expected.
  if (largest_sent_at_last_cutback_.IsInitialized() &&
      packet_number <= largest_sent_at_last_cutback_) {
    if (last_cutback_exited_slowstart_) {
      ++stats_->slowstart_packets_lost;
      stats_->slowstart_bytes_lost += lost_bytes;
      if (slow_start_large_reduction_) {
        // Reduce congestion window by lost_bytes for every loss.
        congestion_window_ = std::max(congestion_window_ - lost_bytes,
                                      min_slow_start_exit_window_);
        slowstart_threshold_ = congestion_window_;
      }
    }
    /*QUIC_DVLOG(1)*/DLOG(INFO)<< "Ignoring loss for largest_missing:" << packet_number
                  << " because it was sent prior to the last CWND cutback.";
    return;
  }
  ++stats_->tcp_loss_events;
  last_cutback_exited_slowstart_ = InSlowStart();
  if (!no_prr_) {
    prr_.OnPacketLost(prior_in_flight);
  }

  // TODO(b/77268641): Separate out all of slow start into a separate class.
  if (slow_start_large_reduction_ && InSlowStart()) {
    DCHECK_LT(kDefaultTCPMSS, congestion_window_);
    if (congestion_window_ >= 2 * initial_tcp_congestion_window_) {
      min_slow_start_exit_window_ = congestion_window_ / 2;
    }
    congestion_window_ = congestion_window_ - kDefaultTCPMSS;
  } else{
      QuicBandwidth bw_est=BandwidthEstimateBest();
      if(bw_est.IsZero()){
          congestion_window_=congestion_window_*RenoBeta();
      }else{
          QuicBandwidth  rate=BandwidthEstimateBest();
          congestion_window_=kWestwoodLossBeta*rate*min_rtt_;
      }
  }
  if (congestion_window_ < min_congestion_window_) {
    congestion_window_ = min_congestion_window_;
  }
  slowstart_threshold_ = congestion_window_;
  largest_sent_at_last_cutback_ = largest_sent_packet_number_;
  //reset_rtt_min_=true;
  num_acked_packets_ = 0;
  delay_yield_flag_=true;
  DLOG(INFO)<< "Incoming loss; congestion window: " << congestion_window_
                << " slowstart threshold: " << slowstart_threshold_;
}

QuicByteCount LiaSenderEnhance::GetCongestionWindow() const {
  return congestion_window_;
}

QuicByteCount LiaSenderEnhance::GetSlowStartThreshold() const {
  return slowstart_threshold_;
}

// Called when we receive an ack. Normal TCP tracks how many packets one ack
// represents, but quic has a separate ack for each packet.
void LiaSenderEnhance::MaybeIncreaseCwnd(
    QuicPacketNumber acked_packet_number,
    QuicByteCount acked_bytes,
    QuicByteCount prior_in_flight,
    ProtoTime event_time) {
  if (!IsCwndLimited(prior_in_flight)) {
    return;
  }
  if (congestion_window_ >= max_congestion_window_) {
    return;
  }
  ++num_acked_packets_;
  if(!other_ccs_.empty()){
	  bool subflows_exit_slow_start=true;
	  for(auto it=other_ccs_.begin();it!=other_ccs_.end();it++){
		   LiaSenderEnhance *sender=(*it);
		  if(sender->InSlowStart()){
			subflows_exit_slow_start=false;
			break;
		  }
	  }
	  if(subflows_exit_slow_start){
		  mptcp_ccc_recalc_alpha();
		  uint64_t send_cwnd=0;
		  send_cwnd =mptcp_ccc_scale(1, alpha_scale)/alpha_;
		  if(send_cwnd<congestion_window_ / kDefaultTCPMSS){
			  send_cwnd=congestion_window_ / kDefaultTCPMSS;
		  }
		  if(num_acked_packets_*num_connections_>=send_cwnd){
			  congestion_window_ += kDefaultTCPMSS;
			  num_acked_packets_=0;
		  }
	  }else{
		    if (num_acked_packets_ * num_connections_ >=
		        congestion_window_ / kDefaultTCPMSS) {
		      congestion_window_ += kDefaultTCPMSS;
		      num_acked_packets_ = 0;
		    }
	  }
  }else{
	    if (num_acked_packets_ * num_connections_ >=
	        congestion_window_ / kDefaultTCPMSS) {
	      congestion_window_ += kDefaultTCPMSS;
	      num_acked_packets_ = 0;
	    }
  }
  DLOG(INFO)<< "Lia congestion window: " << congestion_window_
                << " slowstart threshold: " << slowstart_threshold_
                << " congestion window count: " << num_acked_packets_;
}

void LiaSenderEnhance::HandleRetransmissionTimeout() {
  slowstart_threshold_ = congestion_window_ / 2;
  congestion_window_ = min_congestion_window_;
}
void LiaSenderEnhance::EnterStartupMode(ProtoTime now) {
  mode_ = STARTUP;
  pacing_gain_ = high_gain_;
  congestion_window_gain_ = high_cwnd_gain_;
}

void LiaSenderEnhance::EnterAIMDMode(ProtoTime now) {
  mode_ = AIMD;
  congestion_window_gain_ =1.0;
  congestion_window_=GetTargetCongestionWindow(congestion_window_gain_);
}
void LiaSenderEnhance::DiscardLostPackets(const LostPacketVector& lost_packets) {
  for (const LostPacket& packet : lost_packets) {
    sampler_.OnPacketLost(packet.packet_number);
    if (mode_ == STARTUP) {
      if (startup_rate_reduction_multiplier_ != 0) {
        startup_bytes_lost_ += packet.bytes_lost;
      }
    }
  }
}
bool LiaSenderEnhance::UpdateRoundTripCounter(QuicPacketNumber last_acked_packet) {
  if (!current_round_trip_end_.IsInitialized()||
      last_acked_packet > current_round_trip_end_) {
    round_trip_count_++;
    current_round_trip_end_ =largest_sent_packet_number_;
    return true;
  }
  return false;
}
bool LiaSenderEnhance::UpdateBandwidthAndMinRtt(
    ProtoTime now,
    const AckedPacketVector& acked_packets,QuicBandwidth &bandwidth) {
  TimeDelta sample_min_rtt = TimeDelta::Infinite();
  bandwidth=QuicBandwidth::Zero();
  for (const auto& packet : acked_packets) {
    if (!always_get_bw_sample_when_acked_ && packet.bytes_acked == 0) {
      // Skip acked packets with 0 in flight bytes when updating bandwidth.
      continue;
    }
    BandwidthSample bandwidth_sample =
        sampler_.OnPacketAcknowledged(now, packet.packet_number);
    if (always_get_bw_sample_when_acked_ &&
        !bandwidth_sample.state_at_send.is_valid) {
      // From the sampler's perspective, the packet has never been sent, or the
      // packet has been acked or marked as lost previously.
      continue;
    }
    if (!bandwidth_sample.rtt.IsZero()) {
      sample_min_rtt = std::min(sample_min_rtt, bandwidth_sample.rtt);
    }
    bandwidth=std::max(bandwidth,bandwidth_sample.bandwidth);
    max_bandwidth_.Update(bandwidth_sample.bandwidth, round_trip_count_);
  }

  // If none of the RTT samples are valid, return immediately.
  if (sample_min_rtt.IsInfinite()) {
    return false;
  }
  if(reset_rtt_min_){
      min_rtt_timestamp_=now;
      min_rtt_=sample_min_rtt;
      reset_rtt_min_=false;
      return false;
  }
  min_rtt_since_last_probe_rtt_ =
      std::min(min_rtt_since_last_probe_rtt_, sample_min_rtt);

  // Do not expire min_rtt if none was ever available.
  bool min_rtt_expired =
      !min_rtt_.IsZero() && (now > (min_rtt_timestamp_ + kMinRttExpiry));

  if (min_rtt_expired || sample_min_rtt < min_rtt_ || min_rtt_.IsZero()) {

    if (min_rtt_expired && ShouldExtendMinRttExpiry()) {
      min_rtt_expired = false;
    } else {
      min_rtt_ = sample_min_rtt;
    }
    min_rtt_timestamp_ = now;
    // Reset since_last_probe_rtt fields.
    min_rtt_since_last_probe_rtt_ = TimeDelta::Infinite();
  }
  return min_rtt_expired;
}
bool LiaSenderEnhance::ShouldExtendMinRttExpiry() const {
  const bool min_rtt_increased_since_last_probe =
      min_rtt_since_last_probe_rtt_ > min_rtt_ * kSimilarMinRttThreshold;
  if (probe_rtt_skipped_if_similar_rtt_ &&
      !min_rtt_increased_since_last_probe) {
    // Extend the current min_rtt if we've been app limited recently and an rtt
    // has been measured in that time that's less than 12.5% more than the
    // current min_rtt.
    return true;
  }
  return false;
}
void LiaSenderEnhance::CheckIfFullBandwidthReached(){
  QuicBandwidth target = bandwidth_at_last_round_ * kStartupGrowthTarget;
  if (BandwidthEstimateBest() >= target) {
    bandwidth_at_last_round_ = BandwidthEstimateBest();
    rounds_without_bandwidth_gain_ = 0;
    return;
  }
  rounds_without_bandwidth_gain_++;
  if ((rounds_without_bandwidth_gain_ >= num_startup_rtts_) ||
      (exit_startup_on_loss_ && InRecovery())) {
    is_at_full_bandwidth_ = true;
  }    
}
void LiaSenderEnhance::MaybeExitStartupOrDrain(ProtoTime now) {
  if (mode_ == STARTUP && is_at_full_bandwidth_) {
    mode_ = DRAIN;
    pacing_gain_ = drain_gain_;
    congestion_window_gain_ = high_cwnd_gain_;
  }
  if (mode_ == DRAIN &&
      unacked_packets_->bytes_in_flight() <= GetTargetCongestionWindow(1)) {
    EnterAIMDMode(now);
  }
}
void LiaSenderEnhance::CalculatePacingRate(){
  if (BandwidthEstimateBest().IsZero()) {
    return;
  }

  QuicBandwidth target_rate = pacing_gain_ * BandwidthEstimateBest();
  if (is_at_full_bandwidth_) {
    pacing_rate_ = target_rate;
    return;
  }
  if (pacing_rate_.IsZero() && !rtt_stats_->min_rtt().IsZero()) {
    pacing_rate_ = QuicBandwidth::FromBytesAndTimeDelta(
        initial_tcp_congestion_window_, rtt_stats_->min_rtt());
    return;
  }
  pacing_rate_ = std::max(pacing_rate_, target_rate);
}
void LiaSenderEnhance::CalculateCongestionWindow(){
  QuicByteCount target_window =
      GetTargetCongestionWindow(congestion_window_gain_);
  congestion_window_=target_window;
  congestion_window_ = std::max(congestion_window_, min_congestion_window_);
  congestion_window_ = std::min(congestion_window_, max_congestion_window_);
}
float LiaSenderEnhance::RenoBeta() const {
  return (num_connections_ - 1 + kRenoBeta) / num_connections_;
}
TimeDelta LiaSenderEnhance::GetMinRtt() const {
  return !min_rtt_.IsZero() ? min_rtt_ : rtt_stats_->initial_rtt();
}

QuicByteCount LiaSenderEnhance::GetTargetCongestionWindow(float gain) const {
  QuicByteCount bdp = GetMinRtt() * BandwidthEstimateBest();
  QuicByteCount congestion_window = gain * bdp;

  // BDP estimate will be zero if no bandwidth samples are available yet.
  if (congestion_window == 0) {
    congestion_window = gain * initial_tcp_congestion_window_;
  }

  return std::max(congestion_window, min_congestion_window_);
}
void LiaSenderEnhance::OnConnectionMigration() {
  prr_ = PrrSender();
  largest_sent_packet_number_.Clear();
  largest_acked_packet_number_.Clear();
  largest_sent_at_last_cutback_.Clear();
  last_cutback_exited_slowstart_ = false;
  num_acked_packets_ = 0;
  congestion_window_ = initial_tcp_congestion_window_;
  max_congestion_window_ = initial_max_tcp_congestion_window_;
  slowstart_threshold_ = initial_max_tcp_congestion_window_;
}

CongestionControlType LiaSenderEnhance::GetCongestionControlType() const {
  return kLiaEnhance;
}
void LiaSenderEnhance::SetCongestionId(uint32_t cid){
	if(congestion_id_!=0||cid==0){
		return;
	}
	congestion_id_=cid;
	CoupleManager::Instance()->OnCongestionCreate(this);
}
void LiaSenderEnhance::RegisterCoupleCC(SendAlgorithmInterface*cc){
	bool exist=false;
    LiaSenderEnhance *sender=dynamic_cast<LiaSenderEnhance*>(cc);
    if(this==sender) {return ;}
	for(auto it=other_ccs_.begin();it!=other_ccs_.end();it++){
		if(sender==(*it)){
			exist=true;
			break;
		}
	}
	if(!exist){
		other_ccs_.push_back(sender);
	}
}
void LiaSenderEnhance::UnRegisterCoupleCC(SendAlgorithmInterface*cc){
    LiaSenderEnhance *sender=dynamic_cast<LiaSenderEnhance*>(cc);
	if(!other_ccs_.empty()){
		other_ccs_.remove(sender);
	}
}
uint64_t LiaSenderEnhance::get_srtt_us() const{
	return rtt_stats_->smoothed_rtt().ToMicroseconds();
}
void LiaSenderEnhance::mptcp_ccc_recalc_alpha(){
 if(!other_ccs_.empty()){
	uint64_t max_numerator=0,sum_denominator = 0, alpha = 1;
	uint32_t best_cwnd = 0;
	uint64_t best_rtt = 0;
	uint32_t send_cwnd=GetCongestionWindow()/kDefaultTCPMSS;
	uint64_t srtt=get_srtt_us();
	best_rtt=srtt;
	best_cwnd=send_cwnd;
	max_numerator=mptcp_ccc_scale(send_cwnd,
			alpha_scale_num)/(srtt*srtt);
	for(auto it=other_ccs_.begin();it!=other_ccs_.end();it++){
		LiaSenderEnhance *sender=(*it);
		send_cwnd=sender->GetCongestionWindow()/kDefaultTCPMSS;
		srtt=sender->get_srtt_us();
		uint64_t tmp=mptcp_ccc_scale(send_cwnd,
				alpha_scale_num)/(srtt*srtt);
		if(tmp>max_numerator){
			max_numerator=tmp;
			best_rtt=srtt;
			best_cwnd=send_cwnd;
		}
	}
	send_cwnd=GetCongestionWindow()/kDefaultTCPMSS;
	srtt=get_srtt_us();
	sum_denominator+=mptcp_ccc_scale(send_cwnd,
			alpha_scale_den) * best_rtt/srtt;
	for(auto it=other_ccs_.begin();it!=other_ccs_.end();it++){
			LiaSenderEnhance *sender=(*it);
			send_cwnd=sender->GetCongestionWindow()/kDefaultTCPMSS;
			srtt=sender->get_srtt_us();
			sum_denominator+=mptcp_ccc_scale(send_cwnd,
					alpha_scale_den) * best_rtt/srtt;
	}
	sum_denominator *= sum_denominator;
	CHECK(sum_denominator>0);
	alpha = mptcp_ccc_scale(best_cwnd, alpha_scale_num)/sum_denominator;
	CHECK(alpha>=1);
	alpha_=alpha;
	for(auto it=other_ccs_.begin();it!=other_ccs_.end();it++){
		LiaSenderEnhance *sender=(*it);
		sender->set_alpha(alpha);
	}
 }
}
TimeDelta LiaSenderEnhance::GetDelayThreshold(){
    TimeDelta threshold=TimeDelta::Zero();
    TimeDelta max_rtt=max_rtt_.GetBest();
    if(max_rtt>min_rtt_){
        threshold=min_rtt_+kLatencyFactor*(max_rtt-min_rtt_);
    }
    return threshold;
}
}
