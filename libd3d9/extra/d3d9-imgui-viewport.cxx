#include <libd3d9/extra/d3d9-imgui-viewport.hxx>

#include <cassert>
#include <cmath>

#include <libimgui-docking/backends/imgui_impl_dx9.h>
#include <libimgui-docking/imgui.h>

#include <libd3d9/d3d9-win32.hxx>

namespace d3d9
{
  namespace extra
  {
    namespace
    {
      constexpr const char* dockspace_host_title ("##d3d9_viewport_host");
      constexpr const char* game_window_title ("Game##d3d9_viewport");
      constexpr const char* dockspace_label ("##d3d9_dockspace");
    }

    // We subscribe to device events in the constructor.
    //
    imgui_viewport::imgui_viewport (imgui_renderer& renderer,
                                    aspect_ratio_mode mode)
      : renderer_ (renderer),
        mode_ (mode),
        capture_tex_ (nullptr),
        capture_surface_ (nullptr),
        game_width_ (0),
        game_height_ (0),
        game_format_ (D3DFMT_UNKNOWN),
        dockspace_id_ (0),
        layout_ ()
    {
      // Subscribe to the pre-frame interception for backbuffer capture.
      //
      // We must be the first pre-frame subscriber so that the texture is
      // populated before any other subscriber (including the frame dispatcher)
      // tries to read it.
      //
      pre_frame_token_ = renderer_.on_pre_frame ([this] (IDirect3DDevice9& d)
      {
        on_pre_frame_impl (d);
      });

      // Next, subscribe to the frame interception for the dockspace and
      // viewport rendering.
      //
      frame_token_ = renderer_.on_frame ([this]
      {
        on_frame_impl ();
      });

      // Finally, we subscribe to device reset events through the renderer's
      // device reference.
      //
      // We subscribe through the device directly rather than going through the
      // renderer so that the lifetime relationship is explicit. These tokens
      // will be cancelled before capture_surface_ and capture_tex_ are released
      // in the destructor.
      //
      pre_reset_token_ = renderer_.device_.on_pre_reset (
        [this] (IDirect3DDevice9& d, D3DPRESENT_PARAMETERS& pp)
      {
        on_pre_reset_impl (d, pp);
      });

      post_reset_token_ = renderer_.device_.on_post_reset (
        [this] (IDirect3DDevice9& d, D3DPRESENT_PARAMETERS& pp, HRESULT r)
      {
        on_post_reset_impl (d, pp, r);
      });

      // Install the input gate on the renderer. Note that this replaces any
      // gate the caller may have already installed, which is expected and
      // documented in the class contract.
      //
      renderer_.set_message_gate (make_gate ());
    }

    // Cancel subscriptions before releasing GPU resources.
    //
    // Note that the render thread might still be in flight when the destructor
    // runs. Cancelling the tokens prevent callback attempts from accessing a
    // half-destroyed object.
    //
    imgui_viewport::~imgui_viewport () noexcept
    {
      // First, remove the message gate so no further gate invocations can reach
      // us.
      //
      renderer_.clear_message_gate ();

      // Now cancel the render-thread subscriptions. After we reset() them, the
      // corresponding callbacks cannot fire again. We do this in the reverse
      // order of construction.
      //
      post_reset_token_.reset ();
      pre_reset_token_.reset ();
      frame_token_.reset ();
      pre_frame_token_.reset ();

      // Finally, release the GPU resources. At this point, we are safe and no
      // callback can race with us.
      //
      release_capture_resources ();
    }

    bool
    imgui_viewport::map_to_game_space (float client_x,
                                       float client_y,
                                       float& out_x,
                                       float& out_y) const noexcept
    {
      layout_cache snap;
      {
        std::lock_guard<std::mutex> l (layout_mtx_);
        snap = layout_;
      }

      if (!snap.valid)
        return false;

      // Reject points on or outside the image boundary.
      //
      if (client_x < snap.image_min_x || client_y < snap.image_min_y ||
          client_x >= snap.image_max_x || client_y >= snap.image_max_y)
        return false;

      // Now scale from client pixels to game texels.
      //
      // The rendered image spans [image_min, image_max) in client space and
      // maps onto [0, game_w) x [0, game_h) in game space. The UV extents
      // (uv0/uv1) account for fill-mode cropping, describing the fraction of
      // the texture that is actually visible.
      //
      const float norm_x ((client_x - snap.image_min_x) /
                          (snap.image_max_x - snap.image_min_x));
      const float norm_y ((client_y - snap.image_min_y) /
                          (snap.image_max_y - snap.image_min_y));

      const float gx ((snap.uv0_x + norm_x * (snap.uv1_x - snap.uv0_x)) *
                      snap.game_w);
      const float gy ((snap.uv0_y + norm_y * (snap.uv1_y - snap.uv0_y)) *
                      snap.game_h);

      // Just to be safe, enforce the half-open boundary invariant.
      //
      if (gx < 0.0f || gx >= snap.game_w || gy < 0.0f || gy >= snap.game_h)
        return false;

      out_x = gx;
      out_y = gy;

      return true;
    }

