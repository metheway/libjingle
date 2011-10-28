/*
 * libjingle
 * Copyright 2004--2011, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#ifdef HAVE_WEBRTC_VIDEO

#include "talk/session/phone/webrtcvideoengine.h"

#include "talk/base/common.h"
#include "talk/base/buffer.h"
#include "talk/base/byteorder.h"
#include "talk/base/logging.h"
#include "talk/base/stringutils.h"
#include "talk/session/phone/videorenderer.h"
#include "talk/session/phone/webrtcpassthroughrender.h"
#include "talk/session/phone/webrtcvoiceengine.h"
#include "talk/session/phone/webrtcvideocapturer.h"
#include "talk/session/phone/webrtcvideoframe.h"
#include "talk/session/phone/webrtcvie.h"
#include "talk/session/phone/webrtcvoe.h"

namespace cricket {

static const int kDefaultLogSeverity = talk_base::LS_WARNING;

static const int kMinVideoBitrate = 300;
static const int kMaxVideoBitrate = 2000;

static const int kVideoRtpBufferSize = 65536;

static const int kRedPayloadType = 101;
static const int kFecPayloadType = 102;

static const int kDefaultNumberOfTemporalLayers = 3;

class WebRtcRenderAdapter : public webrtc::ExternalRenderer {
 public:
  explicit WebRtcRenderAdapter(VideoRenderer* renderer)
      : renderer_(renderer), width_(0), height_(0) {
  }
  virtual ~WebRtcRenderAdapter() {
  }

  void SetRenderer(VideoRenderer* renderer) {
    talk_base::CritScope cs(&crit_);
    renderer_ = renderer;
  }
  // Implementation of webrtc::ExternalRenderer.
  virtual int FrameSizeChange(unsigned int width, unsigned int height,
                              unsigned int /*number_of_streams*/) {
    talk_base::CritScope cs(&crit_);
    if (renderer_ == NULL) {
      return 0;
    }
    width_ = width;
    height_ = height;
    return renderer_->SetSize(width_, height_, 0) ? 0 : -1;
  }
  virtual int DeliverFrame(unsigned char* buffer, int buffer_size,
                           unsigned int time_stamp) {
    talk_base::CritScope cs(&crit_);
    frame_rate_tracker_.Update(1);
    if (renderer_ == NULL) {
      return 0;
    }
    WebRtcVideoFrame video_frame;
    video_frame.Attach(buffer, buffer_size, width_, height_,
                       1, 1, 0, time_stamp, 0);

    // Add a watermark to the frame.
    if (!video_frame.AddWatermark()) {
      LOG(LS_ERROR) << "Failed to add watermark to decoded frame.";
    }

    int ret = renderer_->RenderFrame(&video_frame) ? 0 : -1;
    uint8* buffer_temp;
    size_t buffer_size_temp;
    video_frame.Detach(&buffer_temp, &buffer_size_temp);
    return ret;
  }

  unsigned int width() {
   talk_base::CritScope cs(&crit_);
   return width_;
  }
  unsigned int height() {
   talk_base::CritScope cs(&crit_);
   return height_;
  }
  int framerate() {
    talk_base::CritScope cs(&crit_);
    return frame_rate_tracker_.units_second();
  }

 private:
  talk_base::CriticalSection crit_;
  VideoRenderer* renderer_;
  unsigned int width_;
  unsigned int height_;
  talk_base::RateTracker frame_rate_tracker_;
};

class WebRtcDecoderObserver : public webrtc::ViEDecoderObserver {
 public:
  WebRtcDecoderObserver(int video_channel)
       : video_channel_(video_channel),
         framerate_(0),
         bitrate_(0),
         firs_requested_(0) { }

  // virtual functions from VieDecoderObserver.
  virtual void IncomingCodecChanged(const int videoChannel,
                                    const webrtc::VideoCodec& videoCodec) { }
  virtual void IncomingRate(const int videoChannel,
                            const unsigned int framerate,
                            const unsigned int bitrate) {
    ASSERT(video_channel_ == videoChannel);
    framerate_ = framerate;
    bitrate_ = bitrate;
  }
  virtual void RequestNewKeyFrame(const int videoChannel) {
    ASSERT(video_channel_ == videoChannel);
    ++firs_requested_;
  }

  int framerate() const { return framerate_; }
  int bitrate() const { return bitrate_; }
  int firs_requested() const { return firs_requested_; }

 private:
  int video_channel_;
  int framerate_;
  int bitrate_;
  int firs_requested_;
};

class WebRtcEncoderObserver : public webrtc::ViEEncoderObserver {
 public:
  WebRtcEncoderObserver(int video_channel)
       : video_channel_(video_channel), framerate_(0), bitrate_(0) { }

  // virtual functions from VieEncoderObserver.
  virtual void OutgoingRate(const int videoChannel,
                            const unsigned int framerate,
                            const unsigned int bitrate) {
    ASSERT(video_channel_ == videoChannel);
    framerate_ = framerate;
    bitrate_ = bitrate;
  }

  int framerate() const { return framerate_; }
  int bitrate() const { return bitrate_; }

 private:
  int video_channel_;
  int framerate_;
  int bitrate_;
};

class LocalStreamInfo {
 public:
  int width() {
    talk_base::CritScope cs(&crit_);
    return width_;
  }
  int height() {
    talk_base::CritScope cs(&crit_);
    return height_;
  }
  int framerate() {
    talk_base::CritScope cs(&crit_);
    return rate_tracker_.units_second();
  }

  void UpdateFrame(int width, int height) {
    talk_base::CritScope cs(&crit_);
    width_ = width;
    height_ = height;
    rate_tracker_.Update(1);
  }

 private:
  talk_base::CriticalSection crit_;
  unsigned int width_;
  unsigned int height_;
  talk_base::RateTracker rate_tracker_;
};

const WebRtcVideoEngine::VideoCodecPref
    WebRtcVideoEngine::kVideoCodecPrefs[] = {
    {"VP8", 100, 0},
    {"RED", kRedPayloadType, 1},
    {"ULPFEC", kFecPayloadType, 2},
};


