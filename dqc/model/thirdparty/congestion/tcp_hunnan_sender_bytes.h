#pragma once
#include <iostream>
#include <fstream>
#include "proto_types.h"
#include "prr_sender.h"
#include "proto_windowed_filter.h"
#include "proto_bandwidth_sampler.h"
#include "proto_send_algorithm_interface.h"
#include "rtt_stats.h"
#include "optional.h"
namespace dqc{
class TcpHunnanSenderBytes : public SendAlgorithmInterface {
public:
    enum Mode {
        STARTUP,
        DRAIN,
        AIMD,
    };
  TcpHunnanSenderBytes(const ProtoClock* clock,
                      const RttStats* rtt_stats,
                      const UnackedPacketMap* unacked_packets,
                      QuicPacketCount initial_tcp_congestion_window,
                      QuicPacketCount max_congestion_window,
                      QuicConnectionStats* stats,Random* random);
  TcpHunnanSenderBytes(const TcpHunnanSenderBytes&) = delete;
  TcpHunnanSenderBytes& operator=(const TcpHunnanSenderBytes&) = delete;
  ~TcpHunnanSenderBytes() override;

  // Start implementation of SendAlgorithmInterface.
  //void SetFromConfig(const QuicConfig& config,
  //                   Perspective perspective) override;
  void AdjustNetworkParameters(QuicBandwidth bandwidth,
                               TimeDelta rtt,
                               bool allow_cwnd_to_decrease) override;
  void SetNumEmulatedConnections(int num_connections) override;
  void SetInitialCongestionWindowInPackets(
      QuicPacketCount congestion_window) override;
  void OnConnectionMigration() override;
  void OnCongestionEvent(bool rtt_updated,
                         QuicByteCount prior_in_flight,
                         ProtoTime event_time,
                         const AckedPacketVector& acked_packets,
                         const LostPacketVector& lost_packets) override;
  void OnPacketSent(ProtoTime sent_time,
                    QuicByteCount bytes_in_flight,
                    QuicPacketNumber packet_number,
                    QuicByteCount bytes,
                    HasRetransmittableData is_retransmittable) override;
  void OnRetransmissionTimeout(bool packets_retransmitted) override;
  bool CanSend(QuicByteCount bytes_in_flight) override;
  QuicBandwidth PacingRate(QuicByteCount bytes_in_flight) const override;
  QuicBandwidth BandwidthEstimate() const override;
  QuicByteCount GetCongestionWindow() const override;
  QuicByteCount GetSlowStartThreshold() const override;
  CongestionControlType GetCongestionControlType() const override;
  bool InSlowStart() const override;
  bool InRecovery() const override;
  bool ShouldSendProbingPacket() const override;
  std::string GetDebugState() const override;
  void OnApplicationLimited(QuicByteCount bytes_in_flight) override;
  void SetCongestionId(uint32_t cid) override;
  uint32_t GetCongestionId() override{ return congestion_id_;}
  // End implementation of SendAlgorithmInterface.
  QuicByteCount min_congestion_window() const { return min_congestion_window_; }
  bool IsInAimdState(){return mode_==AIMD;}
  protected:
  bool IsCwndLimited(QuicByteCount bytes_in_flight) const;

  // TODO(ianswett): Remove these and migrate to OnCongestionEvent.
  void OnPacketAcked(QuicPacketNumber acked_packet_number,
                     QuicByteCount acked_bytes,
                     QuicByteCount prior_in_flight,
                     ProtoTime event_time);
  void SetCongestionWindowFromBandwidthAndRtt(QuicBandwidth bandwidth,
                                              TimeDelta rtt);
  void SetMinCongestionWindowInPackets(QuicPacketCount congestion_window);
  void CongestionWindowBackoff(ProtoTime event_time,QuicPacketNumber packet_number,QuicByteCount prior_in_flight,float gain);
  void OnPacketLost(QuicPacketNumber largest_loss,
                    QuicByteCount lost_bytes,
                    QuicByteCount prior_in_flight);
  void MaybeIncreaseCwnd(QuicPacketNumber acked_packet_number,
                         QuicByteCount acked_bytes,
                         QuicByteCount prior_in_flight,
                         ProtoTime event_time);
  void HandleRetransmissionTimeout();
  void EnterStartupMode(ProtoTime now);
  void EnterAIMDMode(ProtoTime now);
  void DiscardLostPackets(const LostPacketVector& lost_packets);
  bool UpdateRoundTripCounter(QuicPacketNumber last_acked_packet);
  bool UpdateBandwidthAndMinRtt(ProtoTime now,
  const AckedPacketVector& acked_packets,QuicBandwidth &bandwidth);
  bool ShouldExtendMinRttExpiry() const;
  void CheckIfFullBandwidthReached();
  void MaybeExitStartupOrDrain(ProtoTime now);
  void CalculatePacingRate();
  void CalculateCongestionWindow();
  QuicBandwidth BandwidthEstimateBest() const;
  TimeDelta GetMinRtt() const;
  QuicByteCount GetTargetCongestionWindow(float gain) const;
  float RenoBeta() const;
  TimeDelta GetDelayThreshold();
private:
    const ProtoClock *clock_;
    PrrSender prr_;
    const RttStats* rtt_stats_;
    const UnackedPacketMap* unacked_packets_;
    QuicConnectionStats* stats_;
    Random *random_;
    bool half_cwnd_on_loss_{true};
    // Number of connections to simulate.
    uint32_t num_connections_;
    
