#include "http2/adapter/callback_visitor.h"

#include "http2/adapter/nghttp2_util.h"
#include "third_party/nghttp2/src/lib/includes/nghttp2/nghttp2.h"
#include "common/quiche_endian.h"

// This visitor implementation needs visibility into the
// nghttp2_session_callbacks type. There's no public header, so we'll redefine
// the struct here.
struct nghttp2_session_callbacks {
  nghttp2_send_callback send_callback;
  nghttp2_recv_callback recv_callback;
  nghttp2_on_frame_recv_callback on_frame_recv_callback;
  nghttp2_on_invalid_frame_recv_callback on_invalid_frame_recv_callback;
  nghttp2_on_data_chunk_recv_callback on_data_chunk_recv_callback;
  nghttp2_before_frame_send_callback before_frame_send_callback;
  nghttp2_on_frame_send_callback on_frame_send_callback;
  nghttp2_on_frame_not_send_callback on_frame_not_send_callback;
  nghttp2_on_stream_close_callback on_stream_close_callback;
  nghttp2_on_begin_headers_callback on_begin_headers_callback;
  nghttp2_on_header_callback on_header_callback;
  nghttp2_on_header_callback2 on_header_callback2;
  nghttp2_on_invalid_header_callback on_invalid_header_callback;
  nghttp2_on_invalid_header_callback2 on_invalid_header_callback2;
  nghttp2_select_padding_callback select_padding_callback;
  nghttp2_data_source_read_length_callback read_length_callback;
  nghttp2_on_begin_frame_callback on_begin_frame_callback;
  nghttp2_send_data_callback send_data_callback;
  nghttp2_pack_extension_callback pack_extension_callback;
  nghttp2_unpack_extension_callback unpack_extension_callback;
  nghttp2_on_extension_chunk_recv_callback on_extension_chunk_recv_callback;
  nghttp2_error_callback error_callback;
  nghttp2_error_callback2 error_callback2;
};

