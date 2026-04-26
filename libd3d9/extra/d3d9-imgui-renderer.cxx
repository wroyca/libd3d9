#include <libd3d9/extra/d3d9-imgui-renderer.hxx>

#include <cassert>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include <libimgui-docking/imgui.h>
#include <libimgui-docking/backends/imgui_impl_dx9.h>
#include <libimgui-docking/backends/imgui_impl_win32.h>

#include <libd3d9/d3d9-win32.hxx>

// Forward-declare ImGui_ImplWin32_WndProcHandler which is originally provided
// in imgui_impl_win32.h. Do this to make the dependency explicit at the call
// site and, rather nicely, suppress any "declared but not used" warnings if the
// header pulls in other implementation details.
//
extern LRESULT
ImGui_ImplWin32_WndProcHandler (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace d3d9
{
  namespace extra
  {
    // Translation unit local state and helpers.
    //
    // Maintain a per-HWND renderer registry. This mirrors the pattern used by
    // the device registry in the main translation unit. Key this map by HWND
    // simply because it is the only stable identity we have inside the static
    // WndProc thunk.
    //
    // Notice that only one imgui_renderer can be active per window at any given
    // time. Because we are subclassing the window and taking ownership of its
    // Raw Input registration, attempting a second construction on the same HWND
    // is a logic error and is refused.
    //
    namespace
    {
      std::mutex                                wndproc_reg_mtx;
      std::unordered_map<HWND, imgui_renderer*> wndproc_reg;

      // Register the renderer for a specific window. Return false if this
      // window is already present in the registry.
      //
      bool
      register_renderer (HWND hwnd, imgui_renderer* r)
      {
        std::lock_guard<std::mutex> l (wndproc_reg_mtx);
        return wndproc_reg.emplace (hwnd, r).second;
      }

      // Remove the registry entry for the given window.
      //
      void
      unregister_renderer (HWND hwnd) noexcept
      {
        std::lock_guard<std::mutex> l (wndproc_reg_mtx);
        wndproc_reg.erase (hwnd);
      }

      // Look up the active renderer for the given window. Return a null pointer
      // if nothing is found.
      //
      imgui_renderer*
      find_renderer (HWND hwnd) noexcept
      {
        std::lock_guard<std::mutex> l (wndproc_reg_mtx);
        const auto i (wndproc_reg.find (hwnd));
        return (i != wndproc_reg.end ()) ? i->second : nullptr;
      }
    }

    imgui_renderer::
    imgui_renderer (d3d9::device& dev)
      : device_               (dev),
        window_               (nullptr),
        original_wnd_proc_    (nullptr),
        context_              (nullptr),
        owns_context_         (false)
    {
      // Derive the target window from the device creation parameters. The focus
      // window is the HWND that D3D9 inherently uses for cursor and cooperative
      // level tracking, which makes it the correct target for our input
      // subclass.
      //
      D3DDEVICE_CREATION_PARAMETERS cp {};
      const HRESULT hr (dev.managed_device ()->GetCreationParameters (&cp));

      if (FAILED (hr))
        throw std::runtime_error (
          "IDirect3DDevice9::GetCreationParameters failed");

      if (cp.hFocusWindow == nullptr)
        throw std::runtime_error (
          "device reports a null focus window; "
          "supply an explicit HWND via the two-argument constructor");

      window_ = cp.hFocusWindow;

      init (dev.managed_device ());
    }

    imgui_renderer::
    imgui_renderer (d3d9::device& dev, HWND window)
      : device_               (dev),
        window_               (window),
        original_wnd_proc_    (nullptr),
        context_              (nullptr),
        owns_context_         (false)
    {
      if (window == nullptr)
        throw std::invalid_argument ("window must not be null");

      init (dev.managed_device ());
    }

    // Shared initialization routine.
    //
    // Both constructors call this once the target window is established. Notice
    // how the sequence is carefully ordered so that every acquired resource has
    // a matching release in the destructor. Any failure path cleans up exactly
    // what has been acquired up to that point.
    //
    void
    imgui_renderer::init (IDirect3DDevice9* dev_ptr)
    {
      // ImGui context.
      //
      // First, adopt an existing ImGui context if one is already current. This
      // gives the caller a chance to configure flags before constructing the
      // renderer, without us clobbering their careful setup.
      //
      owns_context_ = (ImGui::GetCurrentContext () == nullptr);

      if (owns_context_)
        context_ = ImGui::CreateContext ();

      ImGuiIO& io (ImGui::GetIO ());

      // D3D9 rendering backend.
      //
      if (!ImGui_ImplDX9_Init (dev_ptr))
      {
        if (owns_context_)
          ImGui::DestroyContext (context_);

        throw std::runtime_error ("ImGui_ImplDX9_Init failed");
      }

      // Win32 platform backend.
      //
      // Rely on ImGui_ImplWin32 for display size, delta time, DPI scaling, and
      // standard cursor and mouse management.
      //
      if (!ImGui_ImplWin32_Init (window_))
      {
        ImGui_ImplDX9_Shutdown ();

        if (owns_context_)
          ImGui::DestroyContext (context_);

        throw std::runtime_error ("ImGui_ImplWin32_Init failed");
      }

      // HWND registry.
      //
      // Register before subclassing so that the WndProc thunk can look us up
      // the instant the very first message arrives after the SetWindowLongPtrW
      // call.
      //
      if (!register_renderer (window_, this))
      {
        ImGui_ImplWin32_Shutdown ();
        ImGui_ImplDX9_Shutdown ();

        if (owns_context_)
          ImGui::DestroyContext (context_);

        throw std::logic_error (
          "an imgui_renderer is already active for this window");
      }

      // Window subclassing.
      //
      // Replace the window procedure with our static thunk. Stash the previous
      // procedure so we can blindly forward messages and restore the chain on
      // destruction. Since SetWindowLongPtrW returns 0 on failure but also sets
      // the last error, check GetLastError to distinguish a genuine zero-return
      // from a error.
      //
      ::SetLastError (0);

      original_wnd_proc_ = reinterpret_cast<WNDPROC> (
        ::SetWindowLongPtrW (
          window_,
          GWLP_WNDPROC,
          reinterpret_cast<LONG_PTR> (&imgui_renderer::thunk_wnd_proc)));

      if (original_wnd_proc_ == nullptr && ::GetLastError () != 0)
      {
        const DWORD err (::GetLastError ());

        unregister_renderer (window_);

        ImGui_ImplWin32_Shutdown ();
        ImGui_ImplDX9_Shutdown ();

        if (owns_context_)
          ImGui::DestroyContext (context_);

        throw std::system_error (
          std::error_code (static_cast<int> (err), std::system_category ()),
          "SetWindowLongPtrW failed");
      }

      // Device event subscriptions.
      //
      end_scene_token_ = device_.on_end_scene (
        [this] (IDirect3DDevice9& d) { on_end_scene_impl (d); });

      // ImGui holds GPU resources (vertex buffers, font textures, etc) in
      // D3DPOOL_DEFAULT. These absolutely must be released prior to Reset() or
      // the call will stubbornly fail with D3DERR_INVALIDCALL.
      //
      pre_reset_token_ = device_.on_pre_reset (
        [this] (IDirect3DDevice9&, D3DPRESENT_PARAMETERS&)
        {
          ImGui_ImplDX9_InvalidateDeviceObjects ();
        });

      // Recreate the GPU resources immediately if the reset succeeds.
      //
      post_reset_token_ = device_.on_post_reset (
        [this] (IDirect3DDevice9&, D3DPRESENT_PARAMETERS&, HRESULT r)
        {
          if (SUCCEEDED (r))
            ImGui_ImplDX9_CreateDeviceObjects ();
        });
    }

    // Destructor.
    //
    // Teardown is the strict reverse of the initialization order. There is one
    // vital exception: cancel the device subscriptions first to guarantee no
    // ImGui call can suddenly fire from the render thread while we are halfway
    // through tearing down the backends.
    //
    imgui_renderer::
    ~imgui_renderer ()
    {
      // Cancel render-thread callbacks first.
      //
      // Resetting the tokens triggers dispatcher::unsubscribe, which acquires
      // the dispatcher's mutex. This is thread-safe from the destructor because
      // the render thread only holds the mutex transiently during the
      // subscriber snapshot, never across the callback execution itself.
      //
      end_scene_token_.reset ();
      pre_reset_token_.reset ();
      post_reset_token_.reset ();

      // Restore the original WndProc.
      //
      // Purposefully restore the window procedure before unregistering from the
      // HWND table. Any rogue message arriving in the narrow time slice between
      // the two operations is cleanly forwarded through the original chain
      // rather than catastrophically hitting DefWindowProcW.
      //
      if (original_wnd_proc_ != nullptr)
      {
        ::SetWindowLongPtrW (
          window_,
          GWLP_WNDPROC,
          reinterpret_cast<LONG_PTR> (original_wnd_proc_));
      }

      unregister_renderer (window_);

      ImGui_ImplWin32_Shutdown ();
      ImGui_ImplDX9_Shutdown ();

      if (owns_context_)
        ImGui::DestroyContext (context_);
    }

    // Subscription API.
    //
    subscription_token
    imgui_renderer::on_pre_frame (std::function<void (IDirect3DDevice9&)> cb)
    {
      assert (cb && "cb must not be empty");

      const auto i (pre_frame_disp_.subscribe (std::move (cb)));
      auto* p (&pre_frame_disp_);

      return subscription_token::make ([p, i] { p->unsubscribe (i); });
    }

    subscription_token
    imgui_renderer::on_frame (std::function<void ()> cb)
    {
      assert (cb && "cb must not be empty");

      const auto i (frame_disp_.subscribe (std::move (cb)));
      auto* p (&frame_disp_);

      return subscription_token::make ([p, i] { p->unsubscribe (i); });
    }

    // Message gate API.
    //
    // Note that we expect the caller to pass a valid, non-empty callable. If
    // the intention is to disable or remove the gate, they should explicitly
    // call clear_message_gate() instead. This keeps the semantics clear and
    // saves us from checking for or invoking an empty wrapper later in the hot
    // path of the render loop.
    //
    void
    imgui_renderer::set_message_gate (gate_fn gate)
    {
      assert (gate && "gate must not be empty");

      // Serialize access to the gate callable. The render loop might be
      // concurrently trying to read or execute the current gate, so we need to
      // ensure this replacement is thread-safe and atomic relative to the
      // invocation.
      //
      std::lock_guard<std::mutex> l (gate_mtx_);
      gate_ = std::move (gate);
    }

    // Clear the currently active message gate.
    //
    // We mark this as noexcept since assigning nullptr (which destroys the
    // underlying type-erased state) and releasing a mutex are non-throwing
    // operations. If either of these somehow throws, the runtime is likely in
    // an unrecoverable state anyway.
    //
    void
    imgui_renderer::clear_message_gate () noexcept
    {
      // Again, serialize with the render thread before clearing.
      //
      std::lock_guard<std::mutex> l (gate_mtx_);
      gate_ = nullptr;
    }

    // EndScene event handler. This is the main ImGui frame driver.
    //
    // Explicitly invoked by the d3d9::device EndScene thunk before it
    // triggers the original EndScene. Because the render target remains
    // open here, ImGui draw calls are perfectly valid.
    //
    // Frame sequence:
    //
    //   1. ImGui_ImplDX9_NewFrame       - Updates vertex/index buffers if
    //                                     needed.
    //   2. ImGui_ImplWin32_NewFrame     - Updates io.DisplaySize, io.DeltaTime,
    //                                     and syncs the cursor position from
    //                                     GetCursorPos once per frame.
    //   3. ImGui::NewFrame              - Begins the ImGui command list.
    //   4. frame_disp_.dispatch         - Invokes all on_frame subscribers.
    //   5. ImGui::Render                - Finalizes the draw list.
    //   6. ImGui_ImplDX9_RenderDrawData - Issues actual D3D9 draw calls.
    //
    void
    imgui_renderer::on_end_scene_impl (IDirect3DDevice9& d)
    {
      pre_frame_disp_.dispatch (d);

      ImGui_ImplDX9_NewFrame ();
      ImGui_ImplWin32_NewFrame ();
      ImGui::NewFrame ();

      frame_disp_.dispatch ();

      ImGui::Render ();
      ImGui_ImplDX9_RenderDrawData (ImGui::GetDrawData ());
    }

    // WndProc thunk.
    //
    // This is the replacement window procedure injected by
    // SetWindowLongPtrW. It routes each message to the
    // relevant ImGui subsystem, evaluates the optional message gate, and
    // then typically tails through to the original procedure.
    //
    LRESULT CALLBACK
    imgui_renderer::thunk_wnd_proc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      imgui_renderer* const r (find_renderer (hwnd));

      // This pointer should never be null during normal operation. The sole
      // edge case is a stray message arriving in the split-second between
      // SetWindowLongPtrW executing and the renderer being fully unregistered
      // during teardown. Failing forward to DefWindowProcW is the safest, least
      // surprising recovery.
      //
      if (r == nullptr)
        return ::DefWindowProcW (hwnd, msg, wp, lp);

      // Delegate to the ImGui Win32 backend. Notice that ImGui ImplWin32
      // WndProcHandler only returns non-zero for WM_SETCURSOR (to prevent
      // flickering). Purposefully do not gate CallWindowProcW on this return
      // value. That is, we must never consume messages from the host
      // application's event chain blindly.
      //
      ImGui_ImplWin32_WndProcHandler (hwnd, msg, wp, lp);

      // Evaluate the message gate.
      //
      // If the gate is installed and returns false, we suppress the message
      // from reaching the original window procedure and return DefWindowProcW
      // instead.
      //
      bool forward (true);
      {
        std::lock_guard<std::mutex> l (r->gate_mtx_);
        if (r->gate_)
          forward = r->gate_ (msg, wp, lp);
      }

      if (!forward)
        return ::DefWindowProcW (hwnd, msg, wp, lp);

      return ::CallWindowProcW (r->original_wnd_proc_, hwnd, msg, wp, lp);
    }
  }
}