// TODO: Add CanSendCodec()
// The formats are sorted by the descending order of width. We use the order to
// find the next format for CPU and bandwidth adaptation.
const VideoFormat WebRtcVideoEngine::kVideoFormats[] = {
  // TODO: Understand why we have problem with 16:9 formats.
  VideoFormat(1280, 800, VideoFormat::FpsToInterval(30), FOURCC_ANY),
//VideoFormat(1280, 720, VideoFormat::FpsToInterval(30), FOURCC_ANY),
  VideoFormat(960, 600, VideoFormat::FpsToInterval(30), FOURCC_ANY),
//VideoFormat(960, 540, VideoFormat::FpsToInterval(30), FOURCC_ANY),
  VideoFormat(640, 400, VideoFormat::FpsToInterval(30), FOURCC_ANY),
//VideoFormat(640, 360, VideoFormat::FpsToInterval(30), FOURCC_ANY),
  VideoFormat(480, 300, VideoFormat::FpsToInterval(30), FOURCC_ANY),
//VideoFormat(480, 270, VideoFormat::FpsToInterval(30), FOURCC_ANY),
  VideoFormat(320, 200, VideoFormat::FpsToInterval(30), FOURCC_ANY),
//VideoFormat(320, 180, VideoFormat::FpsToInterval(30), FOURCC_ANY),
  VideoFormat(240, 150, VideoFormat::FpsToInterval(30), FOURCC_ANY),
//VideoFormat(240, 135, VideoFormat::FpsToInterval(30), FOURCC_ANY),
  VideoFormat(160, 100, VideoFormat::FpsToInterval(30), FOURCC_ANY),
//VideoFormat(160, 90, VideoFormat::FpsToInterval(30), FOURCC_ANY),
};

const VideoFormat WebRtcVideoEngine::kDefaultVideoFormat =
    VideoFormat(640, 400, VideoFormat::FpsToInterval(30), FOURCC_ANY);

WebRtcVideoEngine::WebRtcVideoEngine() {
  Construct(new ViEWrapper(), new ViETraceWrapper(), NULL);
}

WebRtcVideoEngine::WebRtcVideoEngine(WebRtcVoiceEngine* voice_engine,
                                     ViEWrapper* vie_wrapper) {
  Construct(vie_wrapper, new ViETraceWrapper(), voice_engine);
}

WebRtcVideoEngine::WebRtcVideoEngine(WebRtcVoiceEngine* voice_engine,
                                     ViEWrapper* vie_wrapper,
                                     ViETraceWrapper* tracing) {
  Construct(vie_wrapper, tracing, voice_engine);
}

void  WebRtcVideoEngine::Construct(ViEWrapper* vie_wrapper,
                                   ViETraceWrapper* tracing,
                                   WebRtcVoiceEngine* voice_engine) {
  LOG(LS_INFO) << "WebRtcVideoEngine::WebRtcVideoEngine";
  vie_wrapper_.reset(vie_wrapper);
  tracing_.reset(tracing);
  voice_engine_ = voice_engine;
  initialized_ = false;
  log_level_ = kDefaultLogSeverity;
  render_module_.reset(new WebRtcPassthroughRender());
  local_renderer_w_ = local_renderer_h_ = 0;
  local_renderer_ = NULL;
  capture_started_ = false;

  ApplyLogging();
  if (tracing_->SetTraceCallback(this) != 0) {
    LOG_RTCERR1(SetTraceCallback, this);
  }

  // Set default quality levels for our supported codecs. We override them here
  // if we know your cpu performance is low, and they can be updated explicitly
  // by calling SetDefaultCodec.  For example by a flute preference setting, or
  // by the server with a jec in response to our reported system info.
  VideoCodec max_codec(kVideoCodecPrefs[0].payload_type,
                       kVideoCodecPrefs[0].name,
                       kDefaultVideoFormat.width,
                       kDefaultVideoFormat.height,
                       kDefaultVideoFormat.framerate(), 0);
  if (!SetDefaultCodec(max_codec)) {
    LOG(LS_ERROR) << "Failed to initialize list of supported codec types";
  }
}

WebRtcVideoEngine::~WebRtcVideoEngine() {
  LOG(LS_INFO) << "WebRtcVideoEngine::~WebRtcVideoEngine";
  if (initialized_) {
    Terminate();
  }
  tracing_->SetTraceCallback(NULL);
}

bool WebRtcVideoEngine::Init() {
  LOG(LS_INFO) << "WebRtcVideoEngine::Init";
  bool result = InitVideoEngine();
  if (result) {
    LOG(LS_INFO) << "VideoEngine Init done";
  } else {
    LOG(LS_ERROR) << "VideoEngine Init failed, releasing";
    Terminate();
  }
  return result;
}

bool WebRtcVideoEngine::InitVideoEngine() {
  LOG(LS_INFO) << "WebRtcVideoEngine::InitVideoEngine";

  if (vie_wrapper_->base()->Init() != 0) {
    LOG_RTCERR0(Init);
    return false;
  }

  if (!voice_engine_) {
    LOG(LS_WARNING) << "NULL voice engine";
  } else if ((vie_wrapper_->base()->SetVoiceEngine(
      voice_engine_->voe()->engine())) != 0) {
    LOG_RTCERR0(SetVoiceEngine);
    return false;
  }

  if ((vie_wrapper_->base()->RegisterObserver(*this)) != 0) {
    LOG_RTCERR0(RegisterObserver);
    return false;
  }

  if (vie_wrapper_->render()->RegisterVideoRenderModule(
      *render_module_.get()) != 0) {
    LOG_RTCERR0(RegisterVideoRenderModule);
    return false;
  }

  initialized_ = true;
  return true;
}

void WebRtcVideoEngine::Terminate() {
  LOG(LS_INFO) << "WebRtcVideoEngine::Terminate";
  initialized_ = false;
  SetCapture(false);

  if (vie_wrapper_->render()->DeRegisterVideoRenderModule(
      *render_module_.get()) != 0) {
    LOG_RTCERR0(DeRegisterVideoRenderModule);
  }

  if (vie_wrapper_->base()->DeregisterObserver() != 0) {
    LOG_RTCERR0(DeregisterObserver);
  }

  if (vie_wrapper_->base()->SetVoiceEngine(NULL) != 0) {
    LOG_RTCERR0(SetVoiceEngine);
  }
}

int WebRtcVideoEngine::GetCapabilities() {
  return VIDEO_RECV | VIDEO_SEND;
}

bool WebRtcVideoEngine::SetOptions(int options) {
  return true;
}

bool WebRtcVideoEngine::SetDefaultEncoderConfig(
    const VideoEncoderConfig& config) {
  return SetDefaultCodec(config.max_codec);
}

