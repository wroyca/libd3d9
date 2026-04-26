#pragma once

#include <mutex>

#include <libd3d9/d3d9-win32.hxx>

#include <libd3d9/d3d9-device.hxx>
#include <libd3d9/d3d9-subscription-token.hxx>

#include <libd3d9/extra/d3d9-imgui-renderer.hxx>
#include <libd3d9/extra/d3d9-imgui-viewport-types.hxx>

#include <libd3d9/export.hxx>

// Forward-declare the ImGui ID type to avoid a hard dependency on imgui.h
// in this header.
//
using ImGuiID = unsigned int;

namespace d3d9
{
  namespace extra
  {
    // The rendering bridge between the D3D9 backbuffer and an ImGui-driven
    // viewport.
    //
    class LIBD3D9_SYMEXPORT imgui_viewport
    {
    public:
      // Construct and attach to the renderer using aspect_ratio_mode::preserve.
      //
      // Throw std::runtime_error if we cannot query the backbuffer during the
      // capture resource initialisation on the first frame. Note that all other
      // initialisation is deferred to the first pre_frame callback.
      //
      explicit
      imgui_viewport (imgui_renderer& renderer,
                      aspect_ratio_mode mode = aspect_ratio_mode::preserve);

      // Remove the message gate, cancel all subscriptions, and release all GPU
      // resources.
      //
      ~imgui_viewport ();

      imgui_viewport (const imgui_viewport&) = delete;
      imgui_viewport& operator = (const imgui_viewport&) = delete;

      imgui_viewport (imgui_viewport&&) = delete;
      imgui_viewport& operator = (imgui_viewport&&) = delete;

      // Map a point in window client coordinates to game render coordinates.
      //
      // The parameters client_x and client_y are in the same coordinate space
      // as the LPARAM values carried by WM_MOUSEMOVE and related Windows
      // messages. That is, pixels from the top-left corner of the window client
      // area.
      //
      // On success (return true), we set out_x and out_y to the corresponding
      // position in the game's render target, where (0, 0) is the top-left
      // pixel. The output values are in continuous floating-point coordinates.
      // Truncate or round to integer to obtain the actual texel index.
      //
      // We return false without modifying out_x or out_y if:
      //   a. The point falls outside the rendered game image area (e.g., in a
      //      letterbox bar or outside the viewport window bounds).
      //   b. No frame has completed yet (meaning the layout cache is invalid).
      //
      // Invariants enforced: the output is always in [0, game_width) x [0,
      // game_height) when we return true. Points on the exact boundary (==
      // game_width or == game_height) are rejected so callers can safely use
      // the output as a direct texel address without further bounds checking.
      //
      bool
      map_to_game_space (float client_x,
                         float client_y,
                         float& out_x,
                         float& out_y) const noexcept;

      // Return the most recently cached game image rect in window client
      // coordinates.
      //
      // The returned rect describes the rendered image position and size,
      // excluding any letterbox or pillarbox padding. We update it once per
      // render frame. If called before the first complete frame, the returned
      // rect has all fields set to zero.
      //
      // Note that this function is safe to call from any thread.
      //
      viewport_rect
      image_rect () const noexcept;

      // Change the aspect ratio mode. This takes effect from the next frame.
      //
      void
      set_aspect_ratio_mode (aspect_ratio_mode mode) noexcept;

      // Return the current aspect ratio mode.
      //
      aspect_ratio_mode
      get_aspect_ratio_mode () const noexcept;

    private:
      // Pre-frame callback.
      //
      // Capture the backbuffer into capture_tex_. We call this at the very
      // start of the EndScene handler before any ImGui backend call. It lazily
      // creates the capture resources on the first call.
      //
      void
      on_pre_frame_impl (IDirect3DDevice9& d);

      // Frame callback.
      //
      // Render the fullscreen dockspace and the game viewport window. This
      // calls update_layout to cache the computed image rect.
      //
      void
      on_frame_impl ();

      // Pre-reset callback.
      //
      // Release all D3DPOOL_DEFAULT resources.
      //
      void
      on_pre_reset_impl (IDirect3DDevice9& d, D3DPRESENT_PARAMETERS& pp);

