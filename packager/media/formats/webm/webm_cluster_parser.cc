// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "packager/media/formats/webm/webm_cluster_parser.h"

#include <vector>

#include "packager/base/logging.h"
#include "packager/base/sys_byteorder.h"
#include "packager/media/base/decrypt_config.h"
#include "packager/media/base/timestamp.h"
#include "packager/media/filters/vp8_parser.h"
#include "packager/media/filters/vp9_parser.h"
#include "packager/media/filters/webvtt_util.h"
#include "packager/media/formats/webm/webm_constants.h"
#include "packager/media/formats/webm/webm_crypto_helpers.h"
#include "packager/media/formats/webm/webm_webvtt_parser.h"

namespace edash_packager {
namespace media {
namespace {

const int64_t kMicrosecondsPerMillisecond = 1000;

// Helper function used to inspect block data to determine if the
// block is a keyframe.
// |data| contains the bytes in the block.
// |size| indicates the number of bytes in |data|.
bool IsKeyframe(bool is_video,
                VideoCodec codec,
                const uint8_t* data,
                int size) {
  // For now, assume that all blocks are keyframes for datatypes other than
  // video. This is a valid assumption for Vorbis, WebVTT, & Opus.
  if (!is_video)
    return true;

  switch (codec) {
    case kCodecVP8:
      return VP8Parser::IsKeyframe(data, size);
    case kCodecVP9:
      return VP9Parser::IsKeyframe(data, size);
    default:
      NOTIMPLEMENTED() << "Unsupported codec " << codec;
      return false;
  }
}

}  // namespace

WebMClusterParser::WebMClusterParser(
    int64_t timecode_scale,
    scoped_refptr<AudioStreamInfo> audio_stream_info,
    scoped_refptr<VideoStreamInfo> video_stream_info,
    int64_t audio_default_duration,
    int64_t video_default_duration,
    const WebMTracksParser::TextTracks& text_tracks,
    const std::set<int64_t>& ignored_tracks,
    const std::string& audio_encryption_key_id,
    const std::string& video_encryption_key_id,
    const MediaParser::NewSampleCB& new_sample_cb,
    const MediaParser::InitCB& init_cb,
    KeySource* decryption_key_source)
    : timecode_multiplier_(timecode_scale / 1000.0),
      audio_stream_info_(audio_stream_info),
      video_stream_info_(video_stream_info),
      ignored_tracks_(ignored_tracks),
      audio_encryption_key_id_(audio_encryption_key_id),
      video_encryption_key_id_(video_encryption_key_id),
      parser_(kWebMIdCluster, this),
      initialized_(false),
      init_cb_(init_cb),
      cluster_start_time_(kNoTimestamp),
      audio_(audio_stream_info ? audio_stream_info->track_id() : -1,
             false,
             audio_default_duration,
             new_sample_cb),
      video_(video_stream_info ? video_stream_info->track_id() : -1,
             true,
             video_default_duration,
             new_sample_cb) {
  if (decryption_key_source)
    decryptor_source_.reset(new DecryptorSource(decryption_key_source));
  for (WebMTracksParser::TextTracks::const_iterator it = text_tracks.begin();
       it != text_tracks.end();
       ++it) {
    text_track_map_.insert(std::make_pair(
        it->first, Track(it->first, false, kNoTimestamp, new_sample_cb)));
  }
}

WebMClusterParser::~WebMClusterParser() {}

void WebMClusterParser::Reset() {
  last_block_timecode_ = -1;
  cluster_timecode_ = -1;
  cluster_start_time_ = kNoTimestamp;
  cluster_ended_ = false;
  parser_.Reset();
  audio_.Reset();
  video_.Reset();
  ResetTextTracks();
}

void WebMClusterParser::Flush() {
  // Estimate the duration of the last frame if necessary.
  audio_.ApplyDurationEstimateIfNeeded();
  video_.ApplyDurationEstimateIfNeeded();
  Reset();
}

int WebMClusterParser::Parse(const uint8_t* buf, int size) {
  int result = parser_.Parse(buf, size);

  if (result < 0) {
    cluster_ended_ = false;
    return result;
  }

  cluster_ended_ = parser_.IsParsingComplete();
  if (cluster_ended_) {
    // If there were no buffers in this cluster, set the cluster start time to
    // be the |cluster_timecode_|.
    if (cluster_start_time_ == kNoTimestamp) {
      // If the cluster did not even have a |cluster_timecode_|, signal parse
      // error.
      if (cluster_timecode_ < 0)
        return -1;

      cluster_start_time_ = cluster_timecode_ * timecode_multiplier_;
    }

    // Reset the parser if we're done parsing so that
    // it is ready to accept another cluster on the next
    // call.
    parser_.Reset();

    last_block_timecode_ = -1;
    cluster_timecode_ = -1;
  }

  return result;
}

WebMParserClient* WebMClusterParser::OnListStart(int id) {
  if (id == kWebMIdCluster) {
    cluster_timecode_ = -1;
    cluster_start_time_ = kNoTimestamp;
  } else if (id == kWebMIdBlockGroup) {
    block_data_.reset();
    block_data_size_ = -1;
    block_duration_ = -1;
    discard_padding_ = -1;
    discard_padding_set_ = false;
  } else if (id == kWebMIdBlockAdditions) {
    block_add_id_ = -1;
    block_additional_data_.reset();
    block_additional_data_size_ = 0;
  }

  return this;
}

bool WebMClusterParser::OnListEnd(int id) {
  if (id != kWebMIdBlockGroup)
    return true;

  // Make sure the BlockGroup actually had a Block.
  if (block_data_size_ == -1) {
    LOG(ERROR) << "Block missing from BlockGroup.";
    return false;
  }

  bool result = ParseBlock(false, block_data_.get(), block_data_size_,
                           block_additional_data_.get(),
                           block_additional_data_size_, block_duration_,
                           discard_padding_set_ ? discard_padding_ : 0);
  block_data_.reset();
  block_data_size_ = -1;
  block_duration_ = -1;
  block_add_id_ = -1;
  block_additional_data_.reset();
  block_additional_data_size_ = 0;
  discard_padding_ = -1;
  discard_padding_set_ = false;
  return result;
}

bool WebMClusterParser::OnUInt(int id, int64_t val) {
  int64_t* dst;
  switch (id) {
    case kWebMIdTimecode:
      dst = &cluster_timecode_;
      break;
    case kWebMIdBlockDuration:
      dst = &block_duration_;
      break;
    case kWebMIdBlockAddID:
      dst = &block_add_id_;
      break;
    default:
      return true;
  }
  if (*dst != -1)
    return false;
  *dst = val;
  return true;
}

bool WebMClusterParser::ParseBlock(bool is_simple_block,
                                   const uint8_t* buf,
                                   int size,
                                   const uint8_t* additional,
                                   int additional_size,
                                   int duration,
                                   int64_t discard_padding) {
  if (size < 4)
    return false;

  // Return an error if the trackNum > 127. We just aren't
  // going to support large track numbers right now.
  if (!(buf[0] & 0x80)) {
    LOG(ERROR) << "TrackNumber over 127 not supported";
    return false;
  }

  int track_num = buf[0] & 0x7f;
  int timecode = buf[1] << 8 | buf[2];
  int flags = buf[3] & 0xff;
  int lacing = (flags >> 1) & 0x3;

  if (lacing) {
    LOG(ERROR) << "Lacing " << lacing << " is not supported yet.";
    return false;
  }

  // Sign extend negative timecode offsets.
  if (timecode & 0x8000)
    timecode |= ~0xffff;

  const uint8_t* frame_data = buf + 4;
  int frame_size = size - (frame_data - buf);
  return OnBlock(is_simple_block, track_num, timecode, duration, flags,
                 frame_data, frame_size, additional, additional_size,
                 discard_padding);
}

bool WebMClusterParser::OnBinary(int id, const uint8_t* data, int size) {
  switch (id) {
    case kWebMIdSimpleBlock:
      return ParseBlock(true, data, size, NULL, 0, -1, 0);

    case kWebMIdBlock:
      if (block_data_) {
        LOG(ERROR) << "More than 1 Block in a BlockGroup is not "
                      "supported.";
        return false;
      }
      block_data_.reset(new uint8_t[size]);
      memcpy(block_data_.get(), data, size);
      block_data_size_ = size;
      return true;

    case kWebMIdBlockAdditional: {
      uint64_t block_add_id = base::HostToNet64(block_add_id_);
      if (block_additional_data_) {
        // TODO: Technically, more than 1 BlockAdditional is allowed as per
        // matroska spec. But for now we don't have a use case to support
        // parsing of such files. Take a look at this again when such a case
        // arises.
        LOG(ERROR) << "More than 1 BlockAdditional in a "
                      "BlockGroup is not supported.";
        return false;
      }
      // First 8 bytes of side_data in DecoderBuffer is the BlockAddID
      // element's value in Big Endian format. This is done to mimic ffmpeg
      // demuxer's behavior.
      block_additional_data_size_ = size + sizeof(block_add_id);
      block_additional_data_.reset(new uint8_t[block_additional_data_size_]);
      memcpy(block_additional_data_.get(), &block_add_id,
             sizeof(block_add_id));
      memcpy(block_additional_data_.get() + 8, data, size);
      return true;
    }
    case kWebMIdDiscardPadding: {
      if (discard_padding_set_ || size <= 0 || size > 8)
        return false;
      discard_padding_set_ = true;

      // Read in the big-endian integer.
      discard_padding_ = static_cast<int8_t>(data[0]);
      for (int i = 1; i < size; ++i)
        discard_padding_ = (discard_padding_ << 8) | data[i];

      return true;
    }
    default:
      return true;
  }
}

bool WebMClusterParser::OnBlock(bool is_simple_block,
                                int track_num,
                                int timecode,
                                int block_duration,
                                int flags,
                                const uint8_t* data,
                                int size,
                                const uint8_t* additional,
                                int additional_size,
                                int64_t discard_padding) {
  DCHECK_GE(size, 0);
  if (cluster_timecode_ == -1) {
    LOG(ERROR) << "Got a block before cluster timecode.";
    return false;
  }

  // TODO: Should relative negative timecode offsets be rejected?  Or only when
  // the absolute timecode is negative?  See http://crbug.com/271794
  if (timecode < 0) {
    LOG(ERROR) << "Got a block with negative timecode offset " << timecode;
    return false;
  }

  if (last_block_timecode_ != -1 && timecode < last_block_timecode_) {
    LOG(ERROR) << "Got a block with a timecode before the previous block.";
    return false;
  }

  Track* track = NULL;
  StreamType stream_type = kStreamUnknown;
  std::string encryption_key_id;
  if (track_num == audio_.track_num()) {
    track = &audio_;
    encryption_key_id = audio_encryption_key_id_;
    stream_type = kStreamAudio;
  } else if (track_num == video_.track_num()) {
    track = &video_;
    encryption_key_id = video_encryption_key_id_;
    stream_type = kStreamVideo;
  } else if (ignored_tracks_.find(track_num) != ignored_tracks_.end()) {
    return true;
  } else if (Track* const text_track = FindTextTrack(track_num)) {
    if (is_simple_block)  // BlockGroup is required for WebVTT cues
      return false;
    if (block_duration < 0)  // not specified
      return false;
    track = text_track;
    stream_type = kStreamText;
  } else {
    LOG(ERROR) << "Unexpected track number " << track_num;
    return false;
  }
  DCHECK_NE(stream_type, kStreamUnknown);

  last_block_timecode_ = timecode;

  int64_t timestamp = (cluster_timecode_ + timecode) * timecode_multiplier_;

  scoped_refptr<MediaSample> buffer;
  if (stream_type != kStreamText) {
    // The first bit of the flags is set when a SimpleBlock contains only
    // keyframes. If this is a Block, then inspection of the payload is
    // necessary to determine whether it contains a keyframe or not.
    // http://www.matroska.org/technical/specs/index.html
    bool is_keyframe =
        is_simple_block
            ? (flags & 0x80) != 0
            : IsKeyframe(stream_type == kStreamVideo,
                         video_stream_info_ ? video_stream_info_->codec()
                                            : kUnknownVideoCodec,
                         data, size);

    // Every encrypted Block has a signal byte and IV prepended to it. Current
    // encrypted WebM request for comments specification is here
    // http://wiki.webmproject.org/encryption/webm-encryption-rfc
    scoped_ptr<DecryptConfig> decrypt_config;
    int data_offset = 0;
    if (!encryption_key_id.empty() &&
        !WebMCreateDecryptConfig(
             data, size,
             reinterpret_cast<const uint8_t*>(encryption_key_id.data()),
             encryption_key_id.size(),
             &decrypt_config, &data_offset)) {
      return false;
    }

    buffer = MediaSample::CopyFrom(data + data_offset, size - data_offset,
                                   additional, additional_size, is_keyframe);

    // An empty iv indicates that this sample is not encrypted.
    if (decrypt_config && !decrypt_config->iv().empty()) {
      if (!decryptor_source_) {
        LOG(ERROR) << "Encrypted media sample encountered, but decryption is "
                      "not enabled";
        return false;
      }
      if (!decryptor_source_->DecryptSampleBuffer(decrypt_config.get(),
                                                  buffer->writable_data(),
                                                  buffer->data_size())) {
        LOG(ERROR) << "Cannot decrypt samples";
        return false;
      }
    }
  } else {
    std::string id, settings, content;
    WebMWebVTTParser::Parse(data, size, &id, &settings, &content);

    std::vector<uint8_t> side_data;
    MakeSideData(id.begin(), id.end(),
                 settings.begin(), settings.end(),
                 &side_data);

    buffer = MediaSample::CopyFrom(
        reinterpret_cast<const uint8_t*>(content.data()), content.length(),
        &side_data[0], side_data.size(), true);
  }

  buffer->set_dts(timestamp);
  buffer->set_pts(timestamp);
  if (cluster_start_time_ == kNoTimestamp)
    cluster_start_time_ = timestamp;
  buffer->set_duration(block_duration > 0
                           ? (block_duration * timecode_multiplier_)
                           : kNoTimestamp);

  if (!init_cb_.is_null() && !initialized_) {
    std::vector<scoped_refptr<StreamInfo>> streams;
    if (audio_stream_info_)
      streams.push_back(audio_stream_info_);
    if (video_stream_info_) {
      if (stream_type == kStreamVideo) {
        scoped_ptr<VPxParser> vpx_parser;
        switch (video_stream_info_->codec()) {
          case kCodecVP8:
            vpx_parser.reset(new VP8Parser);
            break;
          case kCodecVP9:
            vpx_parser.reset(new VP9Parser);
            break;
          default:
            NOTIMPLEMENTED() << "Unsupported codec "
                             << video_stream_info_->codec();
            return false;
        }
        std::vector<VPxFrameInfo> vpx_frames;
        if (!vpx_parser->Parse(buffer->data(), buffer->data_size(),
                               &vpx_frames)) {
          LOG(ERROR) << "Failed to parse vpx frame.";
          return false;
        }
        if (vpx_frames.size() != 1u || !vpx_frames[0].is_keyframe) {
          LOG(ERROR) << "The first frame should be a key frame.";
          return false;
        }

        const VPCodecConfiguration* codec_config = &vpx_parser->codec_config();
        video_stream_info_->set_codec_string(
            codec_config->GetCodecString(video_stream_info_->codec()));
        std::vector<uint8_t> extra_data;
        codec_config->Write(&extra_data);
        video_stream_info_->set_extra_data(extra_data);
        streams.push_back(video_stream_info_);
        init_cb_.Run(streams);
        initialized_ = true;
      }
    } else {
      init_cb_.Run(streams);
      initialized_ = true;
    }
  }

  return track->EmitBuffer(buffer);
}

WebMClusterParser::Track::Track(int track_num,
                                bool is_video,
                                int64_t default_duration,
                                const MediaParser::NewSampleCB& new_sample_cb)
    : track_num_(track_num),
      is_video_(is_video),
      default_duration_(default_duration),
      estimated_next_frame_duration_(kNoTimestamp),
      new_sample_cb_(new_sample_cb) {
  DCHECK(default_duration_ == kNoTimestamp || default_duration_ > 0);
}

WebMClusterParser::Track::~Track() {}

bool WebMClusterParser::Track::EmitBuffer(
    const scoped_refptr<MediaSample>& buffer) {
  DVLOG(2) << "EmitBuffer() : " << track_num_
           << " ts " << buffer->pts()
           << " dur " << buffer->duration()
           << " kf " << buffer->is_key_frame()
           << " size " << buffer->data_size();

  if (last_added_buffer_missing_duration_.get()) {
    int64_t derived_duration =
        buffer->pts() - last_added_buffer_missing_duration_->pts();
    last_added_buffer_missing_duration_->set_duration(derived_duration);

    DVLOG(2) << "EmitBuffer() : applied derived duration to held-back buffer : "
             << " ts "
             << last_added_buffer_missing_duration_->pts()
             << " dur "
             << last_added_buffer_missing_duration_->duration()
             << " kf " << last_added_buffer_missing_duration_->is_key_frame()
             << " size " << last_added_buffer_missing_duration_->data_size();
    scoped_refptr<MediaSample> updated_buffer =
        last_added_buffer_missing_duration_;
    last_added_buffer_missing_duration_ = NULL;
    if (!EmitBufferHelp(updated_buffer))
      return false;
  }

  if (buffer->duration() == kNoTimestamp) {
    last_added_buffer_missing_duration_ = buffer;
    DVLOG(2) << "EmitBuffer() : holding back buffer that is missing duration";
    return true;
  }

  return EmitBufferHelp(buffer);
}

void WebMClusterParser::Track::ApplyDurationEstimateIfNeeded() {
  if (!last_added_buffer_missing_duration_.get())
    return;

  int64_t estimated_duration = GetDurationEstimate();
  last_added_buffer_missing_duration_->set_duration(estimated_duration);

  VLOG(1) << "Track " << track_num_ << ": Estimating WebM block duration to be "
          << estimated_duration / 1000
          << "ms for the last (Simple)Block in the Cluster for this Track. Use "
             "BlockGroups with BlockDurations at the end of each Track in a "
             "Cluster to avoid estimation.";

  DVLOG(2) << " new dur : ts " << last_added_buffer_missing_duration_->pts()
           << " dur " << last_added_buffer_missing_duration_->duration()
           << " kf " << last_added_buffer_missing_duration_->is_key_frame()
           << " size " << last_added_buffer_missing_duration_->data_size();

  // Don't use the applied duration as a future estimation (don't use
  // EmitBufferHelp() here.)
  new_sample_cb_.Run(track_num_, last_added_buffer_missing_duration_);
  last_added_buffer_missing_duration_ = NULL;
}

void WebMClusterParser::Track::Reset() {
  last_added_buffer_missing_duration_ = NULL;
}

bool WebMClusterParser::Track::EmitBufferHelp(
    const scoped_refptr<MediaSample>& buffer) {
  DCHECK(!last_added_buffer_missing_duration_.get());

  int64_t duration = buffer->duration();
  if (duration < 0 || duration == kNoTimestamp) {
    LOG(ERROR) << "Invalid buffer duration: " << duration;
    return false;
  }

  // The estimated frame duration is the maximum non-zero duration since the
  // last initialization segment.
  if (duration > 0) {
    int64_t orig_duration_estimate = estimated_next_frame_duration_;
    if (estimated_next_frame_duration_ == kNoTimestamp) {
      estimated_next_frame_duration_ = duration;
    } else {
      estimated_next_frame_duration_ =
          std::max(duration, estimated_next_frame_duration_);
    }

    if (orig_duration_estimate != estimated_next_frame_duration_) {
      DVLOG(3) << "Updated duration estimate:"
               << orig_duration_estimate
               << " -> "
               << estimated_next_frame_duration_
               << " at timestamp: "
               << buffer->dts();
    }
  }

  new_sample_cb_.Run(track_num_, buffer);
  return true;
}

int64_t WebMClusterParser::Track::GetDurationEstimate() {
  int64_t duration = kNoTimestamp;
  if (default_duration_ != kNoTimestamp) {
    duration = default_duration_;
    DVLOG(3) << __FUNCTION__ << " : using track default duration " << duration;
  } else if (estimated_next_frame_duration_ != kNoTimestamp) {
    duration = estimated_next_frame_duration_;
    DVLOG(3) << __FUNCTION__ << " : using estimated duration " << duration;
  } else {
    if (is_video_) {
      duration = kDefaultVideoBufferDurationInMs * kMicrosecondsPerMillisecond;
    } else {
      duration = kDefaultAudioBufferDurationInMs * kMicrosecondsPerMillisecond;
    }
    DVLOG(3) << __FUNCTION__ << " : using hardcoded default duration "
             << duration;
  }

  DCHECK_GT(duration, 0);
  DCHECK_NE(duration, kNoTimestamp);
  return duration;
}

void WebMClusterParser::ResetTextTracks() {
  for (TextTrackMap::iterator it = text_track_map_.begin();
       it != text_track_map_.end();
       ++it) {
    it->second.Reset();
  }
}

WebMClusterParser::Track*
WebMClusterParser::FindTextTrack(int track_num) {
  const TextTrackMap::iterator it = text_track_map_.find(track_num);

  if (it == text_track_map_.end())
    return NULL;

  return &it->second;
}

}  // namespace media
}  // namespace edash_packager