// SetDefaultCodec may be called while the capturer is running. For example, a
// test call is started in a page with QVGA default codec, and then a real call
// is started in another page with VGA default codec. This is the corner case
// and happens only when a session is started. We ignore this case currently.
bool WebRtcVideoEngine::SetDefaultCodec(const VideoCodec& codec) {
  if (!RebuildCodecList(codec)) {
    LOG(LS_WARNING) << "Failed to RebuildCodecList";
    return false;
  }

  default_codec_format_ = VideoFormat(
      video_codecs_[0].width,
      video_codecs_[0].height,
      VideoFormat::FpsToInterval(video_codecs_[0].framerate),
      FOURCC_ANY);
  return true;
}

WebRtcVideoMediaChannel* WebRtcVideoEngine::CreateChannel(
    VoiceMediaChannel* voice_channel) {
  WebRtcVideoMediaChannel* channel =
      new WebRtcVideoMediaChannel(this, voice_channel);
  if (!channel->Init()) {
    delete channel;
    channel = NULL;
  }
  return channel;
}

bool WebRtcVideoEngine::SetCaptureDevice(const Device* device) {
  if (!device) {
    video_capturer_.reset();
    LOG(LS_INFO) << "Camera set to NULL";
    return true;
  }
  // No-op if the device hasn't changed.
  if (video_capturer_.get() && video_capturer_->GetId() == device->id) {
    return true;
  }
  // Create a new capturer for the specified device.
  VideoCapturer* capturer = CreateVideoCapturer(*device);
  if (!capturer) {
    LOG(LS_ERROR) << "Failed to create camera '" << device->name << "', id='"
                  << device->id << "'";
    return false;
  }
  if (!SetCapturer(capturer)) {
    return false;
  }
  LOG(LS_INFO) << "Camera set to '" << device->name << "', id='"
               << device->id << "'";
  return true;
}

bool WebRtcVideoEngine::SetCaptureModule(webrtc::VideoCaptureModule* vcm) {
  if (!vcm) {
    video_capturer_.reset();
    LOG(LS_INFO) << "Camera set to NULL";
    return true;
  }
  // Create a new capturer for the specified device.
  WebRtcVideoCapturer* capturer = new WebRtcVideoCapturer;
  if (!capturer->Init(vcm)) {
    LOG(LS_ERROR) << "Failed to create camera from VCM";
    delete capturer;
    return false;
  }
  if (!SetCapturer(capturer)) {
    return false;
  }
  LOG(LS_INFO) << "Camera created with VCM";
  return true;
}

bool WebRtcVideoEngine::SetCapturer(VideoCapturer* capturer) {
  // Hook up signals and install the supplied capturer.
  SignalCaptureResult.repeat(capturer->SignalStartResult);
  capturer->SignalFrameCaptured.connect(this,
      &WebRtcVideoEngine::OnFrameCaptured);
  video_capturer_.reset(capturer);
  // Possibly restart the capturer if it is supposed to be running.
  CaptureResult result = UpdateCapturingState();
  if (result != CR_SUCCESS && result != CR_PENDING) {
    LOG(LS_WARNING) << "Camera failed to restart";
    return false;
  }
  return true;
}

bool WebRtcVideoEngine::SetLocalRenderer(VideoRenderer* renderer) {
  local_renderer_w_ = local_renderer_h_ = 0;
  local_renderer_ = renderer;
  return true;
}

CaptureResult WebRtcVideoEngine::SetCapture(bool capture) {
  bool old_capture = capture_started_;
  capture_started_ = capture;
  CaptureResult res = UpdateCapturingState();
  if (res != CR_SUCCESS && res != CR_PENDING) {
    capture_started_ = old_capture;
  }
  return res;
}

VideoCapturer* WebRtcVideoEngine::CreateVideoCapturer(const Device& device) {
  WebRtcVideoCapturer* capturer = new WebRtcVideoCapturer;
  if (!capturer->Init(device)) {
    delete capturer;
    return NULL;
  }
  return capturer;
}

CaptureResult WebRtcVideoEngine::UpdateCapturingState() {
  CaptureResult result = CR_SUCCESS;

  bool capture = capture_started_;
  if (!IsCapturing() && capture) {  // Start capturing.
    if (!video_capturer_.get()) {
      return CR_NO_DEVICE;
    }

    VideoFormat capture_format;
    if (!video_capturer_->GetBestCaptureFormat(default_codec_format_,
                                               &capture_format)) {
      LOG(LS_WARNING) << "Unsupported format:"
                      << " width=" << default_codec_format_.width
                      << " height=" << default_codec_format_.height
                      << ". Supported formats are:";
      const std::vector<VideoFormat>* formats =
          video_capturer_->GetSupportedFormats();
      for (std::vector<VideoFormat>::const_iterator i = formats->begin();
           i != formats->end(); ++i) {
        const VideoFormat& format = *i;
        LOG(LS_WARNING) << "  " << GetFourccName(format.fourcc) << ":"
                        << format.width << "x" << format.height << "x"
                        << format.framerate();
      }
      return CR_FAILURE;
    }

    // Start the video capturer.
    result = video_capturer_->Start(capture_format);
    if (CR_SUCCESS != result && CR_PENDING != result) {
      LOG(LS_ERROR) << "Failed to start the video capturer";
      return result;
    }
  } else if (IsCapturing() && !capture) {  // Stop capturing.
    video_capturer_->Stop();
  }

  return result;
}

bool WebRtcVideoEngine::IsCapturing() const {
  return video_capturer_.get() && video_capturer_->IsRunning();
}

void WebRtcVideoEngine::OnFrameCaptured(VideoCapturer* capturer,
                                        const CapturedFrame* frame) {
  // Force 16:10 for now. We'll be smarter with the capture refactor.
  int cropped_height = frame->width * kDefaultVideoFormat.height
      / kDefaultVideoFormat.width;

  // This CapturedFrame* will already be in I420. In the future, when
  // WebRtcVideoFrame has support for independent planes, we can just attach
  // to it and update the pointers when cropping.
  WebRtcVideoFrame i420_frame;
  if (!i420_frame.Init(frame, frame->width, cropped_height)) {
    LOG(LS_ERROR) << "Couldn't convert to I420! "
                  << frame->width << " x " << cropped_height;
    return;
  }

  // Send I420 frame to the local renderer.
  if (local_renderer_) {
    if (local_renderer_w_ != static_cast<int>(i420_frame.GetWidth()) ||
        local_renderer_h_ != static_cast<int>(i420_frame.GetHeight())) {
      local_renderer_->SetSize(local_renderer_w_ = i420_frame.GetWidth(),
                               local_renderer_h_ = i420_frame.GetHeight(), 0);
    }
    local_renderer_->RenderFrame(&i420_frame);
  }

  // Send I420 frame to the registered senders.
  talk_base::CritScope cs(&channels_crit_);
  for (VideoChannels::iterator it = channels_.begin();
      it != channels_.end(); ++it) {
    if ((*it)->sending()) (*it)->SendFrame(0, &i420_frame);
  }
}

