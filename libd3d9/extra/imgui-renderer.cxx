#include <libd3d9/extra/imgui-renderer.hxx>

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
        owns_context_         (false),
        raw_input_registered_ (false),
        mouse_x_              (0.0f),
        mouse_y_              (0.0f)
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
        owns_context_         (false),
        raw_input_registered_ (false),
        mouse_x_              (0.0f),
        mouse_y_              (0.0f)
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
      // standard cursor management. Specifically, do not rely on
      // ImGui_ImplWin32_WndProcHandler for mouse movement. That is handled
      // out-of-band through Raw Input instead.
      //
      if (!ImGui_ImplWin32_Init (window_))
      {
        ImGui_ImplDX9_Shutdown ();

        if (owns_context_)
          ImGui::DestroyContext (context_);

        throw std::runtime_error ("ImGui_ImplWin32_Init failed");
      }

      // Raw Input device registration.
      //
      // Request high-frequency mouse reports (HID usage page 0x01, usage
      // 0x02) targeted precisely at our window. Leave dwFlags as 0 (no
      // RIDEV_NOLEGACY, no RIDEV_INPUTSINK) so we only capture while
      // focused. and leave the legacy WM_MOUSEMOVE stream completely intact
      // for the host application to consume.
      //
      // If registration fails, fall back to the WM_MOUSEMOVE path.
      //
      RAWINPUTDEVICE rid {};
      rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
      rid.usUsage     = 0x02; // HID_USAGE_GENERIC_MOUSE
      rid.dwFlags     = 0;
      rid.hwndTarget  = window_;

      raw_input_registered_ =
        (::RegisterRawInputDevices (&rid, 1, sizeof (rid)) == TRUE);

      // assert (raw_input_registered_ && "RegisterRawInputDevices failed. ");

      // HWND registry.
      //
      // Register before subclassing so that the WndProc thunk can look us up
      // the instant the very first message arrives after the SetWindowLongPtrW
      // call.
      //
      if (!register_renderer (window_, this))
      {
        if (raw_input_registered_)
        {
          RAWINPUTDEVICE remove {};
          remove.usUsagePage = 0x01;
          remove.usUsage     = 0x02;
          remove.dwFlags     = RIDEV_REMOVE;
          remove.hwndTarget  = nullptr;
          ::RegisterRawInputDevices (&remove, 1, sizeof (remove));
        }

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

        if (raw_input_registered_)
        {
          RAWINPUTDEVICE remove {};
          remove.usUsagePage = 0x01;
          remove.usUsage     = 0x02;
          remove.dwFlags     = RIDEV_REMOVE;
          remove.hwndTarget  = nullptr;
          ::RegisterRawInputDevices (&remove, 1, sizeof (remove));
        }

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

      if (raw_input_registered_)
      {
        RAWINPUTDEVICE remove {};
        remove.usUsagePage = 0x01;
        remove.usUsage     = 0x02;
        remove.dwFlags     = RIDEV_REMOVE;
        remove.hwndTarget  = nullptr;
        ::RegisterRawInputDevices (&remove, 1, sizeof (remove));
      }

      ImGui_ImplWin32_Shutdown ();
      ImGui_ImplDX9_Shutdown ();

      if (owns_context_)
        ImGui::DestroyContext (context_);
    }

    // Subscription API.
    //
    subscription_token
    imgui_renderer::on_frame (std::function<void ()> cb)
    {
      assert (cb && "cb must not be empty");

      const auto i (frame_disp_.subscribe (std::move (cb)));
      auto* p (&frame_disp_);

      return subscription_token::make ([p, i] { p->unsubscribe (i); });
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
    // Note on mouse position accuracy: WM_INPUT events naturally accumulate
    // into mouse_x_ and mouse_y_ throughout the message-pump phase.
    // ImGui_ImplWin32_NewFrame synchronizes against GetCursorPos, reflecting
    // the true cursor at frame time regardless of accumulated deltas. This
    // yields sub-frame precision during rapid movement without exposing that
    // raw, jittery precision directly to the display pipeline. It is the
    // optimal trade-off for an overlay.
    //
    void
    imgui_renderer::on_end_scene_impl (IDirect3DDevice9&)
    {
      ImGui_ImplDX9_NewFrame ();
      ImGui_ImplWin32_NewFrame ();
      ImGui::NewFrame ();

      frame_disp_.dispatch ();

      ImGui::Render ();
      ImGui_ImplDX9_RenderDrawData (ImGui::GetDrawData ());
    }

    // Raw Input mouse handler.
    //
    // This is called from thunk_wnd_proc for every WM_INPUT message that bears
    // a mouse report. Here we accumulate the client-space position and then
    // forward it to the ImGui event queue.
    //
    void
    imgui_renderer::feed_raw_mouse (const RAWMOUSE& rm) noexcept
    {
      ImGuiIO& io (ImGui::GetIO ());

      if ((rm.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
      {
        // Handle absolute coordinates. These are typically generated by sources
        // like Remote Desktop, Citrix, and various graphics tablets. Note that
        // the values arrive normalized to the [0, 65535] range.
        //
        const bool vd ((rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0);

        const int sw (
          ::GetSystemMetrics (vd ? SM_CXVIRTUALSCREEN : SM_CXSCREEN));
        const int sh (
          ::GetSystemMetrics (vd ? SM_CYVIRTUALSCREEN : SM_CYSCREEN));

        POINT p;
        p.x = static_cast<LONG> ((rm.lLastX / 65535.0f) * sw);
        p.y = static_cast<LONG> ((rm.lLastY / 65535.0f) * sh);

        ::ScreenToClient (window_, &p);

        mouse_x_ = static_cast<float> (p.x);
        mouse_y_ = static_cast<float> (p.y);
      }
      else if (rm.lLastX != 0 || rm.lLastY != 0)
      {
        // Handle relative movement, which represents the common case for gaming
        // mice.
        //
        // We continuously accumulate the delta. Because
        // ImGui_ImplWin32_NewFrame enforces synchronization from GetCursorPos
        // at the start of each frame, any subtle drift between WM_INPUT reports
        // cannot possibly compound beyond a single frame's worth of error.
        //
        mouse_x_ += static_cast<float> (rm.lLastX);
        mouse_y_ += static_cast<float> (rm.lLastY);

        // Clamp tightly to the client area. Severe ballistic acceleration near
        // the edge of the window can push the accumulated position far outside
        // the client rect, which in turn causes ImGui to hallucinate phantom
        // out-of-window coordinates.
        //
        RECT cr {};
        ::GetClientRect (window_, &cr);

        if (mouse_x_ < 0.0f)
          mouse_x_ = 0.0f;
        else if (mouse_x_ > static_cast<float> (cr.right))
          mouse_x_ = static_cast<float> (cr.right);

        if (mouse_y_ < 0.0f)
          mouse_y_ = 0.0f;
        else if (mouse_y_ > static_cast<float> (cr.bottom))
          mouse_y_ = static_cast<float> (cr.bottom);
      }
      else
      {
        // If lLastX == 0 and lLastY == 0 with MOUSE_MOVE_RELATIVE, it implies
        // the report carries strictly button or wheel data with zero movement.
        // We skip the position update entirely to prevent stomping the ImGui
        // state with a useless no-op.
        //
        return;
      }

      io.AddMousePosEvent (mouse_x_, mouse_y_);
    }

    // WndProc thunk.
    //
    // This is the replacement window procedure injected by
    // SetWindowLongPtrW. It routes each message to the
    // relevant ImGui subsystem and then always tails through to the
    // original procedure.
    //
    // Message routing:
    //
    //   WM_INPUT (mouse)
    //     Handled entirely by feed_raw_mouse. Do not forward this to
    //     ImGui_ImplWin32_WndProcHandler to avoid duplicate processing.
    //
    //   WM_MOUSEMOVE
    //     Aggressively suppressed from ImGui. The mouse position already
    //     arrives through WM_INPUT with far superior temporal precision.
    //     Suppressing it here prevents ImGui_ImplWin32_WndProcHandler
    //     from overwriting our carefully accumulated sub-frame position
    //     with a potentially coalesced, lower-frequency variant. The
    //     original WndProc naturally still receives the message.
    //
    //   All other messages
    //     Forwarded faithfully to ImGui_ImplWin32_WndProcHandler to
    //     cover keyboard input, mouse clicks, scrolling, and focus
    //     events.
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

      switch (msg)
      {
        case WM_INPUT:
        {
          // Extract the RAWINPUT header first to confirm this is actually a
          // mouse report before committing to the full GetRawInputData fetch.
          //
          UINT sz (sizeof (RAWINPUT));
          RAWINPUT raw {};

          const UINT read (::GetRawInputData (reinterpret_cast<HRAWINPUT> (lp),
                                              RID_INPUT,
                                              &raw,
                                              &sz,
                                              sizeof (RAWINPUTHEADER)));

          if (read != static_cast<UINT> (-1) &&
              raw.header.dwType == RIM_TYPEMOUSE)
          {
            r->feed_raw_mouse (raw.data.mouse);
          }

          // Do not forward to ImGui_ImplWin32_WndProcHandler. We have already
          // fed the mouse position, and a second update from ImGui's native
          // WM_INPUT handler would corrupt our accumulated positioning.
          //
          break;
        }

        case WM_MOUSEMOVE:
          // Suppressed specifically from ImGui since the high-fidelity position
          // arrives through WM_INPUT. Gracefully fall through to
          // CallWindowProcW so the host application remains unaffected.
          //
          break;

        default:
          // All remaining messages are unconditionally delegated to the ImGui
          // Win32 backend. Notice that ImGui_ImplWin32_WndProcHandler only
          // returns non-zero for WM_SETCURSOR (to prevent flickering).
          // Purposefully do not gate CallWindowProcW on this return value. That
          // is, we must never consume messages from the host application's
          // event chain.
          //
          ImGui_ImplWin32_WndProcHandler (hwnd, msg, wp, lp);
          break;
      }

      return ::CallWindowProcW (r->original_wnd_proc_, hwnd, msg, wp, lp);
    }
  }
}
