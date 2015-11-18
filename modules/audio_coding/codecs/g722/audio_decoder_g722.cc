/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_coding/codecs/g722/audio_decoder_g722.h"

#include <string.h>

#include "webrtc/base/checks.h"
#include "webrtc/modules/audio_coding/codecs/g722/g722_interface.h"

namespace webrtc {

AudioDecoderG722::AudioDecoderG722() {
  WebRtcG722_CreateDecoder(&dec_state_);
  WebRtcG722_DecoderInit(dec_state_);
}

AudioDecoderG722::~AudioDecoderG722() {
  WebRtcG722_FreeDecoder(dec_state_);
}

bool AudioDecoderG722::HasDecodePlc() const {
  return false;
}

int AudioDecoderG722::DecodeInternal(const uint8_t* encoded,
                                     size_t encoded_len,
                                     int sample_rate_hz,
                                     int16_t* decoded,
                                     SpeechType* speech_type) {
  RTC_DCHECK_EQ(sample_rate_hz, 16000);
  int16_t temp_type = 1;  // Default is speech.
  size_t ret =
      WebRtcG722_Decode(dec_state_, encoded, encoded_len, decoded, &temp_type);
  *speech_type = ConvertSpeechType(temp_type);
  return static_cast<int>(ret);
}

void AudioDecoderG722::Reset() {
  WebRtcG722_DecoderInit(dec_state_);
}

int AudioDecoderG722::PacketDuration(const uint8_t* encoded,
                                     size_t encoded_len) const {
  // 1/2 encoded byte per sample per channel.
  return static_cast<int>(2 * encoded_len / Channels());
}

size_t AudioDecoderG722::Channels() const {
  return 1;
}

AudioDecoderG722Stereo::AudioDecoderG722Stereo() {
  WebRtcG722_CreateDecoder(&dec_state_left_);
  WebRtcG722_CreateDecoder(&dec_state_right_);
  WebRtcG722_DecoderInit(dec_state_left_);
  WebRtcG722_DecoderInit(dec_state_right_);
}

AudioDecoderG722Stereo::~AudioDecoderG722Stereo() {
  WebRtcG722_FreeDecoder(dec_state_left_);
  WebRtcG722_FreeDecoder(dec_state_right_);
}

int AudioDecoderG722Stereo::DecodeInternal(const uint8_t* encoded,
                                           size_t encoded_len,
                                           int sample_rate_hz,
                                           int16_t* decoded,
                                           SpeechType* speech_type) {
  RTC_DCHECK_EQ(sample_rate_hz, 16000);
  int16_t temp_type = 1;  // Default is speech.
  // De-interleave the bit-stream into two separate payloads.
  uint8_t* encoded_deinterleaved = new uint8_t[encoded_len];
  SplitStereoPacket(encoded, encoded_len, encoded_deinterleaved);
  // Decode left and right.
  size_t decoded_len = WebRtcG722_Decode(dec_state_left_, encoded_deinterleaved,
                                         encoded_len / 2, decoded, &temp_type);
  size_t ret = WebRtcG722_Decode(
      dec_state_right_, &encoded_deinterleaved[encoded_len / 2],
      encoded_len / 2, &decoded[decoded_len], &temp_type);
  if (ret == decoded_len) {
    ret += decoded_len;  // Return total number of samples.
    // Interleave output.
    for (size_t k = ret / 2; k < ret; k++) {
      int16_t temp = decoded[k];
      memmove(&decoded[2 * k - ret + 2], &decoded[2 * k - ret + 1],
              (ret - k - 1) * sizeof(int16_t));
      decoded[2 * k - ret + 1] = temp;
    }
  }
  *speech_type = ConvertSpeechType(temp_type);
  delete[] encoded_deinterleaved;
  return static_cast<int>(ret);
}

size_t AudioDecoderG722Stereo::Channels() const {
  return 2;
}

void AudioDecoderG722Stereo::Reset() {
  WebRtcG722_DecoderInit(dec_state_left_);
  WebRtcG722_DecoderInit(dec_state_right_);
}

// Split the stereo packet and place left and right channel after each other
// in the output array.
void AudioDecoderG722Stereo::SplitStereoPacket(const uint8_t* encoded,
                                               size_t encoded_len,
                                               uint8_t* encoded_deinterleaved) {
  // Regroup the 4 bits/sample so |l1 l2| |r1 r2| |l3 l4| |r3 r4| ...,
  // where "lx" is 4 bits representing left sample number x, and "rx" right
  // sample. Two samples fit in one byte, represented with |...|.
  for (size_t i = 0; i + 1 < encoded_len; i += 2) {
    uint8_t right_byte = ((encoded[i] & 0x0F) << 4) + (encoded[i + 1] & 0x0F);
    encoded_deinterleaved[i] = (encoded[i] & 0xF0) + (encoded[i + 1] >> 4);
    encoded_deinterleaved[i + 1] = right_byte;
  }

  // Move one byte representing right channel each loop, and place it at the
  // end of the bytestream vector. After looping the data is reordered to:
  // |l1 l2| |l3 l4| ... |l(N-1) lN| |r1 r2| |r3 r4| ... |r(N-1) r(N)|,
  // where N is the total number of samples.
  for (size_t i = 0; i < encoded_len / 2; i++) {
    uint8_t right_byte = encoded_deinterleaved[i + 1];
    memmove(&encoded_deinterleaved[i + 1], &encoded_deinterleaved[i + 2],
            encoded_len - i - 2);
    encoded_deinterleaved[encoded_len - 1] = right_byte;
  }
}

}  // namespace webrtc
