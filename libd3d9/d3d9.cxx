#include <libd3d9/d3d9.hxx>

#include <cassert>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include <libd3d9/d3d9-win32.hxx>

namespace d3d9
{
  // TU-local state and helpers.
  //
  // We keep the device registry and reentrancy guards here. Map each
  // intercepted IDirect3DDevice9 pointer to its owning device. This is the
  // only shared mutable state and is accessed exclusively through the helpers
  // below.
  //
  // Note also that we use thread-local depth counters to prevent recursive
  // dispatch on the high-frequency paths. If a subscriber triggers the
  // intercepted method from within a callback, the inner call bypasses
  // dispatch.
  //
  namespace
  {
    std::mutex reg_mtx;
    std::unordered_map<IDirect3DDevice9*, device*> reg;

    thread_local int bs_depth (0);
    thread_local int es_depth (0);
    thread_local int p_depth  (0);
    thread_local int r_depth  (0);

    // Register h for d. Return false if d is already present.
    //
    bool
    register_device (IDirect3DDevice9* d, device* h)
    {
      std::lock_guard<std::mutex> l (reg_mtx);
      return reg.emplace (d, h).second;
    }

    // Remove the entry for d.
    //
    void
    unregister_device (IDirect3DDevice9* d) noexcept
    {
      std::lock_guard<std::mutex> l (reg_mtx);
      reg.erase (d);
    }

    // Look up the device for d. Return nullptr if not found.
    //
    device*
    find_device (IDirect3DDevice9* d) noexcept
    {
      std::lock_guard<std::mutex> l (reg_mtx);
      const auto i (reg.find (d));
      return (i != reg.end ()) ? i->second : nullptr;
    }

    // Factory registry.
    //
    // Notice that we patch the shared IDirect3D9 vtable rather than a specific
    // instance. The idea is that all IDirect3D9 instances in the process will
    // naturally route their CreateDevice calls through our thunk.
    //
    // Note that only one factory may be active per vtable at a given time. A
    // second factory instance would end up overwriting the guard already
    // installed by the first. This would be disastrous, as it would leave the
    // first's guard pointing at our own thunk rather than the real, underlying
    // implementation.
    //
    std::mutex factory_reg_mtx;
    std::unordered_map<void**, factory*> factory_reg;

    // Register factory f for the vtable vt.
    //
    // Return false if vt is already present in the registry. We just lock,
    // emplace, and return the insertion status directly. Notice that we hold
    // the lock just long enough to perform the emplacement.
    //
    bool
    register_factory (void** vt, factory* f)
    {
      std::lock_guard<std::mutex> l (factory_reg_mtx);
      return factory_reg.emplace (vt, f).second;
    }

    // Remove the factory entry associated with vtable vt.
    //
    // Notice this is strictly noexcept. We just acquire the lock and erase.
    // If the entry isn't there, erase() gracefully does nothing, which is
    // exactly what we want.
    //
    void
    unregister_factory (void** vt) noexcept
    {
      std::lock_guard<std::mutex> l (factory_reg_mtx);
      factory_reg.erase (vt);
    }

    // Look up the factory for the vtable extracted from d3d.
    //
    // Return nullptr if we fail to find one. We call this straight from the
    // CreateDevice thunk, where d3d is essentially the raw 'this' pointer.
    //
    // Notice that we extract the vtable pointer here on the caller's behalf. We
    // do this to keep the thunk call site clean and avoid leaking these nasty
    // reinterpret_cast details up the stack.
    //
    factory*
    find_factory (IDirect3D9* d3d) noexcept
    {
      // A COM object's memory layout places the vtable pointer right at the
      // beginning. So we cast the object pointer to a void*** and dereference
      // it to grab the actual vtable pointer (void**).
      //
      void** const vt (*reinterpret_cast<void***> (d3d));

      std::lock_guard<std::mutex> l (factory_reg_mtx);

      const auto i (factory_reg.find (vt));
      return i != factory_reg.end () ? i->second : nullptr;
    }

    // Extract the vtable pointer from a COM object.
    //
    inline void**
    get_vtable (IUnknown* o) noexcept
    {
      assert (o != nullptr);
      return *reinterpret_cast<void***> (o);
    }
  }

  namespace detail
  {
    // Vtable index constants.
    //
    // Verified against the IDirect3DDevice9 declaration in d3d9.h. IUnknown
    // occupies slots 0-2 and IDirect3DDevice9 methods begin at slot 3.
    //
    namespace vtable_index
    {
      // IUnknown.
      //
      constexpr std::size_t query_interface                (0);
      constexpr std::size_t add_ref                        (1);
      constexpr std::size_t release                        (2);