    viewport_rect
    imgui_viewport::image_rect () const noexcept
    {
      std::lock_guard<std::mutex> l (layout_mtx_);

      if (!layout_.valid)
        return {0.0f, 0.0f, 0.0f, 0.0f};

      return {layout_.image_min_x,
              layout_.image_min_y,
              layout_.image_max_x - layout_.image_min_x,
              layout_.image_max_y - layout_.image_min_y};
    }

    void
    imgui_viewport::set_aspect_ratio_mode (aspect_ratio_mode m) noexcept
    {
      // Atomic store is not required here.
      //
      // We only read mode_ from the render thread (in on_frame_impl) and write
      // it from the caller's thread. We can accept the last-write-wins semantic
      // across a frame boundary, since that is exactly the expected
      // user-visible behaviour.
      //
      mode_ = m;
    }

    aspect_ratio_mode
    imgui_viewport::get_aspect_ratio_mode () const noexcept
    {
      return mode_;
    }

    // Capture the backbuffer.
    //
    // This is called at the very start of the EndScene handler, before any
    // ImGui backend call.
    //
    void
    imgui_viewport::on_pre_frame_impl (IDirect3DDevice9& d)
    {
      // We lazily create the capture resources on the first call, or try again
      // if a previous attempt failed. This way we don't need to access the
      // device at construction time.
      //
      if (capture_surface_ == nullptr)
      {
        if (!create_capture_resources (d))
          return; // resources are unavailable this frame, try again next time
      }

      // Let's acquire the backbuffer for the primary swap chain. Note that
      // index 0 is the only backbuffer in a standard D3D9 swapchain.
      //
      IDirect3DSurface9* bb (nullptr);
      const HRESULT hr (d.GetBackBuffer (0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));

      if (FAILED (hr))
      {
        assert (false && "GetBackBuffer failed in pre_frame capture path");
        return;
      }

      // Now we StretchRect from the backbuffer into our capture surface. We use
      // D3DTEXF_NONE for the copy because we want the exact source pixels,
      // rather than a filtered downsample.
      //
      // Note that both surfaces are D3DPOOL_DEFAULT render target surfaces.
      // This is the only combination that D3D9 guarantees for StretchRect to
      // work correctly.
      //
      const HRESULT sr (
        d.StretchRect (bb, nullptr, capture_surface_, nullptr, D3DTEXF_NONE));
      bb->Release ();

      assert (SUCCEEDED (sr) && "StretchRect to capture surface failed");
    }

