#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <stdexcept>

#include <libd3d9/d3d9-win32.hxx>

#include <libd3d9/d3d9-event-id.hxx>
#include <libd3d9/d3d9-event-traits.hxx>
#include <libd3d9/d3d9-subscription-token.hxx>

#include <libd3d9/detail/d3d9-event-dispatcher.hxx>
#include <libd3d9/detail/d3d9-vtable-guard.hxx>

#include <libd3d9/export.hxx>

namespace d3d9
{
  // Vtable manager for a single IDirect3DDevice9 instance.
  //
  // The general idea here is to intercept a fixed set of IDirect3DDevice9
  // methods. We do this by replacing the corresponding vtable slots with our
  // internal dispatch stubs. Each stub invokes all registered subscribers in
  // the exact order they subscribed, and then delegates the call to the
  // original implementation. Naturally, when our device manager is destroyed,
  // we restore all vtable slots to their original values via RAII.
  //
  // Intercepted methods:
  //
  //   Vtable index  Method                  Events dispatched
  //
  //    3            TestCooperativeLevel    device_lost, device_restored
  //   16            Reset                   pre_reset, post_reset
  //   17            Present                 present
  //   41            BeginScene              begin_scene
  //   42            EndScene                end_scene
  //
  // Usage:
  //
  //   d3d9::device d (pDevice);
  //
  //   auto t1 (d.on_end_scene ([] (IDirect3DDevice9& d) {
  //       // Draw overlay before the scene is finalised.
  //   }));
  //
  //   auto t2 (d.on_pre_reset ([] (IDirect3DDevice9& d,
  //                                D3DPRESENT_PARAMETERS&) {
  //       // Release D3DPOOL_DEFAULT resources.
  //   }));
  //
  //   auto t3 (d.on_post_reset ([] (IDirect3DDevice9& d,
  //                                 D3DPRESENT_PARAMETERS&,
  //                                 HRESULT hr) {
  //       if (SUCCEEDED (hr)) { /* Recreate resources. */ }
  //   }));
  //
  // Keep in mind a few constraints:
  //
  //   1. A device must not be managed by more than one manager at a time.
  //      Constructing a second manager for the exact same device throws
  //      std::logic_error.
  //   2. We make this class non-copyable and non-movable. The internal thunks
  //      hold a raw pointer to this instance and would become dangling after a
  //      move.
  //   3. Subscription_token objects must not outlive the device that issued
  //      them.
  //   4. You can safely call all on_*() methods from any thread.
  //   5. Event callbacks are invoked on whichever thread calls the
  //      corresponding D3D9 method, which is typically the render thread.
  //
  class LIBD3D9_SYMEXPORT device
  {
  public:
    // Install the vtable patches on the specified device.
    //
    // Note that the device pointer must not be null and the device must not
    // already be managed by another instance. We throw std::invalid_argument if
    // the device is null, std::logic_error if it is already managed, and
    // std::system_error if VirtualProtect fails during the patching process.
    //
    explicit
    device (IDirect3DDevice9* device);

    // Restore all vtable and unregister from the internal device table. Note
    // that all registered subscriptions will become inactive automatically.
    //
    ~device () noexcept;

    // Explicitly delete copy and move. The internal thunks hold this instance
    // by raw pointer, so moving or copying is invalid.
    //
    device (const device&) = delete;
    device& operator= (const device&) = delete;

    device (device&&) = delete;
    device& operator= (device&&) = delete;

    // Subscribe to the begin_scene event.
    //
    // We fire this event after IDirect3DDevice9::BeginScene() returns S_OK.
    //
    subscription_token
    on_begin_scene (
      event_traits<event_id::begin_scene>::callback_type callback);

    // Subscribe to the end_scene event.
    //
    // We fire this right before EndScene() delegates to the original function.
    // This is the ideal place for subscribers to issue additional draw calls,
    // since the device is still guaranteed to be in a valid scene context.
    //
    subscription_token
    on_end_scene (event_traits<event_id::end_scene>::callback_type callback);

    // Subscribe to the present event.
    //
    // Fired just before Present() delegates to the original implementation.
    //
    subscription_token
    on_present (event_traits<event_id::present>::callback_type callback);

    // Subscribe to the pre_reset event.
    //
    // Fired before Reset() delegates to the original. Subscribers must use this
    // opportunity to release all D3DPOOL_DEFAULT resources.
    //
    subscription_token
    on_pre_reset (event_traits<event_id::pre_reset>::callback_type callback);

