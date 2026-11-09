#include "shared_encoder_factory.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <vector>

#include "api/video_codecs/sdp_video_format.h"
#include "passthrough_video_encoder.h"
#include "rtc_base/logging.h"
#include "shared_screen_pipeline.h"

namespace discourse {
namespace shared_encoder {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool IsSharedFormat(const webrtc::SdpVideoFormat& format) {
  if (Lower(format.name) != "h264") return false;
  auto it = format.parameters.find("profile-level-id");
  if (it == format.parameters.end()) return false;
  const std::string id = Lower(it->second);
  return id == "640033" || id == "640034";
}

// A format the wrapped factory really lists, to build the H.264 codec behind a
// shared format: the first H.264 with packetization-mode 1 (stock lists have
// Baseline, Constrained Baseline and Main at level 3.1), else any H.264. Never
// a hard-coded format: the wrapped factory returns null for formats it does
// not list. Both stock encoder and decoder handle what High level 5.x senders
// and receivers need (a decoder reads any profile; the software camera
// encoder emits a subset every High decoder accepts).
std::optional<webrtc::SdpVideoFormat> FindStockH264Format(
    const std::vector<webrtc::SdpVideoFormat>& formats) {
  std::optional<webrtc::SdpVideoFormat> any;
  for (const auto& f : formats) {
    if (Lower(f.name) != "h264" || IsSharedFormat(f)) continue;
    auto mode = f.parameters.find("packetization-mode");
    if (mode != f.parameters.end() && mode->second == "1") return f;
    if (!any) any = f;
  }
  return any;
}

webrtc::SdpVideoFormat SharedFormat(const char* profile_level_id) {
  return webrtc::SdpVideoFormat(
      "H264", {{"level-asymmetry-allowed", "1"},
               {"packetization-mode", "1"},
               {"profile-level-id", profile_level_id}});
}

class SharedEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  explicit SharedEncoderFactory(
      std::unique_ptr<webrtc::VideoEncoderFactory> inner)
      : inner_(std::move(inner)) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    auto formats = inner_->GetSupportedFormats();
    if (GetEncoderCapabilities().available) {
      formats.push_back(SharedFormat("640033"));
      formats.push_back(SharedFormat("640034"));
    }
    return formats;
  }

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode,
      std::optional<webrtc::VideoEncoderFactory::Resolution> resolution)
      const override {
    if (IsSharedFormat(format)) {
      CodecSupport support;
      support.is_supported =
          GetEncoderCapabilities().available && !scalability_mode;
      support.is_power_efficient = support.is_supported;
      return support;
    }
    return inner_->QueryCodecSupport(format, scalability_mode, resolution);
  }

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    if (IsSharedFormat(format)) {
      if (!GetEncoderCapabilities().available) return nullptr;
      // Camera frames on the same sender use the wrapped factory's ordinary
      // H.264 encoder (null if it has none: pixel frames then fail closed).
      std::unique_ptr<webrtc::VideoEncoder> software;
      if (auto stock = FindStockH264Format(inner_->GetSupportedFormats())) {
        software = inner_->Create(env, *stock);
      }
      return std::make_unique<PassthroughVideoEncoder>(std::move(software));
    }
    return inner_->Create(env, format);
  }

 private:
  std::unique_ptr<webrtc::VideoEncoderFactory> inner_;
};

bool HasH264(const std::vector<webrtc::SdpVideoFormat>& formats) {
  for (const auto& f : formats) {
    if (Lower(f.name) == "h264") return true;
  }
  return false;
}

class SharedDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  explicit SharedDecoderFactory(
      std::unique_ptr<webrtc::VideoDecoderFactory> inner)
      : inner_(std::move(inner)), has_h264_(HasH264(inner_->GetSupportedFormats())) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    auto formats = inner_->GetSupportedFormats();
    if (has_h264_) {
      formats.push_back(SharedFormat("640033"));
      formats.push_back(SharedFormat("640034"));
    }
    return formats;
  }

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format, bool reference_scaling,
      std::optional<webrtc::VideoDecoderFactory::Resolution> resolution)
      const override {
    if (IsSharedFormat(format)) {
      CodecSupport support;
      support.is_supported = has_h264_ && !reference_scaling;
      return support;
    }
    return inner_->QueryCodecSupport(format, reference_scaling, resolution);
  }

  std::unique_ptr<webrtc::VideoDecoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    if (IsSharedFormat(format)) {
      if (!has_h264_) return nullptr;
      // The stock decoder reads any H.264 profile and level: build it from a
      // format the wrapped factory really lists.
      auto stock = FindStockH264Format(inner_->GetSupportedFormats());
      return stock ? inner_->Create(env, *stock) : nullptr;
    }
    return inner_->Create(env, format);
  }

 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> inner_;
  const bool has_h264_;
};

}  // namespace

const EncoderCapabilities& GetEncoderCapabilities() {
  static const EncoderCapabilities caps = ProbeEncoderCapabilities();
  return caps;
}

std::unique_ptr<webrtc::VideoEncoderFactory> CreateSharedEncoderFactory(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner) {
  return std::make_unique<SharedEncoderFactory>(std::move(inner));
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateSharedDecoderFactory(
    std::unique_ptr<webrtc::VideoDecoderFactory> inner) {
  return std::make_unique<SharedDecoderFactory>(std::move(inner));
}

}  // namespace shared_encoder
}  // namespace discourse
