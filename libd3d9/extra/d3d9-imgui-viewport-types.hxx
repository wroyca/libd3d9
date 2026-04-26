#pragma once

namespace d3d9
{
  namespace extra
  {
    // The idea here is to control how the captured game image gets scaled into
    // the available ImGui viewport region. Depending on the exact layout, we
    // might want to preserve the aspect ratio, stretch it, or fill the space
    // and crop the rest.
    //
    enum class aspect_ratio_mode
    {
      // Maintain the original aspect ratio. We pad with letterbox or pillarbox
      // bars in the background colour if the aspect ratios don't match. This is
      // the safest default since no game pixels are hidden or distorted.
      //
      preserve,

      // Just stretch the image to fill the entire content region. We don't care
      // about aspect ratio here. Every pixel of the content region maps to
      // exactly one game pixel. This only really makes sense if we know the
      // game is running at the same aspect ratio as the UI panel.
      //
      stretch,

      // Scale the image uniformly until it fills the shorter axis, which
      // naturally crops the longer axis symmetrically. We don't get any bars
      // but we do lose game pixels near the edges. Note that the UV coordinates
      // have to be adjusted accordingly.
      //
      fill
    };

    // An axis-aligned rectangle in window client coordinates.
    //
    // We mainly use this to expose the current game image position and size to
    // callers. They typically need this to perform their own hit-testing or
    // coordinate projection.
    //
    struct viewport_rect
    {
      // The top-left corner of the rendered game image.
      //
      // Note that these are in window client coordinates, which means they are
      // pixels from the top-left of the client area, not the screen.
      //
      float x;
      float y;

      // The dimensions of the rendered game image in window client pixels.
      //
      // Keep in mind these are the rendered dimensions, not the original game
      // resolution. If we are at 1:1 scale they are exactly equal, but after we
      // apply letterboxing or fill-cropping, they will differ.
      //
      float w;
      float h;

      // Default initialize everything to zero. We use () for initialization in
      // the member initializer list to stay consistent with the codebase.
      //
      viewport_rect ()
        : x (0.0f), y (0.0f), w (0.0f), h (0.0f) {}

      // We also provide a parameterized constructor for convenience when
      // passing these around.
      //
      viewport_rect (float x_, float y_, float w_, float h_)
        : x (x_), y (y_), w (w_), h (h_) {}
    };
  }
}