namespace http2 {
namespace adapter {

CallbackVisitor::CallbackVisitor(Perspective perspective,
                                 const nghttp2_session_callbacks& callbacks,
                                 void* user_data)
    : perspective_(perspective),
      callbacks_(MakeCallbacksPtr(nullptr)),
      user_data_(user_data) {
  nghttp2_session_callbacks* c;
  nghttp2_session_callbacks_new(&c);
  *c = callbacks;
  callbacks_ = MakeCallbacksPtr(c);
}

ssize_t CallbackVisitor::OnReadyToSend(absl::string_view serialized) {
  return callbacks_->send_callback(nullptr, ToUint8Ptr(serialized.data()),
                                   serialized.size(), 0, user_data_);
}

void CallbackVisitor::OnConnectionError() {
  QUICHE_LOG(FATAL) << "Not implemented";
}

void CallbackVisitor::OnFrameHeader(Http2StreamId stream_id,
                                    size_t length,
                                    uint8_t type,
                                    uint8_t flags) {
  // The general strategy is to clear |current_frame_| at the start of a new
  // frame, accumulate frame information from the various callback events, then
  // invoke the on_frame_recv_callback() with the accumulated frame data.
  memset(&current_frame_, 0, sizeof(current_frame_));
  current_frame_.hd.stream_id = stream_id;
  current_frame_.hd.length = length;
  current_frame_.hd.type = type;
  current_frame_.hd.flags = flags;
  callbacks_->on_begin_frame_callback(nullptr, &current_frame_.hd, user_data_);
}

void CallbackVisitor::OnSettingsStart() {}

void CallbackVisitor::OnSetting(Http2Setting setting) {
  settings_.push_back({.settings_id = setting.id, .value = setting.value});
}

void CallbackVisitor::OnSettingsEnd() {
  current_frame_.settings.niv = settings_.size();
  current_frame_.settings.iv = settings_.data();
  callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
  settings_.clear();
}

void CallbackVisitor::OnSettingsAck() {
  // ACK is part of the flags, which were set in OnFrameHeader().
  callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
}

void CallbackVisitor::OnBeginHeadersForStream(Http2StreamId stream_id) {
  auto it = GetStreamInfo(stream_id);
  if (it->second->received_headers) {
    // At least one headers frame has already been received.
    current_frame_.headers.cat = NGHTTP2_HCAT_HEADERS;
  } else {
    switch (perspective_) {
      case Perspective::kClient:
        current_frame_.headers.cat = NGHTTP2_HCAT_RESPONSE;
        break;
      case Perspective::kServer:
        current_frame_.headers.cat = NGHTTP2_HCAT_REQUEST;
        break;
    }
  }
  callbacks_->on_begin_headers_callback(nullptr, &current_frame_, user_data_);
  it->second->received_headers = true;
}

Http2VisitorInterface::OnHeaderResult CallbackVisitor::OnHeaderForStream(
    Http2StreamId stream_id, absl::string_view name, absl::string_view value) {
  if (callbacks_->on_header_callback) {
    const int result = callbacks_->on_header_callback(
        nullptr, &current_frame_, ToUint8Ptr(name.data()), name.size(),
        ToUint8Ptr(value.data()), value.size(), NGHTTP2_NV_FLAG_NONE,
        user_data_);
    if (result == 0) {
      return HEADER_OK;
    } else if (result == NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE) {
      return HEADER_RST_STREAM;
    } else {
      // Assume NGHTTP2_ERR_CALLBACK_FAILURE.
      return HEADER_CONNECTION_ERROR;
    }
  }
  return HEADER_OK;
}

void CallbackVisitor::OnEndHeadersForStream(Http2StreamId stream_id) {
  callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
}

void CallbackVisitor::OnBeginDataForStream(Http2StreamId stream_id,
                                           size_t payload_length) {
  // TODO(b/181586191): Interpret padding, subtract padding from
  // |remaining_data_|.
  remaining_data_ = payload_length;
  if (remaining_data_ == 0) {
    callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
  }
}

void CallbackVisitor::OnDataForStream(Http2StreamId stream_id,
                                      absl::string_view data) {
  callbacks_->on_data_chunk_recv_callback(nullptr, current_frame_.hd.flags,
                                          stream_id, ToUint8Ptr(data.data()),
                                          data.size(), user_data_);
  remaining_data_ -= data.size();
  if (remaining_data_ == 0) {
    callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
  }
}

void CallbackVisitor::OnEndStream(Http2StreamId stream_id) {}

void CallbackVisitor::OnRstStream(Http2StreamId stream_id,
                                  Http2ErrorCode error_code) {
  current_frame_.rst_stream.error_code = static_cast<uint32_t>(error_code);
  callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
}

void CallbackVisitor::OnCloseStream(Http2StreamId stream_id,
                                    Http2ErrorCode error_code) {
  callbacks_->on_stream_close_callback(
      nullptr, stream_id, static_cast<uint32_t>(error_code), user_data_);
}

void CallbackVisitor::OnPriorityForStream(Http2StreamId stream_id,
                                          Http2StreamId parent_stream_id,
                                          int weight,
                                          bool exclusive) {
  current_frame_.priority.pri_spec.stream_id = parent_stream_id;
  current_frame_.priority.pri_spec.weight = weight;
  current_frame_.priority.pri_spec.exclusive = exclusive;
  callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
}

void CallbackVisitor::OnPing(Http2PingId ping_id, bool is_ack) {
  uint64_t network_order_opaque_data =
      quiche::QuicheEndian::HostToNet64(ping_id);
  std::memcpy(current_frame_.ping.opaque_data, &network_order_opaque_data,
              sizeof(network_order_opaque_data));
  callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
}

void CallbackVisitor::OnPushPromiseForStream(Http2StreamId stream_id,
                                             Http2StreamId promised_stream_id) {
  QUICHE_LOG(FATAL) << "Not implemented";
}

void CallbackVisitor::OnGoAway(Http2StreamId last_accepted_stream_id,
                               Http2ErrorCode error_code,
                               absl::string_view opaque_data) {
  current_frame_.goaway.last_stream_id = last_accepted_stream_id;
  current_frame_.goaway.error_code = static_cast<uint32_t>(error_code);
  current_frame_.goaway.opaque_data = ToUint8Ptr(opaque_data.data());
  current_frame_.goaway.opaque_data_len = opaque_data.size();
  callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
}

void CallbackVisitor::OnWindowUpdate(Http2StreamId stream_id,
                                     int window_increment) {
  current_frame_.window_update.window_size_increment = window_increment;
  if (callbacks_->on_frame_recv_callback) {
    callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
  }
}

void CallbackVisitor::PopulateFrame(nghttp2_frame& frame, uint8_t frame_type,
                                    Http2StreamId stream_id, size_t length,
                                    uint8_t flags, uint32_t error_code,
                                    bool sent_headers) {
  frame.hd.type = frame_type;
  frame.hd.stream_id = stream_id;
  frame.hd.length = length;
  frame.hd.flags = flags;
  const FrameType frame_type_enum = static_cast<FrameType>(frame_type);
  if (frame_type_enum == FrameType::HEADERS) {
    if (sent_headers) {
      frame.headers.cat = NGHTTP2_HCAT_HEADERS;
    } else {
      switch (perspective_) {
        case Perspective::kClient:
          QUICHE_LOG(INFO) << "First headers sent by the client for stream "
                           << stream_id << "; these are request headers";
          frame.headers.cat = NGHTTP2_HCAT_REQUEST;
          break;
        case Perspective::kServer:
          QUICHE_LOG(INFO) << "First headers sent by the server for stream "
                           << stream_id << "; these are response headers";
          frame.headers.cat = NGHTTP2_HCAT_RESPONSE;
          break;
      }
    }
  } else if (frame_type_enum == FrameType::RST_STREAM) {
    frame.rst_stream.error_code = error_code;
  } else if (frame_type_enum == FrameType::GOAWAY) {
    frame.goaway.error_code = error_code;
  }
}

int CallbackVisitor::OnBeforeFrameSent(uint8_t frame_type,
                                       Http2StreamId stream_id, size_t length,
                                       uint8_t flags) {
  if (callbacks_->before_frame_send_callback) {
    QUICHE_LOG(INFO) << "OnBeforeFrameSent(type=" << int(frame_type)
                     << ", stream_id=" << stream_id << ", length=" << length
                     << ", flags=" << int(flags) << ")";
    nghttp2_frame frame;
    auto it = GetStreamInfo(stream_id);
    // The implementation of the before_frame_send_callback doesn't look at the
    // error code, so for now it's populated with 0.
    PopulateFrame(frame, frame_type, stream_id, length, flags, /*error_code=*/0,
                  it->second->before_sent_headers);
    it->second->before_sent_headers = true;
    return callbacks_->before_frame_send_callback(nullptr, &frame, user_data_);
  }
  return 0;
}

int CallbackVisitor::OnFrameSent(uint8_t frame_type, Http2StreamId stream_id,
                                 size_t length, uint8_t flags,
                                 uint32_t error_code) {
  if (callbacks_->on_frame_send_callback) {
    QUICHE_LOG(INFO) << "OnFrameSent(type=" << int(frame_type)
                     << ", stream_id=" << stream_id << ", length=" << length
                     << ", flags=" << int(flags)
                     << ", error_code=" << error_code << ")";
    nghttp2_frame frame;
    auto it = GetStreamInfo(stream_id);
    PopulateFrame(frame, frame_type, stream_id, length, flags, error_code,
                  it->second->sent_headers);
    it->second->sent_headers = true;
    return callbacks_->on_frame_send_callback(nullptr, &frame, user_data_);
  }
  return 0;
}

void CallbackVisitor::OnReadyToSendDataForStream(Http2StreamId stream_id,
                                                 char* destination_buffer,
                                                 size_t length,
                                                 ssize_t* written,
                                                 bool* end_stream) {
  QUICHE_LOG(FATAL) << "Not implemented";
}

bool CallbackVisitor::OnInvalidFrame(Http2StreamId stream_id, int error_code) {
  QUICHE_LOG(INFO) << "OnInvalidFrame(" << stream_id << ", " << error_code
                   << ")";
  QUICHE_DCHECK_EQ(stream_id, current_frame_.hd.stream_id);
  if (callbacks_->on_invalid_frame_recv_callback) {
    return 0 == callbacks_->on_invalid_frame_recv_callback(
                    nullptr, &current_frame_, error_code, user_data_);
  }
  return true;
}

void CallbackVisitor::OnReadyToSendMetadataForStream(Http2StreamId stream_id,
                                                     char* buffer,
                                                     size_t length,
                                                     ssize_t* written) {
  QUICHE_LOG(FATAL) << "Not implemented";
}

void CallbackVisitor::OnBeginMetadataForStream(Http2StreamId stream_id,
                                               size_t payload_length) {
  QUICHE_VLOG(1) << "OnBeginMetadataForStream(stream_id=" << stream_id
                 << ", payload_length=" << payload_length << ")";
}

void CallbackVisitor::OnMetadataForStream(Http2StreamId stream_id,
                                          absl::string_view metadata) {
  QUICHE_VLOG(1) << "OnMetadataForStream(stream_id=" << stream_id
                 << ", len=" << metadata.size() << ")";
  if (callbacks_->on_extension_chunk_recv_callback) {
    int result = callbacks_->on_extension_chunk_recv_callback(
        nullptr, &current_frame_.hd, ToUint8Ptr(metadata.data()),
        metadata.size(), user_data_);
    QUICHE_DCHECK_EQ(0, result);
  }
}

bool CallbackVisitor::OnMetadataEndForStream(Http2StreamId stream_id) {
  QUICHE_LOG_IF(DFATAL, current_frame_.hd.flags != kMetadataEndFlag);
  QUICHE_VLOG(1) << "OnMetadataEndForStream(stream_id=" << stream_id << ")";
  if (callbacks_->unpack_extension_callback) {
    void* payload;
    int result = callbacks_->unpack_extension_callback(
        nullptr, &payload, &current_frame_.hd, user_data_);
    if (callbacks_->on_frame_recv_callback) {
      current_frame_.ext.payload = payload;
      callbacks_->on_frame_recv_callback(nullptr, &current_frame_, user_data_);
    }
    return (result == 0);
  }
  return true;
}

void CallbackVisitor::OnErrorDebug(absl::string_view message) {
  if (callbacks_->error_callback2) {
    callbacks_->error_callback2(nullptr, -1, message.data(), message.size(),
                                user_data_);
  }
}

CallbackVisitor::StreamInfoMap::iterator CallbackVisitor::GetStreamInfo(
    Http2StreamId stream_id) {
  auto it = stream_map_.find(stream_id);
  if (it == stream_map_.end()) {
    auto p = stream_map_.insert({stream_id, absl::make_unique<StreamInfo>()});
    it = p.first;
  }
  return it;
}

}  // namespace adapter
}  // namespace http2