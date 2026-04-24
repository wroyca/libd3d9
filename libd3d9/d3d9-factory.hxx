#pragma once

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
  class LIBD3D9_SYMEXPORT factory
  {
  public:
    // Resolve Direct3DCreate9, spin up a dummy interface to get the vtable,
    // patch CreateDevice, and immediately release the dummy.
    //
    // Throws std::runtime_error if we can't find d3d9.dll or the entry point.
    // Also throws std::logic_error if another factory is already active, and
    // std::system_error if the memory protection fails during the patch.
    //
    factory ();

    // Clean up. Restores the original CreateDevice pointer and removes us from
    // the process-wide registry.
    //
    ~factory () noexcept;

    // Disable copying and moving. The internal thunk holds a raw pointer to
    // this instance, so moving it would leave a dangling pointer.
    //
    factory (const factory&) = delete;
    factory& operator= (const factory&) = delete;

    factory (factory&&) = delete;
    factory& operator= (factory&&) = delete;

    // Subscribe to the device creation event.
    //
    // We'll call this after every successful CreateDevice, but note that
    // subscriptions added while we are already dispatching won't see the
    // current event.
    //
    subscription_token
    on_device_created (
      event_traits<event_id::device_created>::callback_type callback);

  private:
    template <event_id E>
    using dispatcher_t =
      detail::event_dispatcher<typename event_traits<E>::callback_type>;

    // The actual vtable thunk for IDirect3D9::CreateDevice (slot 16).
    //
    // Standard COM ABI here: the first argument is the implicit this pointer.
    // If the call succeeds, we dispatch to all subscribers. Otherwise, we just
    // return the error code as-is.
    //
    static HRESULT STDMETHODCALLTYPE
    thunk_create_device (IDirect3D9* d3d,
                         UINT adapter,
                         D3DDEVTYPE device_type,
                         HWND focus_window,
                         DWORD behavior_flags,
                         D3DPRESENT_PARAMETERS* pp,
                         IDirect3DDevice9** out_device);

    // The base pointer of the IDirect3D9 vtable we patched.
    //
    // We cache it here so the destructor can unregister us without digging
    // it out of the guard's slot address.
    //
    void** vtable_;

    // Vtable guard for slot 16 (CreateDevice).
    //
    detail::vtable_guard create_device_guard_;

    // Dispatcher for the device_created event.
    //
    dispatcher_t<event_id::device_created> device_created_disp_;
  };
}
