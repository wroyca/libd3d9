#pragma once

#include <functional>

#include <libd3d9/d3d9-win32.hxx>

#include <libd3d9/d3d9-device.hxx>
#include <libd3d9/d3d9-subscription-token.hxx>

#include <libd3d9/detail/d3d9-event-dispatcher.hxx>

#include <libd3d9/export.hxx>

// Forward-declare the ImGui context. We do this so the caller does not need
// to have <imgui.h> on their include path just to hold a renderer instance.
//
struct ImGuiContext;

namespace d3d9
{
  namespace extra
  {
    // Provide the ImGui integration layer for a managed d3d9::device:
    //
    //   d3d9::factory f;
    //
    //   std::unique_ptr<d3d9::device>                dev_mgr;
    //   std::unique_ptr<d3d9::extra::imgui_renderer> ui;
    //   d3d9::subscription_token                     frame_token;
    //
    //   auto create_token (f.on_device_created (
    //     [&] (IDirect3DDevice9& d, D3DPRESENT_PARAMETERS&)
    //     {
    //       dev_mgr = std::make_unique<d3d9::device> (&d);
    //       ui      = std::make_unique<d3d9::extra::imgui_renderer> (*dev_mgr);
    //
    //       frame_token = ui->on_frame ([]
    //       {
    //         ImGui::Begin ("Debug");
    //         ImGui::Text ("Hello from libd3d9!");
    //         ImGui::End ();
    //       });
    //     }));
    //
    // Usage example with an explicit device and window:
    //
    //   d3d9::device dev (pDevice);
    //   d3d9::extra::imgui_renderer ui (dev); // HWND from creation params
    //   // or:
    //   d3d9::extra::imgui_renderer ui (dev, hMyWnd);   // Explicit HWND
    //
    //   auto t (ui.on_frame ([]
    //   {
    //     ImGui::ShowDemoWindow ();
    //   }));
    //
    // Pre-configuring ImGui before construction:
    //
    //   ImGui::CreateContext ();
    //   ImGui::StyleColorsDark ();
    //   ImGuiIO& io (ImGui::GetIO ());
    //   io.Fonts->AddFontFromFileTTF ("myfont.ttf", 16.0f);
    //
    //   d3d9::extra::imgui_renderer ui (dev); // Adopts the context above.
    //
    //
    class LIBD3D9_SYMEXPORT imgui_renderer
    {
    public:
      // Construct and attach to the device. We derive the target HWND from the
      // device's creation parameters.
      //
      explicit
      imgui_renderer (d3d9::device& dev);

      // Construct and attach to the device using an explicitly supplied window
      // handle for the input subclassing.
      //
      imgui_renderer (d3d9::device& dev, HWND window);

      // Cancel all the device subscriptions, restore the original WndProc,
      // remove the Raw Input registration, and shut down the ImGui backends. If
      // we own the ImGui context, we destroy it here as well.
      //
      ~imgui_renderer () noexcept;

      // Explicitly delete the copy and move operations. We do this because the
      // WndProc thunk and the device subscriptions hold raw pointers to this
      // specific instance.
      //
      imgui_renderer (const imgui_renderer&) = delete;
      imgui_renderer& operator= (const imgui_renderer&) = delete;

      imgui_renderer (imgui_renderer&&) = delete;
      imgui_renderer& operator= (imgui_renderer&&) = delete;

      // Subscribe a per-frame callback.
      //
      // We invoke this callback between ImGui::NewFrame() and ImGui::Render()
      // during every EndScene call. The caller should submit their ImGui
      // widgets from within this callback.
      //
      // Note that subscriptions added during dispatch take effect on the next
      // frame. Unsubscribing during dispatch will not suppress the
      // current-frame invocation.
      //
      subscription_token
      on_frame (std::function<void ()> callback);

    private:
      using frame_dispatcher_t =
        detail::event_dispatcher<std::function<void ()>>;

      // Execute the shared initialization logic. Both constructors call this
      // after they establish the window handle.
      //
      void
      init (IDirect3DDevice9* dev_ptr);

      // Handle the EndScene event. This drives the full ImGui frame sequence,
      // including dispatching the frame callbacks and rendering the draw data.
      //
      void
      on_end_scene_impl (IDirect3DDevice9& d);

      // Feed a single RAWMOUSE report into the ImGui mouse position state.
      //
      // We handle both relative (gaming mice) and absolute (Remote Desktop,
      // tablets) coordinate modes. We also clamp the accumulated position to
      // the client area to prevent phantom out-of-window positions.
      //
      void
      feed_raw_mouse (const RAWMOUSE& rm) noexcept;

      // Subclass the window procedure. We look up the renderer associated with
      // this HWND in the process-wide registry, perform the per-message ImGui
      // routing, and then always tail into the original WndProc via
      // CallWindowProcW.
      //
      static LRESULT CALLBACK
      thunk_wnd_proc (HWND hwnd,
                      UINT message,
                      WPARAM wparam,
                      LPARAM lparam);

      // The managed device that we borrowed from the caller.
      //
      d3d9::device& device_;

      // The window that we subclassed for the input forwarding.
      //
      HWND window_;

      // The original window procedure. We call it at the end of every message
      // and restore it in our destructor.
      //
      WNDPROC original_wnd_proc_;

      // The ImGui context. This is non-null only if we created it ourselves.
      //
      ImGuiContext* context_;

      // Flag indicating whether we own the ImGui context and must destroy it in
      // the destructor.
      //
      bool owns_context_;

      // Flag indicating whether RegisterRawInputDevices succeeded. We check
      // this to safely attempt removal in the destructor.
      //
      bool raw_input_registered_;

      // The accumulated client-space mouse coordinates derived from the
      // WM_INPUT reports.
      //
      // Note that for relative mice, we update this incrementally and clamp it
      // to the client rectangle. For absolute sources, we overwrite it
      // directly. Because ImGui synchronizes the cursor position once per
      // frame, any drift between reports will not persist across the frame
      // boundaries.
      //
      float mouse_x_;
      float mouse_y_;

      // The subscriptions to the underlying device events. We store them as
      // members so they are cancelled automatically upon destruction, even if
      // the destructor body is bypassed.
      //
      subscription_token end_scene_token_;
      subscription_token pre_reset_token_;
      subscription_token post_reset_token_;

      // The event dispatcher for the frame subscribers.
      //
      frame_dispatcher_t frame_disp_;
    };
  }
}