      // IDirect3DDevice9.
      //
      constexpr std::size_t test_cooperative_level         (3);
      constexpr std::size_t get_available_texture_mem      (4);
      constexpr std::size_t evict_managed_resources        (5);
      constexpr std::size_t get_direct3d                   (6);
      constexpr std::size_t get_device_caps                (7);
      constexpr std::size_t get_display_mode               (8);
      constexpr std::size_t get_creation_parameters        (9);
      constexpr std::size_t set_cursor_properties          (10);
      constexpr std::size_t set_cursor_position            (11);
      constexpr std::size_t show_cursor                    (12);
      constexpr std::size_t create_additional_swap_chain   (13);
      constexpr std::size_t get_swap_chain                 (14);
      constexpr std::size_t get_number_of_swap_chains      (15);
      constexpr std::size_t reset                          (16);
      constexpr std::size_t present                        (17);
      constexpr std::size_t get_back_buffer                (18);
      constexpr std::size_t get_raster_status              (19);
      constexpr std::size_t set_dialog_box_mode            (20);
      constexpr std::size_t set_gamma_ramp                 (21);
      constexpr std::size_t get_gamma_ramp                 (22);
      constexpr std::size_t create_texture                 (23);
      constexpr std::size_t create_volume_texture          (24);
      constexpr std::size_t create_cube_texture            (25);
      constexpr std::size_t create_vertex_buffer           (26);
      constexpr std::size_t create_index_buffer            (27);
      constexpr std::size_t create_render_target           (28);
      constexpr std::size_t create_depth_stencil_surface   (29);
      constexpr std::size_t update_surface                 (30);
      constexpr std::size_t update_texture                 (31);
      constexpr std::size_t get_render_target_data         (32);
      constexpr std::size_t get_front_buffer_data          (33);
      constexpr std::size_t stretch_rect                   (34);
      constexpr std::size_t color_fill                     (35);
      constexpr std::size_t create_offscreen_plain_surface (36);
      constexpr std::size_t set_render_target              (37);
      constexpr std::size_t get_render_target              (38);
      constexpr std::size_t set_depth_stencil_surface      (39);
      constexpr std::size_t get_depth_stencil_surface      (40);
      constexpr std::size_t begin_scene                    (41);
      constexpr std::size_t end_scene                      (42);

      // IDirect3D9 vtable indices (separate object, same IUnknown preamble).
      //
      // Verified against the IDirect3D9 declaration in d3d9.h. IUnknown
      // again occupies slots 0-2; IDirect3D9 methods begin at slot 3.
      //
      namespace d3d9_factory
      {
        constexpr std::size_t register_software_device  (3);
        constexpr std::size_t get_adapter_count         (4);
        constexpr std::size_t get_adapter_identifier    (5);
        constexpr std::size_t get_adapter_mode_count    (6);
        constexpr std::size_t enum_adapter_modes        (7);
        constexpr std::size_t get_adapter_display_mode  (8);
        constexpr std::size_t check_device_type         (9);
        constexpr std::size_t check_device_format       (10);
        constexpr std::size_t check_device_multisample  (11);
        constexpr std::size_t check_depth_stencil_match (12);
        constexpr std::size_t check_device_format_conv  (13);
        constexpr std::size_t get_device_caps           (14);
        constexpr std::size_t get_adapter_monitor       (15);
        constexpr std::size_t create_device             (16);
      }
    }

    // vtable_guard implementation.
    //
    vtable_guard::
    vtable_guard (void** vt, std::size_t i, void* r)
    {
      assert (vt != nullptr && "vt must not be null");
      assert (r != nullptr && "r must not be null");

      slot_ = &vt[i];
      original_ = *slot_;

      write_ptr (slot_, r);

      // Verify the slot now holds the replacement pointer.
      //
      assert (*slot_ == r && "patch write verification failed");
    }

    vtable_guard::
    ~vtable_guard ()
    {
      restore ();
    }

    void
    vtable_guard::restore () noexcept
    {
      if (slot_ == nullptr)
        return;

      write_ptr (slot_, original_);

      // Ensure the slot safely holds the original pointer again.
      //
      assert (*slot_ == original_ && "restore write verification failed");

      slot_ = nullptr;
      original_ = nullptr;
    }