    // Render the dockspace and game viewport.
    //
    void
    imgui_viewport::on_frame_impl ()
    {
      ImGuiIO& io (ImGui::GetIO ());
      ImGuiViewport* mv (ImGui::GetMainViewport ());

      // First, we set up the fullscreen opaque dockspace host window.
      //
      // This host window covers the entire OS window client area. It carries a
      // fully opaque background that occludes the raw backbuffer. Thus, the
      // game is visible only through the viewport texture.
      //
      ImGui::SetNextWindowPos (mv->WorkPos);
      ImGui::SetNextWindowSize (mv->WorkSize);
      ImGui::SetNextWindowViewport (mv->ID);

      // We don't want any alpha here since the background must be entirely
      // opaque.
      //
      ImGui::SetNextWindowBgAlpha (1.0f);

      const ImGuiWindowFlags host_flags (
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking);

      ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, 0.0f);
      ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));

      ImGui::Begin (dockspace_host_title, nullptr, host_flags);
      ImGui::PopStyleVar (3);

      // Let's derive a stable dockspace ID from the label. We only do this once
      // and cache it because GetID(), while cheap, is not completely free.
      //
      if (dockspace_id_ == 0)
        dockspace_id_ = ImGui::GetID (dockspace_label);

      ImGui::DockSpace (dockspace_id_,
                        ImVec2 (0.0f, 0.0f),
                        ImGuiDockNodeFlags_None);
      ImGui::End ();

      // Second, we handle the game viewport window.
      //
      // We dock into our dockspace on the first use. After that, the user gets
      // to control the docking layout.
      //
      ImGui::SetNextWindowDockID (dockspace_id_, ImGuiCond_FirstUseEver);

      // We use no padding so the image fills the content region exactly.
      //
      ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));

      ImGui::Begin (game_window_title,
                    nullptr,
                    ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

      ImGui::PopStyleVar ();

      const ImVec2 avail_pos (ImGui::GetCursorScreenPos ());
      const ImVec2 avail_size (ImGui::GetContentRegionAvail ());

      if (capture_tex_ != nullptr && avail_size.x > 0.0f &&
          avail_size.y > 0.0f && game_width_ > 0 && game_height_ > 0)
      {
        // First, compute the layout parameters for this frame.
        //
        update_layout (avail_pos.x, avail_pos.y, avail_size.x, avail_size.y);

        // Now retrieve the snapshot that update_layout just wrote. This lets us
        // read the UV extents and draw position without holding the lock across
        // the ImGui calls.
        //
        layout_cache snap;
        {
          std::lock_guard<std::mutex> l (layout_mtx_);
          snap = layout_;
        }

        const float draw_x (snap.image_min_x - avail_pos.x);
        const float draw_y (snap.image_min_y - avail_pos.y);
        const float draw_w (snap.image_max_x - snap.image_min_x);
        const float draw_h (snap.image_max_y - snap.image_min_y);

        // If there is letterbox or pillarbox padding, we need to fill it with
        // the window background colour so no raw backbuffer pixels bleed
        // through.
        //
        // We achieve this simply by drawing a solid-coloured rectangle over the
        // full content region before the Image call. The Image itself is then
        // drawn on top.
        //
        ImDrawList* dl (ImGui::GetWindowDrawList ());

        const ImU32 bg (ImGui::GetColorU32 (ImGuiCol_WindowBg));

        dl->AddRectFilled (
          avail_pos,
          ImVec2 (avail_pos.x + avail_size.x, avail_pos.y + avail_size.y),
          bg);

        // Advance the cursor to the image origin so ImGui::Image is positioned
        // correctly within the content region.
        //
        ImGui::SetCursorPosX (ImGui::GetCursorPosX () + draw_x);
        ImGui::SetCursorPosY (ImGui::GetCursorPosY () + draw_y);

        ImGui::Image (reinterpret_cast<ImTextureID> (capture_tex_),
                      ImVec2 (draw_w, draw_h),
                      ImVec2 (snap.uv0_x, snap.uv0_y),
                      ImVec2 (snap.uv1_x, snap.uv1_y));
      }
      else
      {
        // We don't have a texture yet, so invalidate the layout cache. This
        // tells the gate not to pass any input to the game.
        //
        std::lock_guard<std::mutex> l (layout_mtx_);
        layout_.valid = false;
      }

      ImGui::End ();
    }

    void
    imgui_viewport::on_pre_reset_impl (IDirect3DDevice9&,
                                       D3DPRESENT_PARAMETERS&)
    {
      // We must release all D3DPOOL_DEFAULT resources before the device resets.
      // If we fail to do so, Reset() will return D3DERR_INVALIDCALL.
      //
      release_capture_resources ();

      // Invalidate the layout cache so the gate does not forward input to a
      // device that is about to be reset.
      //
      std::lock_guard<std::mutex> l (layout_mtx_);
      layout_.valid = false;
    }

    void
    imgui_viewport::on_post_reset_impl (IDirect3DDevice9& d,
                                        D3DPRESENT_PARAMETERS&,
                                        HRESULT r)
    {
      if (FAILED (r))
        return;

      // Recreate the capture resources now. If this fails, on_pre_frame_impl
      // will just retry on the next frame.
      //
      create_capture_resources (d);
    }

    bool
    imgui_viewport::create_capture_resources (IDirect3DDevice9& d) noexcept
    {
      assert (capture_tex_ == nullptr && "create called with live texture");
      assert (capture_surface_ == nullptr && "create called with live surface");

      // Let's obtain the backbuffer to read its description. We release it
      // immediately since we only need the format and dimensions.
      //
      IDirect3DSurface9* bb (nullptr);
      const HRESULT hbb (d.GetBackBuffer (0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));

      if (FAILED (hbb))
      {
        assert (false &&
                "GetBackBuffer failed during capture resource creation");
        return false;
      }

      D3DSURFACE_DESC desc {};
      const HRESULT hd (bb->GetDesc (&desc));
      bb->Release ();

      if (FAILED (hd))
      {
        assert (false && "GetDesc on backbuffer failed");
        return false;
      }

      game_width_ = desc.Width;
      game_height_ = desc.Height;
      game_format_ = desc.Format;

      // Now create a render-target texture that matches the backbuffer exactly.
      // Note that D3DUSAGE_RENDERTARGET + D3DPOOL_DEFAULT is the only valid
      // combination for a StretchRect destination.
      //
      IDirect3DTexture9* tex (nullptr);
      const HRESULT ht (d.CreateTexture (game_width_,
                                         game_height_,
                                         1, // mip levels
                                         D3DUSAGE_RENDERTARGET,
                                         game_format_,
                                         D3DPOOL_DEFAULT,
                                         &tex,
                                         nullptr));

      if (FAILED (ht))
      {
        assert (false && "CreateTexture for capture failed");
        return false;
      }

      // Acquire a pointer to level 0 of the texture. This is the surface we
      // will pass to StretchRect. We hold onto it separately to avoid calling
      // GetSurfaceLevel every single frame.
      //
      IDirect3DSurface9* surf (nullptr);
      const HRESULT hs (tex->GetSurfaceLevel (0, &surf));

      if (FAILED (hs))
      {
        tex->Release ();
        assert (false && "GetSurfaceLevel on capture texture failed");
        return false;
      }

      capture_tex_ = tex;
      capture_surface_ = surf;

      return true;
    }

    void
    imgui_viewport::release_capture_resources () noexcept
    {
      if (capture_surface_ != nullptr)
      {
        capture_surface_->Release ();
        capture_surface_ = nullptr;
      }

      if (capture_tex_ != nullptr)
      {
        capture_tex_->Release ();
        capture_tex_ = nullptr;
      }
    }

    // Compute the layout.
    //
    // This is called from on_frame_impl once per frame, inside the game
    // viewport window, after the cursor position and content region size are
    // known.
    //
    // All three aspect ratio modes are handled here:
    //
    //   preserve
    //     We compute a uniform scale factor that fits the game image within
    //     the available region while preserving the aspect ratio. Letterbox
    //     or pillarbox padding is then centred, and the UV covers the full
    //     [0,1] x [0,1] range.
    //
    //   stretch
    //     The image occupies the full available region. Here the scale is
    //     non-uniform, but the UV still covers the full range.
    //
    //   fill
    //     We compute a uniform scale factor that fills the available region
    //     by overscaling, meaning the image is cropped symmetrically. We
    //     adjust the UV inward so only the visible centre portion of the
    //     game image is shown.
    //
    // Note that avail_pos is in ImGui screen coordinates (origin is top-left
    // of the OS window). Since ImGui screen coordinates are identical to
    // window client coordinates on D3D9 (there is only one window), we can
    // store them directly in layout_ as client coordinates.
    //
    void
    imgui_viewport::update_layout (float avail_pos_x,
                                   float avail_pos_y,
                                   float avail_w,
                                   float avail_h) noexcept
    {
      const float gw (static_cast<float> (game_width_));
      const float gh (static_cast<float> (game_height_));

      float draw_w (0.0f);
      float draw_h (0.0f);
      float uv0_x (0.0f);
      float uv0_y (0.0f);
      float uv1_x (1.0f);
      float uv1_y (1.0f);
      float pad_x (0.0f);
      float pad_y (0.0f);

      switch (mode_)
      {
        case aspect_ratio_mode::preserve:
        {
          const float scale (std::fminf (avail_w / gw, avail_h / gh));
          draw_w = gw * scale;
          draw_h = gh * scale;
          pad_x = std::floorf ((avail_w - draw_w) * 0.5f);
          pad_y = std::floorf ((avail_h - draw_h) * 0.5f);
          break;
        }

        case aspect_ratio_mode::stretch:
        {
          draw_w = avail_w;
          draw_h = avail_h;
          break;
        }

        case aspect_ratio_mode::fill:
        {
          // Scale uniformly until the shorter axis fills the available space.
          // The longer axis will overflow, so we adjust the UV to crop it
          // symmetrically.
          //
          const float scale (std::fmaxf (avail_w / gw, avail_h / gh));
          const float scaled_w (gw * scale);
          const float scaled_h (gh * scale);

          // Find the UV fraction that is visible on each axis.
          //
          const float vis_u (avail_w / scaled_w);
          const float vis_v (avail_h / scaled_h);

          uv0_x = (1.0f - vis_u) * 0.5f;
          uv0_y = (1.0f - vis_v) * 0.5f;
          uv1_x = uv0_x + vis_u;
          uv1_y = uv0_y + vis_v;

          draw_w = avail_w;
          draw_h = avail_h;
          break;
        }
      }

      // Compute the image rect in window client coordinates.
      //
      const float min_x (avail_pos_x + pad_x);
      const float min_y (avail_pos_y + pad_y);

      layout_cache next;
      next.image_min_x = min_x;
      next.image_min_y = min_y;
      next.image_max_x = min_x + draw_w;
      next.image_max_y = min_y + draw_h;
      next.game_w = gw;
      next.game_h = gh;
      next.uv0_x = uv0_x;
      next.uv0_y = uv0_y;
      next.uv1_x = uv1_x;
      next.uv1_y = uv1_y;
      next.valid = true;

      std::lock_guard<std::mutex> l (layout_mtx_);
      layout_ = next;
    }

    imgui_renderer::gate_fn
    imgui_viewport::make_gate () noexcept
    {
      // Capture 'this' by raw pointer.
      //
      // The gate is always cleared in the destructor before any member is
      // released, so the pointer is guaranteed to be valid for the full
      // lifetime of any gate invocation. Let's just pass it along.
      //
      return [this] (UINT msg, WPARAM wp, LPARAM lp) noexcept -> bool
      {
        // Keyboard messages are always forwarded to the game, regardless of
        // where the cursor is. The game must remain able to receive key input
        // even when the cursor is over a panel outside the viewport.
        //
        if (!is_mouse_message (msg))
          return true;

        // For mouse messages we need the client-space coordinates to check
        // against the image rect. Let's extract them from the message.
        //
        float cx (0.0f);
        float cy (0.0f);
        if (!extract_client_coords (renderer_.window (), msg, lp, cx, cy))
          return true; // forward as we cannot extract coords

        // Snapshot the layout. If the layout is not yet valid (i.e. no frame
        // has completed), block all mouse input as a conservative default.
        // The game image is not visible yet, so there is nowhere valid to
        // send it.
        //
        layout_cache snap;
        {
          std::lock_guard<std::mutex> l (layout_mtx_);
          snap = layout_;
        }

        if (!snap.valid)
          return false;

        // Finally, perform a hard clip. Forward the input only if the cursor
        // is strictly inside the image rect. The half-open boundary [min, max)
        // matches the invariant enforced by map_to_game_space.
        //
        return (cx >= snap.image_min_x && cy >= snap.image_min_y &&
                cx < snap.image_max_x && cy < snap.image_max_y);
      };
    }

    bool
    imgui_viewport::is_mouse_message (UINT msg) noexcept
    {
      switch (msg)
      {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_MOUSELEAVE:
        case WM_NCMOUSEMOVE:
          return true;

        default:
          return false;
      }
    }

    bool
    imgui_viewport::extract_client_coords (HWND hwnd,
                                           UINT msg,
                                           LPARAM lp,
                                           float& out_x,
                                           float& out_y) const noexcept
    {
      if (msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE)
        return false;

      if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
      {
        // These messages carry screen-space coordinates in the LPARAM. We
        // must convert them to client space before comparing against our
        // image rect.
        //
        POINT p;
        p.x = static_cast<LONG> (static_cast<short> (LOWORD (lp)));
        p.y = static_cast<LONG> (static_cast<short> (HIWORD (lp)));
        ::ScreenToClient (hwnd, &p);

        out_x = static_cast<float> (p.x);
        out_y = static_cast<float> (p.y);

        return true;
      }

      // All remaining mouse messages carry client-space coordinates directly in
      // the LPARAM (where LOWORD = x, HIWORD = y), signed 16-bit each.
      //
      out_x = static_cast<float> (static_cast<short> (LOWORD (lp)));
      out_y = static_cast<float> (static_cast<short> (HIWORD (lp)));

      return true;
    }
  }
}