    // Track the largest packet that has been sent.
    QuicPacketNumber largest_sent_packet_number_;
    
    // Track the largest packet that has been acked.
    QuicPacketNumber largest_acked_packet_number_;
    
    // Track the largest packet number outstanding when a CWND cutback occurs.
    QuicPacketNumber largest_sent_at_last_cutback_;
    
    // Whether to use 4 packets as the actual min, but pace lower.
    bool min4_mode_;

    // Whether the last loss event caused us to exit slowstart.
    // Used for stats collection of slowstart_packets_lost
    bool last_cutback_exited_slowstart_;
    
    // When true, exit slow start with large cutback of congestion window.
    bool slow_start_large_reduction_;
    
    // When true, use unity pacing instead of PRR.
    bool no_prr_;
    // ACK counter for the Reno implementation.
    uint64_t num_acked_packets_;
    
    // Congestion window in bytes.
    QuicByteCount congestion_window_;
    
    // Minimum congestion window in bytes.
    QuicByteCount min_congestion_window_;
    
    // Maximum congestion window in bytes.
    QuicByteCount max_congestion_window_;
    
    // Slow start congestion window in bytes, aka ssthresh.
    QuicByteCount slowstart_threshold_;

    // Initial TCP congestion window in bytes. This variable can only be set when
    // this algorithm is created.
    const QuicByteCount initial_tcp_congestion_window_;
    
    // Initial maximum TCP congestion window in bytes. This variable can only be
    // set when this algorithm is created.
    const QuicByteCount initial_max_tcp_congestion_window_;
    
    // The minimum window when exiting slow start with large reduction.
    QuicByteCount min_slow_start_exit_window_;
    typedef WindowedFilter<QuicBandwidth,
                         MaxFilter<QuicBandwidth>,
                         QuicRoundTripCount,
                         QuicRoundTripCount> MaxBandwidthFilter;
    typedef WindowedFilter<TimeDelta,MaxFilter<TimeDelta>,QuicRoundTripCount,QuicRoundTripCount> RTTFilter;                     
    Mode mode_;
    BandwidthSampler sampler_;
    // The number of the round trips that have occurred during the connection.
    QuicRoundTripCount round_trip_count_;
    QuicPacketNumber current_round_trip_end_;
    MaxBandwidthFilter max_bandwidth_;
    float high_gain_;
    float high_cwnd_gain_;
    float drain_gain_;
    QuicBandwidth pacing_rate_;
    float pacing_gain_;
    float congestion_window_gain_;
    // The number of RTTs to stay in STARTUP mode.  Defaults to 3.
    QuicRoundTripCount num_startup_rtts_;
    // Indicates whether the connection has reached the full bandwidth mode.
    bool is_at_full_bandwidth_{false};
    // Number of rounds during which there was no significant bandwidth increase.
    QuicRoundTripCount rounds_without_bandwidth_gain_;
    // The bandwidth compared to which the increase is measured.
    QuicBandwidth bandwidth_at_last_round_;
    TimeDelta min_rtt_since_last_probe_rtt_;

    uint8_t startup_rate_reduction_multiplier_;
    QuicByteCount startup_bytes_lost_;
    const bool always_get_bw_sample_when_acked_;
    TimeDelta min_rtt_;
    ProtoTime min_rtt_timestamp_;
    TimeDelta base_rtt_;
    bool probe_rtt_skipped_if_similar_rtt_;
    bool exit_startup_on_loss_;
    uint32_t congestion_id_{0};
    RTTFilter max_rtt_;
    nonstd::optional<QuicByteCount> cwnd_before_delay_;
    ProtoTime yield_time_;
    bool delay_backoff_flag_{false};
    int increase_factor_{1};
};
}
