/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/video/video_receive_stream.h"

#include <stdlib.h>

#include <set>
#include <string>

#include "webrtc/base/checks.h"
#include "webrtc/base/logging.h"
#include "webrtc/call/congestion_controller.h"
#include "webrtc/common_video/libyuv/include/webrtc_libyuv.h"
#include "webrtc/modules/utility/include/process_thread.h"
#include "webrtc/system_wrappers/include/clock.h"
#include "webrtc/video/call_stats.h"
#include "webrtc/video/receive_statistics_proxy.h"
#include "webrtc/video_receive_stream.h"

namespace webrtc {

static bool UseSendSideBwe(const VideoReceiveStream::Config& config) {
  if (!config.rtp.transport_cc)
    return false;
  for (const auto& extension : config.rtp.extensions) {
    if (extension.name == RtpExtension::kTransportSequenceNumber)
      return true;
  }
  return false;
}

std::string VideoReceiveStream::Decoder::ToString() const {
  std::stringstream ss;
  ss << "{decoder: " << (decoder != nullptr ? "(VideoDecoder)" : "nullptr");
  ss << ", payload_type: " << payload_type;
  ss << ", payload_name: " << payload_name;
  ss << '}';

  return ss.str();
}

std::string VideoReceiveStream::Config::ToString() const {
  std::stringstream ss;
  ss << "{decoders: [";
  for (size_t i = 0; i < decoders.size(); ++i) {
    ss << decoders[i].ToString();
    if (i != decoders.size() - 1)
      ss << ", ";
  }
  ss << ']';
  ss << ", rtp: " << rtp.ToString();
  ss << ", renderer: " << (renderer != nullptr ? "(renderer)" : "nullptr");
  ss << ", render_delay_ms: " << render_delay_ms;
  if (!sync_group.empty())
    ss << ", sync_group: " << sync_group;
  ss << ", pre_decode_callback: "
     << (pre_decode_callback != nullptr ? "(EncodedFrameObserver)" : "nullptr");
  ss << ", pre_render_callback: "
     << (pre_render_callback != nullptr ? "(I420FrameCallback)" : "nullptr");
  ss << ", target_delay_ms: " << target_delay_ms;
  ss << '}';

  return ss.str();
}

std::string VideoReceiveStream::Config::Rtp::ToString() const {
  std::stringstream ss;
  ss << "{remote_ssrc: " << remote_ssrc;
  ss << ", local_ssrc: " << local_ssrc;
  ss << ", rtcp_mode: "
     << (rtcp_mode == RtcpMode::kCompound ? "RtcpMode::kCompound"
                                          : "RtcpMode::kReducedSize");
  ss << ", rtcp_xr: ";
  ss << "{receiver_reference_time_report: "
     << (rtcp_xr.receiver_reference_time_report ? "on" : "off");
  ss << '}';
  ss << ", remb: " << (remb ? "on" : "off");
  ss << ", transport_cc: " << (transport_cc ? "on" : "off");
  ss << ", nack: {rtp_history_ms: " << nack.rtp_history_ms << '}';
  ss << ", fec: " << fec.ToString();
  ss << ", rtx: {";
  for (auto& kv : rtx) {
    ss << kv.first << " -> ";
    ss << "{ssrc: " << kv.second.ssrc;
    ss << ", payload_type: " << kv.second.payload_type;
    ss << '}';
  }
  ss << '}';
  ss << ", extensions: [";
  for (size_t i = 0; i < extensions.size(); ++i) {
    ss << extensions[i].ToString();
    if (i != extensions.size() - 1)
      ss << ", ";
  }
  ss << ']';
  ss << '}';
  return ss.str();
}

namespace internal {
namespace {

VideoCodec CreateDecoderVideoCodec(const VideoReceiveStream::Decoder& decoder) {
  VideoCodec codec;
  memset(&codec, 0, sizeof(codec));

  codec.plType = decoder.payload_type;
  strncpy(codec.plName, decoder.payload_name.c_str(), sizeof(codec.plName));
  if (decoder.payload_name == "VP8") {
    codec.codecType = kVideoCodecVP8;
  } else if (decoder.payload_name == "VP9") {
    codec.codecType = kVideoCodecVP9;
  } else if (decoder.payload_name == "H264") {
    codec.codecType = kVideoCodecH264;
  } else {
    codec.codecType = kVideoCodecGeneric;
  }

  if (codec.codecType == kVideoCodecVP8) {
    codec.codecSpecific.VP8 = VideoEncoder::GetDefaultVp8Settings();
  } else if (codec.codecType == kVideoCodecVP9) {
    codec.codecSpecific.VP9 = VideoEncoder::GetDefaultVp9Settings();
  } else if (codec.codecType == kVideoCodecH264) {
    codec.codecSpecific.H264 = VideoEncoder::GetDefaultH264Settings();
  }

  codec.width = 320;
  codec.height = 180;
  codec.startBitrate = codec.minBitrate = codec.maxBitrate =
      Call::Config::kDefaultStartBitrateBps / 1000;

  return codec;
}
}  // namespace

VideoReceiveStream::VideoReceiveStream(
    int num_cpu_cores,
    CongestionController* congestion_controller,
    const VideoReceiveStream::Config& config,
    webrtc::VoiceEngine* voice_engine,
    ProcessThread* process_thread,
    CallStats* call_stats)
    : transport_adapter_(config.rtcp_send_transport),
      encoded_frame_proxy_(config.pre_decode_callback),
      config_(config),
      process_thread_(process_thread),
      clock_(Clock::GetRealTimeClock()),
      congestion_controller_(congestion_controller),
      call_stats_(call_stats),
      vcm_(VideoCodingModule::Create(clock_, nullptr, nullptr)),
      incoming_video_stream_(
          0,
          config.renderer ? config.renderer->SmoothsRenderedFrames() : false),
      stats_proxy_(config_.rtp.remote_ssrc, clock_),
      vie_channel_(&transport_adapter_,
                   process_thread,
                   nullptr,
                   vcm_.get(),
                   nullptr,
                   nullptr,
                   nullptr,
                   congestion_controller_->GetRemoteBitrateEstimator(
                       UseSendSideBwe(config_)),
                   call_stats_->rtcp_rtt_stats(),
                   congestion_controller_->pacer(),
                   congestion_controller_->packet_router(),
                   1,
                   false),
      vie_receiver_(vie_channel_.vie_receiver()),
      rtp_rtcp_(vie_channel_.rtp_rtcp()) {
  LOG(LS_INFO) << "VideoReceiveStream: " << config_.ToString();

  RTC_CHECK(vie_channel_.Init() == 0);

  // Register the channel to receive stats updates.
  call_stats_->RegisterStatsObserver(vie_channel_.GetStatsObserver());

  // TODO(pbos): This is not fine grained enough...
  vie_channel_.SetProtectionMode(config_.rtp.nack.rtp_history_ms > 0, false, -1,
                                 -1);
  RTC_DCHECK(config_.rtp.rtcp_mode != RtcpMode::kOff)
      << "A stream should not be configured with RTCP disabled. This value is "
         "reserved for internal usage.";
  rtp_rtcp_->SetRTCPStatus(config_.rtp.rtcp_mode);

  RTC_DCHECK(config_.rtp.remote_ssrc != 0);
  // TODO(pbos): What's an appropriate local_ssrc for receive-only streams?
  RTC_DCHECK(config_.rtp.local_ssrc != 0);
  RTC_DCHECK(config_.rtp.remote_ssrc != config_.rtp.local_ssrc);
  rtp_rtcp_->SetSSRC(config_.rtp.local_ssrc);

  // TODO(pbos): Support multiple RTX, per video payload.
  for (const auto& kv : config_.rtp.rtx) {
    RTC_DCHECK(kv.second.ssrc != 0);
    RTC_DCHECK(kv.second.payload_type != 0);

    vie_receiver_->SetRtxSsrc(kv.second.ssrc);
    vie_receiver_->SetRtxPayloadType(kv.second.payload_type, kv.first);
  }
  // TODO(holmer): When Chrome no longer depends on this being false by default,
  // always use the mapping and remove this whole codepath.
  vie_receiver_->SetUseRtxPayloadMappingOnRestore(
      config_.rtp.use_rtx_payload_mapping_on_restore);

  congestion_controller_->SetChannelRembStatus(false, config_.rtp.remb,
                                               rtp_rtcp_);

  for (size_t i = 0; i < config_.rtp.extensions.size(); ++i) {
    const std::string& extension = config_.rtp.extensions[i].name;
    int id = config_.rtp.extensions[i].id;
    // One-byte-extension local identifiers are in the range 1-14 inclusive.
    RTC_DCHECK_GE(id, 1);
    RTC_DCHECK_LE(id, 14);
    if (extension == RtpExtension::kTOffset) {
      RTC_CHECK(vie_receiver_->SetReceiveTimestampOffsetStatus(true, id));
    } else if (extension == RtpExtension::kAbsSendTime) {
      RTC_CHECK(vie_receiver_->SetReceiveAbsoluteSendTimeStatus(true, id));
    } else if (extension == RtpExtension::kVideoRotation) {
      RTC_CHECK(vie_receiver_->SetReceiveVideoRotationStatus(true, id));
    } else if (extension == RtpExtension::kTransportSequenceNumber) {
      RTC_CHECK(vie_receiver_->SetReceiveTransportSequenceNumber(true, id));
    } else {
      RTC_NOTREACHED() << "Unsupported RTP extension.";
    }
  }

  if (config_.rtp.fec.ulpfec_payload_type != -1) {
    // ULPFEC without RED doesn't make sense.
    RTC_DCHECK(config_.rtp.fec.red_payload_type != -1);
    VideoCodec codec;
    memset(&codec, 0, sizeof(codec));
    codec.codecType = kVideoCodecULPFEC;
    strncpy(codec.plName, "ulpfec", sizeof(codec.plName));
    codec.plType = config_.rtp.fec.ulpfec_payload_type;
    RTC_CHECK(vie_receiver_->SetReceiveCodec(codec));
  }
  if (config_.rtp.fec.red_payload_type != -1) {
    VideoCodec codec;
    memset(&codec, 0, sizeof(codec));
    codec.codecType = kVideoCodecRED;
    strncpy(codec.plName, "red", sizeof(codec.plName));
    codec.plType = config_.rtp.fec.red_payload_type;
    RTC_CHECK(vie_receiver_->SetReceiveCodec(codec));
    if (config_.rtp.fec.red_rtx_payload_type != -1) {
      vie_receiver_->SetRtxPayloadType(config_.rtp.fec.red_rtx_payload_type,
                                       config_.rtp.fec.red_payload_type);
    }
  }

  if (config.rtp.rtcp_xr.receiver_reference_time_report)
    rtp_rtcp_->SetRtcpXrRrtrStatus(true);

  vie_channel_.RegisterReceiveStatisticsProxy(&stats_proxy_);
  vie_receiver_->GetReceiveStatistics()->RegisterRtpStatisticsCallback(
      &stats_proxy_);
  vie_receiver_->GetReceiveStatistics()->RegisterRtcpStatisticsCallback(
      &stats_proxy_);
  // Stats callback for CNAME changes.
  rtp_rtcp_->RegisterRtcpStatisticsCallback(&stats_proxy_);
  vie_channel_.RegisterRtcpPacketTypeCounterObserver(&stats_proxy_);

  RTC_DCHECK(!config_.decoders.empty());
  std::set<int> decoder_payload_types;
  for (size_t i = 0; i < config_.decoders.size(); ++i) {
    const Decoder& decoder = config_.decoders[i];
    RTC_CHECK(decoder.decoder);
    RTC_CHECK(decoder_payload_types.find(decoder.payload_type) ==
              decoder_payload_types.end())
        << "Duplicate payload type (" << decoder.payload_type
        << ") for different decoders.";
    decoder_payload_types.insert(decoder.payload_type);
    vcm_->RegisterExternalDecoder(decoder.decoder, decoder.payload_type);

    VideoCodec codec = CreateDecoderVideoCodec(decoder);

    RTC_CHECK(vie_receiver_->SetReceiveCodec(codec));
    RTC_CHECK_EQ(VCM_OK,
                 vcm_->RegisterReceiveCodec(&codec, num_cpu_cores, false));
  }

  vcm_->SetRenderDelay(config.render_delay_ms);
  incoming_video_stream_.SetExpectedRenderDelay(config.render_delay_ms);
  vcm_->RegisterPreDecodeImageCallback(this);
  incoming_video_stream_.SetExternalCallback(this);
  vie_channel_.SetIncomingVideoStream(&incoming_video_stream_);
  vie_channel_.RegisterPreRenderCallback(this);

  process_thread_->RegisterModule(vcm_.get());
}

VideoReceiveStream::~VideoReceiveStream() {
  LOG(LS_INFO) << "~VideoReceiveStream: " << config_.ToString();
  incoming_video_stream_.Stop();
  process_thread_->DeRegisterModule(vcm_.get());
  vie_channel_.RegisterPreRenderCallback(nullptr);
  vcm_->RegisterPreDecodeImageCallback(nullptr);

  call_stats_->DeregisterStatsObserver(vie_channel_.GetStatsObserver());
  congestion_controller_->SetChannelRembStatus(false, false, rtp_rtcp_);

  congestion_controller_->GetRemoteBitrateEstimator(UseSendSideBwe(config_))
      ->RemoveStream(vie_receiver_->GetRemoteSsrc());
}

void VideoReceiveStream::Start() {
  transport_adapter_.Enable();
  incoming_video_stream_.Start();
  vie_channel_.StartReceive();
}

void VideoReceiveStream::Stop() {
  incoming_video_stream_.Stop();
  vie_channel_.StopReceive();
  transport_adapter_.Disable();
}

void VideoReceiveStream::SetSyncChannel(VoiceEngine* voice_engine,
                                        int audio_channel_id) {
  if (voice_engine != nullptr && audio_channel_id != -1) {
    VoEVideoSync* voe_sync_interface = VoEVideoSync::GetInterface(voice_engine);
    vie_channel_.SetVoiceChannel(audio_channel_id, voe_sync_interface);
    voe_sync_interface->Release();
  } else {
    vie_channel_.SetVoiceChannel(-1, nullptr);
  }
}

VideoReceiveStream::Stats VideoReceiveStream::GetStats() const {
  return stats_proxy_.GetStats();
}

bool VideoReceiveStream::DeliverRtcp(const uint8_t* packet, size_t length) {
  return vie_receiver_->DeliverRtcp(packet, length);
}

bool VideoReceiveStream::DeliverRtp(const uint8_t* packet,
                                    size_t length,
                                    const PacketTime& packet_time) {
  return vie_receiver_->DeliverRtp(packet, length, packet_time);
}

void VideoReceiveStream::FrameCallback(VideoFrame* video_frame) {
  stats_proxy_.OnDecodedFrame();

  // Post processing is not supported if the frame is backed by a texture.
  if (video_frame->native_handle() == NULL) {
    if (config_.pre_render_callback)
      config_.pre_render_callback->FrameCallback(video_frame);
  }
}

int VideoReceiveStream::RenderFrame(const uint32_t /*stream_id*/,
                                    const VideoFrame& video_frame) {
  // TODO(pbos): Wire up config_.render->IsTextureSupported() and convert if not
  // supported. Or provide methods for converting a texture frame in
  // VideoFrame.

  if (config_.renderer != nullptr)
    config_.renderer->RenderFrame(
        video_frame,
        video_frame.render_time_ms() - clock_->TimeInMilliseconds());

  stats_proxy_.OnRenderedFrame(video_frame.width(), video_frame.height());

  return 0;
}

// TODO(asapersson): Consider moving callback from video_encoder.h or
// creating a different callback.
int32_t VideoReceiveStream::Encoded(
    const EncodedImage& encoded_image,
    const CodecSpecificInfo* codec_specific_info,
    const RTPFragmentationHeader* fragmentation) {
  stats_proxy_.OnPreDecode(encoded_image, codec_specific_info);
  if (config_.pre_decode_callback) {
    // TODO(asapersson): Remove EncodedFrameCallbackAdapter.
    encoded_frame_proxy_.Encoded(
        encoded_image, codec_specific_info, fragmentation);
  }
  return 0;
}

void VideoReceiveStream::SignalNetworkState(NetworkState state) {
  rtp_rtcp_->SetRTCPStatus(state == kNetworkUp ? config_.rtp.rtcp_mode
                                               : RtcpMode::kOff);
}

}  // namespace internal
}  // namespace webrtc