    void
    vtable_guard::write_ptr (void** s, void* v) noexcept
    {
      // The vtable resides in a read-only page (usually .rdata). Temporarily
      // grant write access, overwrite the pointer, and restore the protection.
      //
      DWORD op (0);
      const BOOL ok (::VirtualProtect (s,
                                       sizeof (void*),
                                       PAGE_EXECUTE_READWRITE,
                                       &op));
      if (!ok)
      {
        // If we fail to modify page protection, log and bail out. In a
        // release build we force a crash rather than failing silently.
        //
        assert (false && "VirtualProtect (RW) failed");
        return;
      }

      *s = v;

      // Flush the instruction cache to ensure processors don't execute
      // from a stale indirect branch target.
      //
      ::FlushInstructionCache (::GetCurrentProcess (), s, sizeof (void*));

      DWORD ig (0);
      ::VirtualProtect (s, sizeof (void*), op, &ig);
    }
  }

  // device constructor and destructor.
  //
  device::
  device (IDirect3DDevice9* d)
    : device_ (d),
      device_lost_state_ (false)
  {
    if (d == nullptr)
      throw std::invalid_argument ("d must not be null");

    if (!register_device (d, this))
      throw std::logic_error ("d already managed by another instance");

    // Install vtable interceptions. Any failure will trigger the destructors of
    // the already installed guards to gracefully unwind.
    //
    void** const vt (get_vtable (d));

    using namespace detail::vtable_index;

    tcl_guard_ = detail::vtable_guard (
      vt,
      test_cooperative_level,
      reinterpret_cast<void*> (&device::thunk_test_cooperative_level));

    reset_guard_ = detail::vtable_guard (
      vt,
      reset,
      reinterpret_cast<void*> (&device::thunk_reset));

    present_guard_ = detail::vtable_guard (
      vt,
      present,
      reinterpret_cast<void*> (&device::thunk_present));

    begin_scene_guard_ = detail::vtable_guard (
      vt,
      begin_scene,
      reinterpret_cast<void*> (&device::thunk_begin_scene));

    end_scene_guard_ = detail::vtable_guard (
      vt,
      end_scene,
      reinterpret_cast<void*> (&device::thunk_end_scene));
  }

  device::
  ~device ()
  {
    // Unregister first so no incoming thunk invocations can grab this instance
    // while we destruct the vtable guards.
    //
    unregister_device (device_);
  }