      // Post-reset callback.
      //
      // Recreate the capture resources if the reset succeeded.
      //
      void
      on_post_reset_impl (IDirect3DDevice9& d,
                          D3DPRESENT_PARAMETERS& pp,
                          HRESULT r);

      // Allocate capture_tex_ and capture_surface_ matching the current
      // backbuffer dimensions and format. Return true on success.
      //
      // We read game_width_, game_height_, and game_format_ from the backbuffer
      // surface description on the first call. Subsequent calls (after reset)
      // use the already-stored values.
      //
      bool
      create_capture_resources (IDirect3DDevice9& d) noexcept;

      // Release capture_surface_ and capture_tex_. Note that this is safe to
      // call even if they are already released.
      //
      void
      release_capture_resources () noexcept;

      // Compute the image draw parameters.
      //
      // Calculate the position, size, and UV extents for the given available
      // content region and cache the result in layout_.
      //
      // Here, avail_pos_x/y is the top-left corner of the available content
      // region in ImGui screen coordinates (the output of GetCursorScreenPos).
      // The avail_w/h is the size of the available content region
      // (GetContentRegionAvail).
      //
      // Since both are in ImGui screen coordinates (origin = top-left of the OS
      // window), we convert them to window client coordinates before storing
      // them in layout_. This ensures that map_to_game_space and the gate
      // operate in the exact same space as LPARAM mouse coordinates.
      //
      void
      update_layout (float avail_pos_x,
                     float avail_pos_y,
                     float avail_w,
                     float avail_h) noexcept;

      // Build and return the gate predicate that should be installed on the
      // renderer. We capture 'this' by raw pointer.
      //
      imgui_renderer::gate_fn
      make_gate () noexcept;

      // Classify a Windows message as a mouse input message.
      //
      static bool
      is_mouse_message (UINT msg) noexcept;

      // Extract client-space coordinates from the LPARAM of a mouse message.
      //
      // For WM_MOUSEWHEEL and WM_MOUSEHWHEEL, the coordinates are in screen
      // space and must be converted. This function handles that transparently.
      // We return false if the message does not carry coordinates (such as
      // WM_MOUSELEAVE).
      //
      bool
      extract_client_coords (HWND hwnd,
                             UINT msg,
                             LPARAM lp,
                             float& out_x,
                             float& out_y) const noexcept;

      // The imgui_renderer we are attached to.
      //
      imgui_renderer& renderer_;

      // The current aspect ratio mode.
      //
      // We write this only from the UI thread (via the public setter) and read
      // it only from the render thread (in on_frame_impl). This is safe because
      // we can accept last-write-wins across a frame boundary.
      //
      aspect_ratio_mode mode_;

      // The capture resources.
      //
      // These are D3DPOOL_DEFAULT and must be released before Reset.
      //
      IDirect3DTexture9* capture_tex_;
      IDirect3DSurface9* capture_surface_;

      // The original game render resolution and backbuffer format.
      //
      // We populate these on the first successful create_capture_resources call
      // and reconfirm them after every reset.
      //
      UINT game_width_;
      UINT game_height_;
      D3DFORMAT game_format_;

      // The stable dockspace ID.
      //
      // We set this on the first call to on_frame_impl and reuse it every
      // subsequent frame.
      //
      ImGuiID dockspace_id_;

      // The cached image layout.
      //
      // We write this from update_layout on the render thread, and read it from
      // map_to_game_space and the gate on any thread. It is protected by
      // layout_mtx_.
      //
      struct layout_cache
      {
        // Rendered game image rect in window client coordinates.
        //
        float image_min_x;
        float image_min_y;
        float image_max_x;
        float image_max_y;

        // Game render resolution (texel dimensions).
        //
        float game_w;
        float game_h;

        // UV extents for fill mode (this may be < 0 or > 1 for cropping).
        //
        float uv0_x;
        float uv0_y;
        float uv1_x;
        float uv1_y;

        // True once update_layout has been called at least once.
        //
        bool valid;
      };

      mutable std::mutex layout_mtx_;
      layout_cache layout_;

      // The subscription tokens.
      //
      // Destroying them cancels the subscriptions, ensuring no callback can
      // fire after the destructor body begins.
      //
      subscription_token pre_frame_token_;
      subscription_token frame_token_;
      subscription_token pre_reset_token_;
      subscription_token post_reset_token_;
    };
  }
}