    // Subscribe to the post_reset event.
    //
    // Fired immediately after Reset() returns from the original implementation.
    //
    subscription_token
    on_post_reset (event_traits<event_id::post_reset>::callback_type callback);

    // Subscribe to the device_lost event.
    //
    // Fired exactly once when TestCooperativeLevel() first detects device loss.
    //
    subscription_token
    on_device_lost (
      event_traits<event_id::device_lost>::callback_type callback);

    // Subscribe to the device_restored event.
    //
    // Fired exactly once when TestCooperativeLevel() first detects that the
    // device has successfully recovered.
    //
    subscription_token
    on_device_restored (
      event_traits<event_id::device_restored>::callback_type callback);

    // Query

    // Return the managed device pointer.
    //
    // Note that this is never null after successful construction.
    //
    IDirect3DDevice9*
    managed_device () const noexcept;

  private:
    template <event_id E>
    using dispatcher_t =
      detail::event_dispatcher<typename event_traits<E>::callback_type>;

    // Vtable thunks
    //
    // These static functions match the ABI of the corresponding
    // IDirect3DDevice9 vtable entries. The execution flow in each thunk goes
    // like this:
    //
    //   1. Look up our manager instance for the incoming device pointer.
    //   2. Dispatch pre-call subscribers, if we have any.
    //   3. Call the original function via the stored function pointer.
    //   4. Dispatch post-call subscribers.
    //   5. Finally, return the original HRESULT to the caller.
    //
    // We use STDMETHODCALLTYPE here because it expands to __stdcall on x86 and
    // is completely empty on x64, perfectly matching the COM vtable ABI on both
    // architectures.
    //
    static HRESULT STDMETHODCALLTYPE
    thunk_begin_scene (IDirect3DDevice9* device);

    static HRESULT STDMETHODCALLTYPE
    thunk_end_scene (IDirect3DDevice9* device);

    static HRESULT STDMETHODCALLTYPE
    thunk_present (IDirect3DDevice9* device,
                   const RECT* source_rect,
                   const RECT* dest_rect,
                   HWND dest_window,
                   const RGNDATA* dirty_region);

    static HRESULT STDMETHODCALLTYPE
    thunk_reset (IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp);

    static HRESULT STDMETHODCALLTYPE
    thunk_test_cooperative_level (IDirect3DDevice9* device);

    IDirect3DDevice9* device_;

    // Vtable guards.
    //
    // Each guard owns exactly one patched slot and restores it upon
    // destruction. We declare them in the exact order they are installed,
    // meaning they are destroyed in reverse declaration order according to
    // standard C++ rules.
    //
    detail::vtable_guard tcl_guard_;         // vtable[3]  TestCooperativeLevel
    detail::vtable_guard reset_guard_;       // vtable[16] Reset
    detail::vtable_guard present_guard_;     // vtable[17] Present
    detail::vtable_guard begin_scene_guard_; // vtable[41] BeginScene
    detail::vtable_guard end_scene_guard_;   // vtable[42] EndScene

    // Event dispatchers.
    //
    // We maintain one dispatcher per logical event rather than one per vtable
    // slot. This is because functions like Reset generate multiple distinct
    // events (pre_reset and post_reset).
    //
    dispatcher_t<event_id::begin_scene>     begin_scene_disp_;
    dispatcher_t<event_id::end_scene>       end_scene_disp_;
    dispatcher_t<event_id::present>         present_disp_;
    dispatcher_t<event_id::pre_reset>       pre_reset_disp_;
    dispatcher_t<event_id::post_reset>      post_reset_disp_;
    dispatcher_t<event_id::device_lost>     device_lost_disp_;
    dispatcher_t<event_id::device_restored> device_restored_disp_;

    // Device loss state tracking.
    //
    // We use this to ensure that device_lost and device_restored are fired
    // exactly once per state transition.
    //
    // Regarding access: this is written and read exclusively from the render
    // thread (inside our TestCooperativeLevel thunk), so no synchronization is
    // strictly required for correctness. We use std::atomic anyway just to
    // satisfy data-race requirements in case the caller decides to randomly
    // invoke TCL from a secondary thread.
    //
    std::atomic<bool> device_lost_state_;
  };

  inline IDirect3DDevice9*
  device::managed_device () const noexcept
  {
    return device_;
  }
}