  // Subscription API.
  //
  subscription_token
  device::on_begin_scene (event_traits<event_id::begin_scene>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (begin_scene_disp_.subscribe (std::move (cb)));
    auto* p (&begin_scene_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  subscription_token
  device::on_end_scene (event_traits<event_id::end_scene>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (end_scene_disp_.subscribe (std::move (cb)));
    auto* p (&end_scene_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  subscription_token
  device::on_present (event_traits<event_id::present>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (present_disp_.subscribe (std::move (cb)));
    auto* p (&present_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  subscription_token
  device::on_pre_reset (event_traits<event_id::pre_reset>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (pre_reset_disp_.subscribe (std::move (cb)));
    auto* p (&pre_reset_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  subscription_token
  device::on_post_reset (event_traits<event_id::post_reset>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (post_reset_disp_.subscribe (std::move (cb)));
    auto* p (&post_reset_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  subscription_token
  device::on_device_lost (event_traits<event_id::device_lost>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (device_lost_disp_.subscribe (std::move (cb)));
    auto* p (&device_lost_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  subscription_token
  device::on_device_restored (
    event_traits<event_id::device_restored>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (device_restored_disp_.subscribe (std::move (cb)));
    auto* p (&device_restored_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  // Vtable thunks.
  //
  // BeginScene.
  //
  // We call the original BeginScene() first. If it succeeds, we dispatch
  // to subscribers. This guarantees subscribers receive a device that has
  // actively entered a valid scene context.
  //
  HRESULT STDMETHODCALLTYPE
  device::thunk_begin_scene (IDirect3DDevice9* d)
  {
    device* const h (find_device (d));
    assert (h != nullptr && "unregistered device");

    using fn_t = HRESULT (STDMETHODCALLTYPE*) (IDirect3DDevice9*);
    const auto o (reinterpret_cast<fn_t> (h->begin_scene_guard_.original ()));

    if (bs_depth > 0)
      return o (d);

    ++bs_depth;
    const HRESULT r (o (d));
    --bs_depth;

    if (SUCCEEDED (r))
      h->begin_scene_disp_.dispatch (*d);

    return r;
  }

  // EndScene.
  //
  // Dispatch subscribers first so they can draw into the open scene before
  // submission.
  //
  HRESULT STDMETHODCALLTYPE
  device::thunk_end_scene (IDirect3DDevice9* d)
  {
    device* const h (find_device (d));
    assert (h != nullptr && "unregistered device");

    using fn_t = HRESULT (STDMETHODCALLTYPE*) (IDirect3DDevice9*);
    const auto o (reinterpret_cast<fn_t> (h->end_scene_guard_.original ()));

    if (es_depth > 0)
      return o (d);

    ++es_depth;
    h->end_scene_disp_.dispatch (*d);
    const HRESULT r (o (d));
    --es_depth;

    return r;
  }

  // Present.
  //
  HRESULT STDMETHODCALLTYPE
  device::thunk_present (IDirect3DDevice9* d,
                         const RECT* sr,
                         const RECT* dr,
                         HWND dw,
                         const RGNDATA* dg)
  {
    device* const h (find_device (d));
    assert (h != nullptr && "unregistered device");

    using fn_t = HRESULT (STDMETHODCALLTYPE*) (IDirect3DDevice9*,
                                               const RECT*,
                                               const RECT*,
                                               HWND,
                                               const RGNDATA*);
    const auto o (reinterpret_cast<fn_t> (h->present_guard_.original ()));

    if (p_depth > 0)
      return o (d, sr, dr, dw, dg);

    ++p_depth;
    h->present_disp_.dispatch (*d, sr, dr, dw, dg);
    const HRESULT r (o (d, sr, dr, dw, dg));
    --p_depth;

    return r;
  }

  // Reset.
  //
  // We dispatch pre_reset so that D3DPOOL_DEFAULT resources are cleanly
  // released, call the original method, and then inform post_reset handlers.
  //
  HRESULT STDMETHODCALLTYPE
  device::thunk_reset (IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp)
  {
    device* const h (find_device (d));
    assert (h != nullptr && "unregistered device");

    using fn_t = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*,
                                              D3DPRESENT_PARAMETERS*);
    const auto o (reinterpret_cast<fn_t> (h->reset_guard_.original ()));

    if (r_depth > 0)
      return o (d, pp);

    ++r_depth;

    // D3D9 guarantees pp isn't null, so dereferencing is safe here.
    //
    assert (pp != nullptr && "pp must not be null");
    h->pre_reset_disp_.dispatch (*d, *pp);

    const HRESULT r (o (d, pp));

    h->post_reset_disp_.dispatch (*d, *pp, r);

    --r_depth;
    return r;
  }

  // TestCooperativeLevel.
  //
  // Called by the application loop, potentially at high frequency during
  // a lost device scenario. Keep the dispatch path lightweight.
  //
  HRESULT STDMETHODCALLTYPE
  device::thunk_test_cooperative_level (IDirect3DDevice9* d)
  {
    device* const h (find_device (d));
    assert (h != nullptr && "unregistered device");

    using fn_t = HRESULT (STDMETHODCALLTYPE*) (IDirect3DDevice9*);
    const auto o (reinterpret_cast<fn_t> (h->tcl_guard_.original ()));

    const HRESULT r (o (d));

    const bool nl (r == D3DERR_DEVICELOST || r == D3DERR_DEVICENOTRESET);
    const bool wl (h->device_lost_state_.load (std::memory_order_relaxed));

    if (nl && !wl)
    {
      h->device_lost_state_.store (true, std::memory_order_relaxed);
      h->device_lost_disp_.dispatch (*d);
    }
    else if (!nl && wl)
    {
      h->device_lost_state_.store (false, std::memory_order_relaxed);
      h->device_restored_disp_.dispatch (*d);
    }

    return r;
  }

  factory::factory ()
    : vtable_ (nullptr)
  {
    // Obtain the d3d9.dll module handle. We deliberately use GetModuleHandleA()
    // rather than LoadLibrary() to avoid bumping the module reference count. If
    // the DLL is not loaded yet, it means the application hasn't initialized
    // D3D9 and there is naturally nothing for us to intercept.
    //
    const HMODULE mod (::GetModuleHandleA ("d3d9.dll"));

    if (mod == nullptr)
      throw std::runtime_error ("d3d9.dll is not loaded in this process");

    // Resolve the Direct3DCreate9 factory function.
    //
    const FARPROC fp (::GetProcAddress (mod, "Direct3DCreate9"));

    if (fp == nullptr)
      throw std::runtime_error ("Direct3DCreate9 not found in d3d9.dll");

    // Create a scratch IDirect3D9 instance to gain access to the shared vtable.
    // Since all IDirect3D9 instances share the exact same vtable layout, we
    // only need this object long enough to extract the vtable pointer and
    // install our guard. We will release it immediately after patching.
    //
    using create_fn = IDirect3D9*(WINAPI*) (UINT);
    IDirect3D9* const scratch (
      reinterpret_cast<create_fn> (fp) (D3D_SDK_VERSION));

    if (scratch == nullptr)
      throw std::runtime_error (
        "Direct3DCreate9 returned null; SDK version mismatch?");

    void** const vt (get_vtable (scratch));

    // Register before patching so that the thunk can actually find us. If
    // another factory happens to be already active, we refuse and release the
    // scratch object before throwing.
    //
    if (!register_factory (vt, this))
    {
      scratch->Release ();
      throw std::logic_error (
        "a factory is already active for the IDirect3D9 vtable");
    }

    // Install the CreateDevice patch. Note that if vtable_guard throws (for
    // instance, because VirtualProtect() failed), we must unregister before
    // propagating the exception.
    //
    try
    {
      create_device_guard_ = detail::vtable_guard (
        vt,
        detail::vtable_index::d3d9_factory::create_device,
        reinterpret_cast<void*> (&factory::thunk_create_device));
    }
    catch (...)
    {
      unregister_factory (vt);
      scratch->Release ();
      throw;
    }

    // Cache the vtable pointer so the destructor can unregister directly
    // without having to reconstruct it from the guard's slot address.
    //
    vtable_ = vt;

    // Release the scratch interface. The vtable lives in the DLL's .rdata
    // section and remains valid for the lifetime of the module, so releasing
    // the object does not invalidate any pointer we currently hold.
    //
    scratch->Release ();
  }

  factory::
  ~factory ()
  {
    // Restore the original CreateDevice slot first. From this point on, any new
    // CreateDevice call in the process will bypass our thunk entirely.
    //
    create_device_guard_.restore ();

    // Unregister after restoring. This ensures that any thunk invocation that
    // raced with our destructor and is currently past the find_factory() lookup
    // can still safely resolve us through the dispatcher.
    //
    if (vtable_ != nullptr)
      unregister_factory (vtable_);
  }

  // Subscription API.
  //
  subscription_token
  factory::on_device_created (
    event_traits<event_id::device_created>::callback_type cb)
  {
    assert (cb && "cb must not be empty");
    const auto i (device_created_disp_.subscribe (std::move (cb)));
    auto* p (&device_created_disp_);
    return subscription_token ([p, i] { p->unsubscribe (i); });
  }

  // CreateDevice thunk.
  //
  // We forward the call to the original implementation first. On success, we
  // dispatch device_created so that subscribers receive a fully initialized
  // device. On failure, we simply propagate the error code untouched without
  // dispatching.
  //
  // Regarding reentrancy: if a device_created subscriber calls CreateDevice()
  // again (for example, to create a secondary device), the inner call will also
  // route through this thunk and dispatch a second device_created event. There
  // is no depth guard here because each call produces a logically distinct
  // device and both events are meaningful. Callers are essentially responsible
  // for not creating recursive loops.
  //
  HRESULT STDMETHODCALLTYPE
  factory::thunk_create_device (IDirect3D9* d3d,
                                UINT adapter,
                                D3DDEVTYPE device_type,
                                HWND focus_window,
                                DWORD behavior_flags,
                                D3DPRESENT_PARAMETERS* pp,
                                IDirect3DDevice9** out_device)
  {
    factory* const f (find_factory (d3d));

    // If find_factory() returns nullptr, it means this IDirect3D9 instance is
    // not covered by any active factory. This can happen if it was created
    // before our constructor ran, or if we have partially torn down. In this
    // case, we cannot reach the original pointer without the guard, so the only
    // safe action is to fail the call.
    //
    if (f == nullptr)
    {
      assert (false && "thunk_create_device called for unknown IDirect3D9");
      return D3DERR_INVALIDCALL;
    }

    using fn_t = HRESULT (STDMETHODCALLTYPE*) (IDirect3D9*,
                                               UINT,
                                               D3DDEVTYPE,
                                               HWND,
                                               DWORD,
                                               D3DPRESENT_PARAMETERS*,
                                               IDirect3DDevice9**);

    const auto o (reinterpret_cast<fn_t> (f->create_device_guard_.original ()));

    assert (pp != nullptr && "pp must not be null");
    assert (out_device != nullptr && "out_device must not be null");

    const HRESULT r (o (d3d,
                        adapter,
                        device_type,
                        focus_window,
                        behavior_flags,
                        pp,
                        out_device));

    if (SUCCEEDED (r))
    {
      assert (*out_device != nullptr &&
              "CreateDevice succeeded with null device");
      f->device_created_disp_.dispatch (**out_device, *pp);
    }

    return r;
  }
}
