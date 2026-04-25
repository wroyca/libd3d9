#pragma once

namespace d3d9
{
  // Interception points wired into the D3D9 device vtable and the D3D9
  // factory vtable.
  //
  // Each enumerator represents an event. The exact dispatch semantics (for
  // example, whether we fire before or after delegating to the original
  // function) and the corresponding callback signatures are defined in the
  // event traits.
  //
  enum class event_id : unsigned int
  {
    // Fired after BeginScene() returns S_OK. We do not fire if the
    // scene failed to start.
    //
    begin_scene,

    // Fired just before we delegate EndScene() to the original
    // implementation. This is where subscribers can safely issue
    // additional draw calls.
    //
    end_scene,

    // Fired before we delegate Present() to the original.
    //
    present,

    // Fired before we delegate Reset(). Subscribers must release all
    // D3DPOOL_DEFAULT resources here, otherwise the device will refuse
    // to reset.
    //
    pre_reset,

    // Fired after the original Reset() implementation returns.
    //
    post_reset,

    // Fired once when TestCooperativeLevel() notices the device went
    // from an available state to lost (or not reset).
    //
    device_lost,

    // Fired once when TestCooperativeLevel() notices the device is
    // back to S_OK after being in a lost state.
    //
    device_restored,

    // Fired after IDirect3D9::CreateDevice() succeeds and returns a
    // new IDirect3DDevice9 to the application. Emitted by factory, not
    // by device.
    //
    device_created
  };
}