const std::vector<VideoCodec>& WebRtcVideoEngine::codecs() const {
  return video_codecs_;
}

void WebRtcVideoEngine::SetLogging(int min_sev, const char* filter) {
  log_level_ = min_sev;
  ApplyLogging();
}

int WebRtcVideoEngine::GetLastEngineError() {
  return vie_wrapper_->error();
}

// Checks to see whether we comprehend and could receive a particular codec
bool WebRtcVideoEngine::FindCodec(const VideoCodec& in) {
  for (int i = 0; i < ARRAY_SIZE(kVideoFormats); ++i) {
    const VideoFormat& fmt = kVideoFormats[i];
    if ((in.width == 0 && in.height == 0) ||
        (fmt.width == in.width && fmt.height == in.height)) {
      for (int j = 0; j < ARRAY_SIZE(kVideoCodecPrefs); ++j) {
        VideoCodec codec(kVideoCodecPrefs[j].payload_type,
                         kVideoCodecPrefs[j].name, 0, 0, 0, 0);
        if (codec.Matches(in)) {
          return true;
        }
      }
    }
  }
  return false;
}

void WebRtcVideoEngine::ConvertToCricketVideoCodec(
    const webrtc::VideoCodec& in_codec, VideoCodec& out_codec) {
  out_codec.id = in_codec.plType;
  out_codec.name = in_codec.plName;
  out_codec.width = in_codec.width;
  out_codec.height = in_codec.height;
  out_codec.framerate = in_codec.maxFramerate;
}

bool WebRtcVideoEngine::ConvertFromCricketVideoCodec(
    const VideoCodec& in_codec, webrtc::VideoCodec& out_codec) {
  bool found = false;
  int ncodecs = vie_wrapper_->codec()->NumberOfCodecs();
  for (int i = 0; i < ncodecs; ++i) {
    if (vie_wrapper_->codec()->GetCodec(i, out_codec) == 0 &&
      in_codec.name == out_codec.plName) {
      found = true;
      break;
    }
  }

  if (!found) {
    LOG(LS_ERROR) << "invalid codec type";
    return false;
  }

  if (in_codec.id != 0)
    out_codec.plType = in_codec.id;

  if (in_codec.width != 0)
    out_codec.width = in_codec.width;

  if (in_codec.height != 0)
    out_codec.height = in_codec.height;

  if (in_codec.framerate != 0)
    out_codec.maxFramerate = in_codec.framerate;

  // Init the codec with the default bandwidth options.
  out_codec.maxBitrate = kMaxVideoBitrate;
  out_codec.startBitrate = kMinVideoBitrate;
  out_codec.minBitrate = kMinVideoBitrate;

  return true;
}

void WebRtcVideoEngine::RegisterChannel(WebRtcVideoMediaChannel *channel) {
  talk_base::CritScope cs(&channels_crit_);
  channels_.push_back(channel);
}

void WebRtcVideoEngine::UnregisterChannel(WebRtcVideoMediaChannel *channel) {
  talk_base::CritScope cs(&channels_crit_);
  std::remove(channels_.begin(), channels_.end(), channel);
}

bool WebRtcVideoEngine::SetVoiceEngine(WebRtcVoiceEngine* voice_engine) {
  if (initialized_) {
    LOG(LS_WARNING) << "SetVoiceEngine can not be called after Init.";
    return false;
  }
  voice_engine_ = voice_engine;
  return true;
}

bool WebRtcVideoEngine::EnableTimedRender() {
  if (initialized_) {
    LOG(LS_WARNING) << "EnableTimedRender can not be called after Init.";
    return false;
  }
  render_module_.reset(webrtc::VideoRender::CreateVideoRender(0, NULL,
      false, webrtc::kRenderExternal));
  return true;
}

void WebRtcVideoEngine::ApplyLogging() {
  int filter = 0;
  switch (log_level_) {
    case talk_base::LS_VERBOSE: filter |= webrtc::kTraceAll;
    case talk_base::LS_INFO: filter |= webrtc::kTraceStateInfo;
    case talk_base::LS_WARNING: filter |= webrtc::kTraceWarning;
    case talk_base::LS_ERROR: filter |=
        webrtc::kTraceError | webrtc::kTraceCritical;
  }
  tracing_->SetTraceFilter(filter);
}

// Rebuilds the codec list to be only those that are less intensive
// than the specified codec.
bool WebRtcVideoEngine::RebuildCodecList(const VideoCodec& in_codec) {
  if (!FindCodec(in_codec))
    return false;

  video_codecs_.clear();

  bool found = false;
  for (size_t i = 0; i < ARRAY_SIZE(kVideoCodecPrefs); ++i) {
    const VideoCodecPref& pref(kVideoCodecPrefs[i]);
    if (!found)
      found = (in_codec.name == pref.name);
    if (found) {
      VideoCodec codec(pref.payload_type, pref.name,
                       in_codec.width, in_codec.height, in_codec.framerate,
                       ARRAY_SIZE(kVideoCodecPrefs) - i);
      video_codecs_.push_back(codec);
    }
  }
  ASSERT(found);
  return true;
}

void WebRtcVideoEngine::PerformanceAlarm(const unsigned int cpu_load) {
  LOG(LS_INFO) << "WebRtcVideoEngine::PerformanceAlarm";
}

// Ignore spammy trace messages, mostly from the stats API when we haven't
// gotten RTCP info yet from the remote side.
static bool ShouldIgnoreTrace(const std::string& trace) {
  static const char* kTracesToIgnore[] = {
    "\tfailed to GetReportBlockInformation",
    NULL
  };
  for (const char* const* p = kTracesToIgnore; *p; ++p) {
    if (trace.find(*p) == 0) {
      return true;
    }
  }
  return false;
}

