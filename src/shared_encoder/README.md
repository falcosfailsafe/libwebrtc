# Shared NVENC screen share (Phase 2 native)

Overlay copied over the libwebrtc wrapper (`tool/libwebrtc/stage.py`). Replaces
two wrapper files whole (derived from tag 4515f9c): `BUILD.gn` (sources, deps,
d3d11/dxgi libs, include dirs) and `src/rtc_peerconnection_factory_impl.cc`
(wraps the encoder and decoder factories). Re-derive both on a wrapper bump.

Flow: `D3D11Capture` (DXGI duplication, video processor to NV12, GPU only) ->
`NvencH264Encoder` (one session) -> `SharedScreenTrackSource` (VideoBroadcaster)
-> each peer's `PassthroughVideoEncoder` -> `KeyframeGate`
(`passthrough_core.h`) -> packetizer.

* Frames carry `SharedEncodedFrameBuffer` (kNative, no pixels, `ToI420()` is
  null; recognised by `storage_representation()`, the build has no RTTI). A
  frame that reaches any other encoder fails closed.
* One sender may carry camera and screen: `PassthroughVideoEncoder` sends
  pixel frames to a software H.264 encoder (from the wrapped factory) and
  shared frames through the gate; switching either way restarts on a keyframe.
  While in camera mode the encoder reports the software encoder's info.
* The factory advertises H.264 High 5.1/5.2 (`640033`/`640034`) for encoding
  only when `ProbeEncoderCapabilities()` opened a real capture + NVENC
  session; for decoding always (software receivers), see
  `CreateSharedDecoderFactory`.
* Drops: libwebrtc may drop frames before `Encode()`; the gap in `sequence`
  closes the peer's gate until the next IDR with SPS/PPS.
* Exports for the plugin: `include/rtc_shared_screen.h` (`extern "C"`).

Verification status: `tool/libwebrtc/run_native_tests.sh` builds the portable
core tests (gate, passthrough, preset math, Annex B) and compile-checks the
NVENC wrapper against the vendored header on Linux. Everything that includes
libwebrtc or D3D11 headers is written against the m150 sources but has NOT been
compiled or run. Nothing has run on a GPU.
