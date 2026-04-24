#pragma once

#include <functional>

#include <libd3d9/d3d9-win32.hxx>

#include <libd3d9/d3d9-event-id.hxx>

namespace d3d9
{
  // Per-event callback type information.
  //
  // Provides a nested type, callback_type, that carries the exact signature you
  // have to supply for a given event. We intentionally leave the primary
  // template undefined. This way, if you forget to add a specialisation for a
  // new event_id, you get a hard compile error instead of weird runtime bugs.
  //
  template <event_id e>
  struct event_traits;

  // begin_scene
  //
  // Fired after IDirect3DDevice9::BeginScene() returns S_OK. At this point,
  // the device is in a valid scene context so we can start rendering.
  //
  template <>
  struct event_traits<event_id::begin_scene>
  {
    using callback_type = std::function<void (IDirect3DDevice9& d)>;
  };

  // end_scene
  //
  // Fired just before EndScene() delegates to the original. We can still
  // issue draw calls here since the scene context is still open.
  //
  template <>
  struct event_traits<event_id::end_scene>
  {
    using callback_type = std::function<void (IDirect3DDevice9& d)>;
  };

  // present
  //
  // Fired before Present() hits the original implementation. The parameters
  // here map exactly to IDirect3DDevice9::Present().
  //
  template <>
  struct event_traits<event_id::present>
  {
    using callback_type = std::function<void (IDirect3DDevice9& d,
                                              const RECT* sr,
                                              const RECT* dr,
                                              HWND dw,
                                              const RGNDATA* r)>;
  };

  // pre_reset
  //
  // Fired right before Reset() is called on the original device.
  //
  // Note that you MUST release all D3DPOOL_DEFAULT resources here (render
  // targets, default pool buffers, state blocks, etc). If we forget, the
  // underlying Reset() will just fail with D3DERR_INVALIDCALL.
  //
  template <>
  struct event_traits<event_id::pre_reset>
  {
    using callback_type =
      std::function<void (IDirect3DDevice9& d, D3DPRESENT_PARAMETERS& pp)>;
  };

  // post_reset
  //
  // Fired right after the original Reset() call returns. We pass the result
  // along so you can check if it actually succeeded (S_OK) before trying to
  // recreate the resources dropped in pre_reset. If it didn't succeed, the
  // device might still be lost.
  //
  template <>
  struct event_traits<event_id::post_reset>
  {
    using callback_type = std::function<void (IDirect3DDevice9& d,
                                              D3DPRESENT_PARAMETERS& pp,
                                              HRESULT r)>;
  };

  // device_lost
  //
  // Fired once when the device goes from available to lost. We trigger this
  // on the first TestCooperativeLevel() call that returns DEVICELOST or
  // DEVICENOTRESET. We won't fire it again until the device recovers and
  // gets lost a second time.
  //
  template <>
  struct event_traits<event_id::device_lost>
  {
    using callback_type = std::function<void (IDirect3DDevice9& d)>;
  };

  // device_restored
  //
  // Fired once when the device transitions back from lost to available. We
  // trigger this on the first TestCooperativeLevel() call that returns S_OK.
  //
  // Keep in mind the device is accessible here, but Reset() hasn't
  // necessarily been called yet. If you need to recreate D3DPOOL_DEFAULT
  // resources, you should intercept into post_reset instead.
  //
  template <>
  struct event_traits<event_id::device_restored>
  {
    using callback_type = std::function<void (IDirect3DDevice9& d)>;
  };

  // device_created
  //
  // Fired by factory after IDirect3D9::CreateDevice() returns S_OK. At this
  // point, the IDirect3DDevice9 is fully initialised and the application has
  // already received the pointer via its own out-parameter.
  //
  // The callback receives a reference to the freshly created device and the
  // presentation parameters that were used to create it. The latter is
  // particularly useful for driving an immediate d3d9::device setup since the
  // swap-chain dimensions are already known.
  //
  // Note that subscribing to this event does not automatically install a
  // d3d9::device manager. That remains the caller's responsibility. A typical
  // subscriber creates a d3d9::device from the received pointer and stores it
  // alongside any subscription tokens for further event handling.
  //
  template <>
  struct event_traits<event_id::device_created>
  {
    using callback_type = std::function<void (IDirect3DDevice9& d,
                                              D3DPRESENT_PARAMETERS& pp)>;
  };
}