void WebRtcVideoEngine::Print(const webrtc::TraceLevel level,
                              const char* trace, const int length) {
  talk_base::LoggingSeverity sev = talk_base::LS_VERBOSE;
  if (level == webrtc::kTraceError || level == webrtc::kTraceCritical)
    sev = talk_base::LS_ERROR;
  else if (level == webrtc::kTraceWarning)
    sev = talk_base::LS_WARNING;
  else if (level == webrtc::kTraceStateInfo || level == webrtc::kTraceInfo)
    sev = talk_base::LS_INFO;

  if (sev >= log_level_) {
    // Skip past boilerplate prefix text
    if (length < 72) {
      std::string msg(trace, length);
      LOG(LS_ERROR) << "Malformed webrtc log message: ";
      LOG_V(sev) << msg;
    } else {
      std::string msg(trace + 71, length - 72);
      if (!ShouldIgnoreTrace(msg)) {
        LOG_V(sev) << "WebRtc ViE:" << msg;
      }
    }
  }
}

// TODO: stubs for now
bool WebRtcVideoEngine::RegisterProcessor(
    VideoProcessor* video_processor) {
  return true;
}
bool WebRtcVideoEngine::UnregisterProcessor(
    VideoProcessor* video_processor) {
  return true;
}

// WebRtcVideoMediaChannel

WebRtcVideoMediaChannel::WebRtcVideoMediaChannel(
    WebRtcVideoEngine* engine, VoiceMediaChannel* channel)
    : engine_(engine),
      voice_channel_(channel),
      vie_channel_(-1),
      vie_capture_(-1),
      external_capture_(NULL),
      sending_(false),
      render_started_(false),
      muted_(false),
      send_min_bitrate_(kMinVideoBitrate),
      send_max_bitrate_(kMaxVideoBitrate),
      local_stream_info_(new LocalStreamInfo()) {
  engine->RegisterChannel(this);
}

bool WebRtcVideoMediaChannel::Init() {
  if (engine_->video_engine()->base()->CreateChannel(vie_channel_) != 0) {
    LOG_RTCERR1(CreateChannel, vie_channel_);
    return false;
  }

  LOG(LS_INFO) << "WebRtcVideoMediaChannel::Init "
               << "vie_channel " << vie_channel_ << " created";

  // Connect the voice channel, if there is one.
  if (voice_channel_) {
    WebRtcVoiceMediaChannel* channel =
        static_cast<WebRtcVoiceMediaChannel*>(voice_channel_);
    if (engine_->video_engine()->base()->ConnectAudioChannel(
        vie_channel_, channel->voe_channel()) != 0) {
      LOG_RTCERR2(ConnectAudioChannel, vie_channel_, channel->voe_channel());
      LOG(LS_WARNING) << "A/V not synchronized";
      // Not a fatal error.
    }
  }

  // Register external transport.
  if (engine_->video_engine()->network()->RegisterSendTransport(
      vie_channel_, *this) != 0) {
    LOG_RTCERR1(RegisterSendTransport, vie_channel_);
    return false;
  }

  // Register external capture.
  if (engine()->video_engine()->capture()->AllocateExternalCaptureDevice(
      vie_capture_, external_capture_) != 0) {
    LOG_RTCERR0(AllocateExternalCaptureDevice);
    return false;
  }

  // Connect external capture.
  if (engine()->video_engine()->capture()->ConnectCaptureDevice(
      vie_capture_, vie_channel_) != 0) {
    LOG_RTCERR2(ConnectCaptureDevice, vie_capture_, vie_channel_);
    return false;
  }

  // Install render adapter.
  remote_renderer_.reset(new WebRtcRenderAdapter(NULL));
  if (engine_->video_engine()->render()->AddRenderer(vie_channel_,
      webrtc::kVideoI420, remote_renderer_.get()) != 0) {
    LOG_RTCERR3(AddRenderer, vie_channel_, webrtc::kVideoI420,
                remote_renderer_.get());
    remote_renderer_.reset();
    return false;
  }

  // Register decoder observer for incoming framerate and bitrate.
  decoder_observer_.reset(new WebRtcDecoderObserver(vie_channel_));
  if (engine()->video_engine()->codec()->RegisterDecoderObserver(
      vie_channel_, *decoder_observer_) != 0) {
    LOG_RTCERR1(RegisterDecoderObserver, decoder_observer_.get());
    return false;
  }

  // Register encoder observer for outgoing framerate and bitrate.
  encoder_observer_.reset(new WebRtcEncoderObserver(vie_channel_));
  if (engine()->video_engine()->codec()->RegisterEncoderObserver(
      vie_channel_, *encoder_observer_) != 0) {
    LOG_RTCERR1(RegisterEncoderObserver, encoder_observer_.get());
    return false;
  }

  if (!EnableRtcp())
    return false;

  if (!EnablePli())
    return false;

  if (!EnableNack())
    return false;

  return true;
}

WebRtcVideoMediaChannel::~WebRtcVideoMediaChannel() {
  if (vie_channel_ != -1) {
    // Stop the renderer.
    SetRender(false);
    if (remote_renderer_.get() &&
        engine()->video_engine()->render()->RemoveRenderer(vie_channel_) != 0) {
      LOG_RTCERR1(RemoveRenderer, vie_channel_);
    }

    // Destroy the external capture interface.
    if (vie_capture_ != -1) {
      if (engine()->video_engine()->capture()->DisconnectCaptureDevice(
          vie_channel_) != 0) {
        LOG_RTCERR1(DisconnectCaptureDevice, vie_channel_);
      }
      if (engine()->video_engine()->capture()->ReleaseCaptureDevice(
          vie_capture_) != 0) {
        LOG_RTCERR1(ReleaseCaptureDevice, vie_capture_);
      }
    }

    // Deregister external transport
    if (engine()->video_engine()->network()->DeregisterSendTransport(
        vie_channel_) != 0) {
      LOG_RTCERR1(DeregisterSendTransport, vie_channel_);
    }

    // Delete the VideoEngine channel.
    if (engine()->video_engine()->base()->DeleteChannel(vie_channel_) != 0) {
      LOG_RTCERR1(DeleteChannel, vie_channel_);
    }

    // Deregister decoder observer.
    if (engine()->video_engine()->codec()->DeregisterDecoderObserver(
        vie_channel_) != 0) {
      LOG_RTCERR1(DeregisterDecoderObserver, vie_channel_);
    }

    // Deregister encoder observer.
    if (engine()->video_engine()->codec()->DeregisterEncoderObserver(
        vie_channel_) != 0) {
      LOG_RTCERR1(DeregisterEncoderObserver, vie_channel_);
    }
  }

  // Unregister the channel from the engine.
  engine()->UnregisterChannel(this);
}

