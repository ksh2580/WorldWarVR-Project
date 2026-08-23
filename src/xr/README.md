# WorldAtWarVR XR core

This directory is the engine-neutral half of the VR mod. It contains:

- `OpenXrRuntime`: instance, HMD system, D3D11 session, local/stage space,
  stereo swapchains, frame lifecycle, controller actions, and haptics.
- `D3D9ExSharedTextureBridge`: a three-slot (configurable) GPU bridge from the
  game's packed side-by-side D3D9Ex back buffer to the OpenXR D3D11 device.
- `D3D9CpuCapture`: a functional synchronous readback/upload fallback for the
  stock game's plain D3D9 device, intended to unblock early mono/stereo tests.
- `D3D11Compositor`: an sRGB-correct full-screen-triangle copy from each half
  of the packed game frame into its OpenXR eye swapchain.
- `xr_math`: the OpenXR-to-IW axis conversion and relative tracked-pose helper.

No header in this directory includes an external game-source or T4 header. A T4 adapter that
activates the shared bridge is responsible for:

1. Making the retail game create a real `IDirect3DDevice9Ex` and handling
   D3D9Ex-incompatible managed resources.
2. Rendering two eyes into a packed back buffer without advancing simulation.
3. Calling `CaptureBackBuffer(device, &frame)` before `Present` and bracketing
   compositor use with `AcquireLatest` / `Release`.
4. Mapping `ActionSnapshot` onto the verified T4 `usercmd` layout and applying
   converted HMD/controller poses to verified camera and weapon hooks.
5. Calling `Invalidate(runtime.d3d11_context())` before `Reset`/`ResetEx` while
   no consumer frame is acquired. Defer the reset when it returns false.

The current validated T4 host does not yet satisfy that device contract. It
discovers an already-created `IDirect3DDevice9` and swap chain, then hooks
`IDirect3DSwapChain9::Present` and `IDirect3DDevice9::Reset`. It does not own a
`Direct3DCreate9Ex`/`CreateDeviceEx` path, migrate D3D9Ex-incompatible managed
resources, or intercept `ResetEx`. A successful opportunistic `QueryInterface`
would therefore not prove the shared-handle and reset lifetime. The Present
path deliberately keeps `D3D9CpuCapture` active until those prerequisites are
implemented and validated together; it reports that active path once per
process rather than pretending the dormant bridge is in use.

The Ex bridge intentionally remains GPU-only. `D3D9CpuCapture` is a separate,
explicit fallback because it moves tens of MiB per frame at VR side-by-side
resolutions and is useful for diagnostics, not for a playable target.

Typical render-thread order is:

```text
runtime.PollEvents()
runtime.BeginFrame(frame)
render T4 left/right eyes into packed D3D9 target
bridge.CaptureBackBuffer(d3d9_device, &frame) // before Present
bridge.AcquireLatest(runtime D3D11 device/context, shared)
compositor.RenderStereo(runtime, frame, shared.source)
bridge.Release(runtime D3D11 context, shared)
runtime.EndFrame(frame, {
    .kind = compositor_succeeded
        ? CompositionLayerKind::projection
        : CompositionLayerKind::none,
    .projection_source = &shared.source,
})
```

Until the host can create a real D3D9Ex device, replace the bridge acquire and
release pair with `cpu_capture.UploadLatest(...)`; the returned source can be
passed to the same compositor and needs no release call.

All bridge calls must run on the render thread that owns the D3D9 device. The
mutex protects bookkeeping but cannot make a non-multithreaded D3D9 device safe
to call from another thread.

The compositor also deliberately flushes the D3D11 immediate context once per
eye immediately before `xrReleaseSwapchainImage`. The current loop transfers
the first acquired eye back to OpenXR before acquiring and drawing the second.
Removing either flush would let the runtime consume an image whose draw may
still be queued. One-flush batching requires a two-eye acquire/draw/flush/release
transaction with explicit cleanup for a second-eye acquisition or draw failure;
that lifecycle change remains deferred rather than weakening the proven
release boundary.

Set `WAWVR_FRAME_TIMING_DIAGNOSTICS=1` before launch to enable approximately
60-second timing aggregates. They report predicted display period and inferred
refresh, adjacent predicted-time steps and inferred missed intervals,
`xrWaitFrame`, application work, `xrEndFrame`, and `shouldRender=false` counts.
No clock sampling or timing log is performed when the switch is absent. Set
`WAWVR_FX_STEREO_DIAGNOSTICS=1` only for the separate backend FX-state
diagnostic; ordinary play leaves that render-thread telemetry disabled. The
stereo compositor enables its eye-local LDR bloom approximation only during
gameplay stereo. Set `WAWVR_DISABLE_EYE_LOCAL_BLOOM=1` before launch to return
to the exact compositor copy path for visual A/B testing.