bool WebRtcVideoMediaChannel::SetRecvCodecs(
    const std::vector<VideoCodec>& codecs) {
  bool ret = true;
  for (std::vector<VideoCodec>::const_iterator iter = codecs.begin();
      iter != codecs.end(); ++iter) {
    if (engine()->FindCodec(*iter)) {
      webrtc::VideoCodec wcodec;
      if (engine()->ConvertFromCricketVideoCodec(*iter, wcodec)) {
        if (engine()->video_engine()->codec()->SetReceiveCodec(
            vie_channel_, wcodec) != 0) {
          LOG_RTCERR2(SetReceiveCodec, vie_channel_, wcodec.plName);
          ret = false;
        }
      }
    } else {
      LOG(LS_INFO) << "Unknown codec " << iter->name;
      ret = false;
    }
  }

  // make channel ready to receive packets
  if (ret) {
    if (engine()->video_engine()->base()->StartReceive(vie_channel_) != 0) {
      LOG_RTCERR1(StartReceive, vie_channel_);
      ret = false;
    }
  }
  return ret;
}

bool WebRtcVideoMediaChannel::SetSendCodecs(
    const std::vector<VideoCodec>& codecs) {
  // Match with local video codec list.
  std::vector<webrtc::VideoCodec> send_codecs;
  for (std::vector<VideoCodec>::const_iterator iter = codecs.begin();
      iter != codecs.end(); ++iter) {
    if (engine()->FindCodec(*iter)) {
      webrtc::VideoCodec wcodec;
      if (engine()->ConvertFromCricketVideoCodec(*iter, wcodec))
        send_codecs.push_back(wcodec);
    }
  }

  // Fail if we don't have a match.
  if (send_codecs.empty()) {
    LOG(LS_WARNING) << "No matching codecs avilable";
    return false;
  }

  // Select the first matched codec.
  webrtc::VideoCodec& codec(send_codecs[0]);

  // TODO: Remove this once WebRtcVideoFrame::Stretch() is implemented.
  // O3D has a default texture size 1024 x 512.
  // On streamon, the renderer needs to write the decoded frame into the
  // default texture. If the decoded frame is bigger than the texture buffer,
  // the renderer scales it down.
  // After the first frame is received by O3D, the renderer sends streamready
  // back to order a new texture with real size.
  // WebRtcVideoFrame::Stretch() is not implemented yet, so the first frame
  // can not be fit in the default 1024 x 512 texture, and this causes rendering
  // failure.
  // So here, as a workaround, we cut at 640 x 400 here.
  if (codec.width > engine()->default_codec_format().width ||
      codec.height > engine()->default_codec_format().height) {
    codec.width = engine()->default_codec_format().width;
    codec.height = engine()->default_codec_format().height;
  }

  // Set the default number of temporal layers for VP8.
  if (webrtc::kVideoCodecVP8 == codec.codecType) {
    codec.codecSpecific.VP8.numberOfTemporalLayers =
        kDefaultNumberOfTemporalLayers;
  }

  if (!SetSendCodec(codec, send_min_bitrate_, send_max_bitrate_)) {
    return false;
  }

  LOG(LS_INFO) << "Selected video codec " << send_codec_->plName << "/"
               << send_codec_->width << "x" << send_codec_->height << "x"
               << static_cast<int>(send_codec_->maxFramerate);
  if (webrtc::kVideoCodecVP8 == codec.codecType) {
    LOG(LS_INFO) << "VP8 number of layers: "
                 << send_codec_->codecSpecific.VP8.numberOfTemporalLayers;
  }
  return true;
}

bool WebRtcVideoMediaChannel::SetRender(bool render) {
  if (render == render_started_) {
    return true;  // no action required
  }

  bool ret = true;
  if (render) {
    if (engine()->video_engine()->render()->StartRender(vie_channel_) != 0) {
      LOG_RTCERR1(StartRender, vie_channel_);
      ret = false;
    }
  } else {
    if (engine()->video_engine()->render()->StopRender(vie_channel_) != 0) {
      LOG_RTCERR1(StopRender, vie_channel_);
      ret = false;
    }
  }
  if (ret) {
    render_started_ = render;
  }

  return ret;
}

bool WebRtcVideoMediaChannel::SetSend(bool send) {
  if (send == sending()) {
    return true;  // no action required
  }

  bool ret = true;
  if (send) {  // enable
    if (engine()->video_engine()->base()->StartSend(vie_channel_) != 0) {
      LOG_RTCERR1(StartSend, vie_channel_);
      ret = false;
    }
  } else {  // disable
    if (engine()->video_engine()->base()->StopSend(vie_channel_) != 0) {
      LOG_RTCERR1(StopSend, vie_channel_);
      ret = false;
    }
  }
  if (ret) {
    sending_ = send;
  }

  return ret;
}

bool WebRtcVideoMediaChannel::AddStream(uint32 ssrc, uint32 voice_ssrc) {
  return false;
}

bool WebRtcVideoMediaChannel::RemoveStream(uint32 ssrc) {
  return false;
}

bool WebRtcVideoMediaChannel::SetRenderer(
    uint32 ssrc, VideoRenderer* renderer) {
  if (ssrc != 0)
    return false;

  remote_renderer_->SetRenderer(renderer);
  return true;
}

bool WebRtcVideoMediaChannel::GetStats(VideoMediaInfo* info) {
  // Get RTP statistics.
  unsigned int bytes_sent;
  unsigned int packets_sent;
  unsigned int bytes_recv;
  unsigned int packets_recv;
  if (engine_->video_engine()->rtp()->GetRTPStatistics(vie_channel_,
          bytes_sent, packets_sent, bytes_recv, packets_recv) != 0) {
    LOG_RTCERR5(GetRTPStatistics, vie_channel_,
        bytes_sent, packets_sent, bytes_recv, packets_recv);
    return false;
  }

  // Get received RTCP statistics.
  uint16 r_fraction_lost;
  unsigned int r_cumulative_lost;
  unsigned int r_extended_max;
  unsigned int r_jitter;
  int r_rtt_ms;
  if (engine_->video_engine()->rtp()->GetReceivedRTCPStatistics(vie_channel_,
          r_fraction_lost, r_cumulative_lost, r_extended_max,
          r_jitter, r_rtt_ms) != 0) {
    LOG_RTCERR6(GetReceivedRTCPStatistics, vie_channel_,
        r_fraction_lost, r_cumulative_lost, r_extended_max, r_jitter, r_rtt_ms);
    return false;
  }

  // Get sent RTCP statistics.
  uint16 s_fraction_lost;
  unsigned int s_cumulative_lost;
  unsigned int s_extended_max;
  unsigned int s_jitter;
  int s_rtt_ms;
  if (engine_->video_engine()->rtp()->GetReceivedRTCPStatistics(vie_channel_,
          s_fraction_lost, s_cumulative_lost, s_extended_max,
          s_jitter, s_rtt_ms) != 0) {
    LOG_RTCERR6(GetReceivedRTCPStatistics, vie_channel_,
        s_fraction_lost, s_cumulative_lost, s_extended_max, s_jitter, s_rtt_ms);
    return false;
  }

  unsigned int ssrc;

  // Build VideoSenderInfo.
  if (engine_->video_engine()->rtp()->GetLocalSSRC(vie_channel_,
                                                   ssrc) != 0) {
    LOG_RTCERR2(GetLocalSSRC, vie_channel_, ssrc);
    return false;
  }
  VideoSenderInfo sinfo;
  sinfo.ssrc = ssrc;
  sinfo.codec_name = send_codec_.get() ? send_codec_->plName : "";
  sinfo.bytes_sent = bytes_sent;
  sinfo.packets_sent = packets_sent;
  sinfo.packets_cached = -1;
  sinfo.packets_lost = r_cumulative_lost;  // from ReceivedRTCP
  sinfo.fraction_lost = r_fraction_lost;  // from ReceivedRTCP
  sinfo.firs_rcvd = -1;
  sinfo.nacks_rcvd = -1;
  sinfo.rtt_ms = s_rtt_ms;
  sinfo.frame_width = local_stream_info_->width();
  sinfo.frame_height = local_stream_info_->height();
  sinfo.framerate_input = local_stream_info_->framerate();
  sinfo.framerate_sent = encoder_observer_->framerate();
  sinfo.nominal_bitrate = encoder_observer_->bitrate();
  sinfo.preferred_bitrate = kMaxVideoBitrate;
  info->senders.push_back(sinfo);

  // Build VideoReceiverInfo.
  if (engine_->video_engine()->rtp()->GetRemoteSSRC(vie_channel_,
                                                    ssrc) != 0) {
    LOG_RTCERR2(GetRemoteSSRC, vie_channel_, ssrc);
    return false;
  }
  VideoReceiverInfo rinfo;
  rinfo.ssrc = ssrc;
  rinfo.bytes_rcvd = bytes_recv;
  rinfo.packets_rcvd = packets_recv;
  rinfo.packets_lost = s_cumulative_lost;  // from SentRTCP
  rinfo.packets_concealed = -1;
  rinfo.fraction_lost = s_fraction_lost;  // from SentRTCP
  rinfo.firs_sent = decoder_observer_->firs_requested();
  rinfo.nacks_sent = -1;
  rinfo.frame_width = remote_renderer_->width();
  rinfo.frame_height = remote_renderer_->height();
  rinfo.framerate_rcvd = decoder_observer_->framerate();
  int fps = remote_renderer_->framerate();
  rinfo.framerate_decoded = fps;
  rinfo.framerate_output = fps;
  info->receivers.push_back(rinfo);

  // Build BandwidthEstimationInfo.
  // TODO: Fill in BWE once we have them.
  unsigned int total_bitrate_sent;
  unsigned int fec_bitrate_sent;
  unsigned int nack_bitrate_sent;
  if (engine_->video_engine()->rtp()->GetBandwidthUsage(
      vie_channel_,
      total_bitrate_sent,
      fec_bitrate_sent,
      nack_bitrate_sent) != 0) {
    LOG_RTCERR4(GetBandwidthUsage, vie_channel_,
                total_bitrate_sent, fec_bitrate_sent, nack_bitrate_sent);
    return false;
  }
  BandwidthEstimationInfo bwe;
  bwe.actual_enc_bitrate = total_bitrate_sent - nack_bitrate_sent -
      fec_bitrate_sent;
  bwe.transmit_bitrate = total_bitrate_sent;
  bwe.retransmit_bitrate = nack_bitrate_sent;
  info->bw_estimations.push_back(bwe);

  return true;
}

bool WebRtcVideoMediaChannel::SendIntraFrame() {
  bool ret = true;
  if (engine()->video_engine()->codec()->SendKeyFrame(vie_channel_) != 0) {
    LOG_RTCERR1(SendKeyFrame, vie_channel_);
    ret = false;
  }

  return ret;
}

bool WebRtcVideoMediaChannel::RequestIntraFrame() {
  // There is no API exposed to application to request a key frame
  // ViE does this internally when there are errors from decoder
  return false;
}

void WebRtcVideoMediaChannel::OnPacketReceived(talk_base::Buffer* packet) {
  engine()->video_engine()->network()->ReceivedRTPPacket(vie_channel_,
                                                         packet->data(),
                                                         packet->length());
}

void WebRtcVideoMediaChannel::OnRtcpReceived(talk_base::Buffer* packet) {
  engine_->video_engine()->network()->ReceivedRTCPPacket(vie_channel_,
                                                         packet->data(),
                                                         packet->length());
}

void WebRtcVideoMediaChannel::SetSendSsrc(uint32 id) {
  if (!sending_) {
    if (engine()->video_engine()->rtp()->SetLocalSSRC(vie_channel_,
                                                      id) != 0) {
      LOG_RTCERR1(SetLocalSSRC, vie_channel_);
    }
  } else {
    LOG(LS_ERROR) << "Channel already in send state";
  }
}

bool WebRtcVideoMediaChannel::SetRtcpCName(const std::string& cname) {
  if (engine()->video_engine()->rtp()->SetRTCPCName(vie_channel_,
                                                    cname.c_str()) != 0) {
    LOG_RTCERR2(SetRTCPCName, vie_channel_, cname.c_str());
    return false;
  }
  return true;
}

bool WebRtcVideoMediaChannel::Mute(bool on) {
  muted_ = on;
  return true;
}

bool WebRtcVideoMediaChannel::SetSendBandwidth(bool autobw, int bps) {
  LOG(LS_INFO) << "RtcVideoMediaChanne::SetSendBandwidth";

  if (!send_codec_.get()) {
    LOG(LS_INFO) << "The send codec has not been set up yet.";
    return true;
  }

  int min_bitrate = kMinVideoBitrate;
  int max_bitrate = kMaxVideoBitrate;
  if (autobw) {
    // Use the default values as min
    min_bitrate = kMinVideoBitrate;
    // Use the default value or the bps for the max
    max_bitrate = (bps <= 0) ? kMaxVideoBitrate : (bps / 1000);
  } else {
    // Use the default start or the bps as the target bitrate.
    int target_bitrate = (bps <= 0) ? kMinVideoBitrate : (bps / 1000);
    min_bitrate = target_bitrate;
    max_bitrate = target_bitrate;
  }

  if (!SetSendCodec(*send_codec_, min_bitrate, max_bitrate)) {
    return false;
  }

  return true;
}

bool WebRtcVideoMediaChannel::SetOptions(int options) {
  return true;
}

void WebRtcVideoMediaChannel::SetInterface(NetworkInterface* iface) {
  MediaChannel::SetInterface(iface);
  // Set the RTP recv/send buffer to a bigger size
  if (network_interface_) {
    network_interface_->SetOption(NetworkInterface::ST_RTP,
                                  talk_base::Socket::OPT_RCVBUF,
                                  kVideoRtpBufferSize);
    network_interface_->SetOption(NetworkInterface::ST_RTP,
                                  talk_base::Socket::OPT_SNDBUF,
                                  kVideoRtpBufferSize);
  }
}

// TODO: Add unittests to test this function.
bool WebRtcVideoMediaChannel::SendFrame(uint32 ssrc, const VideoFrame* frame) {
  if (ssrc != 0 || !external_capture_) {
    return false;
  }

  // Update local stream statistics.
  local_stream_info_->UpdateFrame(frame->GetWidth(), frame->GetHeight());

  // Blacken the frame if video is muted.
  const VideoFrame* frame_out = frame;
  talk_base::scoped_ptr<VideoFrame> black_frame;
  if (muted_) {
    black_frame.reset(frame->Copy());
    black_frame->SetToBlack();
    frame_out = black_frame.get();
  }

  webrtc::ViEVideoFrameI420 frame_i420;
  // TODO: Update the webrtc::ViEVideoFrameI420
  // to use const unsigned char*
  frame_i420.y_plane = const_cast<unsigned char*>(frame_out->GetYPlane());
  frame_i420.u_plane = const_cast<unsigned char*>(frame_out->GetUPlane());
  frame_i420.v_plane = const_cast<unsigned char*>(frame_out->GetVPlane());
  frame_i420.y_pitch = frame_out->GetYPitch();
  frame_i420.u_pitch = frame_out->GetUPitch();
  frame_i420.v_pitch = frame_out->GetVPitch();
  frame_i420.width = frame_out->GetWidth();
  frame_i420.height = frame_out->GetHeight();

  // Convert from nanoseconds to milliseconds.
  WebRtc_Word64 clocks = frame_out->GetTimeStamp() /
      talk_base::kNumNanosecsPerMillisec;

  return (external_capture_->IncomingFrameI420(frame_i420, clocks) == 0);
}

bool WebRtcVideoMediaChannel::EnableRtcp() {
  if (engine()->video_engine()->rtp()->SetRTCPStatus(
          vie_channel_, webrtc::kRtcpCompound_RFC4585) != 0) {
    LOG_RTCERR2(SetRTCPStatus, vie_channel_, webrtc::kRtcpCompound_RFC4585);
    return false;
  }
  return true;
}

bool WebRtcVideoMediaChannel::EnablePli() {
  if (engine_->video_engine()->rtp()->SetKeyFrameRequestMethod(
          vie_channel_, webrtc::kViEKeyFrameRequestPliRtcp) != 0) {
    LOG_RTCERR2(SetRTCPStatus,
                vie_channel_, webrtc::kViEKeyFrameRequestPliRtcp);
    return false;
  }
  return true;
}

bool WebRtcVideoMediaChannel::EnableTmmbr() {
  if (engine_->video_engine()->rtp()->SetTMMBRStatus(vie_channel_, true) != 0) {
    LOG_RTCERR1(SetTMMBRStatus, vie_channel_);
    return false;
  }
  return true;
}

bool WebRtcVideoMediaChannel::EnableNack() {
  if (engine_->video_engine()->rtp()->SetNACKStatus(vie_channel_, true) != 0) {
    LOG_RTCERR1(SetNACKStatus, vie_channel_);
    return false;
  }
  return true;
}

bool WebRtcVideoMediaChannel::EnableNackFec(int red_payload_type,
                                            int fec_payload_type) {
  if (engine_->video_engine()->rtp()->SetHybridNACKFECStatus(
          vie_channel_, true, red_payload_type, fec_payload_type) != 0) {
    LOG_RTCERR3(SetHybridNACKFECStatus,
                vie_channel_, red_payload_type, fec_payload_type);
    return false;
  }
  return true;
}

bool WebRtcVideoMediaChannel::SetSendCodec(const webrtc::VideoCodec& codec,
    int min_bitrate, int max_bitrate) {
  // Make a copy of the codec
  webrtc::VideoCodec target_codec = codec;
  target_codec.startBitrate = min_bitrate;
  target_codec.minBitrate = min_bitrate;
  target_codec.maxBitrate = max_bitrate;

  if (engine()->video_engine()->codec()->SetSendCodec(vie_channel_,
      target_codec) != 0) {
    LOG_RTCERR2(SetSendCodec, vie_channel_, send_codec_->plName);
    return false;
  }

  // Reset the send_codec_ only if SetSendCodec is success.
  send_codec_.reset(new webrtc::VideoCodec(target_codec));
  send_min_bitrate_ = min_bitrate;
  send_max_bitrate_ = max_bitrate;

  return true;
}

int WebRtcVideoMediaChannel::SendPacket(int channel, const void* data,
                                        int len) {
  if (!network_interface_) {
    return -1;
  }
  talk_base::Buffer packet(data, len, kMaxRtpPacketLen);
  return network_interface_->SendPacket(&packet) ? len : -1;
}

int WebRtcVideoMediaChannel::SendRTCPPacket(int channel,
                                            const void* data,
                                            int len) {
  if (!network_interface_) {
    return -1;
  }
  talk_base::Buffer packet(data, len, kMaxRtpPacketLen);
  return network_interface_->SendRtcp(&packet) ? len : -1;
}

}  // namespace cricket

#endif  // HAVE_WEBRTC_VIDEO
