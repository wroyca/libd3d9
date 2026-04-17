# libd3d9

This library provides an interception framework for Direct3D 9 devices. The idea is to intercept key presentation and lifecycle events without the usual vtable manipulation boilerplate.

## Usage Examples

Let's look at a few common scenarios to see how this fits together in practice.

### Basic overlay rendering

Suppose we want to draw a custom overlay. We intercept into the end scene event so we can issue our draw calls just before the scene is actually submitted.

```cpp
#include <libd3d9/d3d9.hxx>

class overlay
{
public:
  explicit
  overlay (IDirect3DDevice9* d)
    : d_ (d),
      t_ (d_.on_end_scene ([this] (IDirect3DDevice9& cd) {
        render (cd);
      }))
  {
    // We intercept the end scene event here so we can render our custom UI
    // just before the scene is submitted to the presentation engine.
    //
  }

private:
  void
  render (IDirect3DDevice9& d)
  {
    // We can issue our draw calls here. The scene is still open and
    // ready to accept new primitives.
    //
    // d.DrawPrimitive (...);
  }

  d3d9::device d_;
  d3d9::subscription_token t_;
};
```

### Handling device reset

Dealing with device resets means we have to release all `D3DPOOL_DEFAULT` resources before the reset happens, and recreate them afterwards. We use the pre and post reset events for this.

```cpp
d3d9::device device (d);

auto t1 (
  device.on_pre_reset (
    [] (IDirect3DDevice9& d, D3DPRESENT_PARAMETERS& p)
    {
      // We have to release all D3DPOOL_DEFAULT resources right before
      // the Reset() function is called.
      //
      rt.Reset ();
      vb.Reset ();
    }));

auto t2 (
  device.on_post_reset (
    [] (IDirect3DDevice9& d, D3DPRESENT_PARAMETERS& p, HRESULT r)
    {
      if (FAILED (r))
      {
        // The device is still unavailable. This means we have to wait
        // for the next reset cycle to try again.
        //
        return;
      }

      // The device has been successfully reset so we can go ahead
      // and recreate the D3DPOOL_DEFAULT resources now.
      //
      d.CreateRenderTarget (..., &rt);
      d.CreateVertexBuffer (..., &vb);
    }));
```

### Responding to device loss

If the device is lost, we need to pause our rendering logic. Once it becomes accessible again, we can signal the application to trigger a reset.

```cpp
d3d9::device device (d);

auto t1 (
  device.on_device_lost (
    [] (IDirect3DDevice9& d)
    {
      // The device is lost, which means we need to stop all rendering
      // operations and pause the application state.
      //
      a = false;
    }));

auto t2 (
  device.on_device_restored (
    [] (IDirect3DDevice9& d)
    {
      // The device is accessible again but Reset() has not yet been
      // called. This is typically where we trigger the reset cycle.
      //
      r = true;
    }));
```

### Intercepting Present (frame capture / statistics)

We can also intercept the presentation phase itself, which is useful for things like frame counting or capture.

```cpp
d3d9::device device (d);

auto t (
  device.on_present (
    [&c] (IDirect3DDevice9&, const RECT*, const RECT*, HWND, const RGNDATA*)
    {
      // Let's keep track of the total number of frames presented.
      //
      ++c;
    }));
```

### Early unsubscription

Tokens automatically unsubscribe when they go out of scope. But occasionally we need to drop a interception early based on some internal condition.

```cpp
{
  auto t (
    device.on_end_scene (
      [] (IDirect3DDevice9& d)
      {
        once ();
      }));

  // We might wait for some internal condition to be met before unintercepting.
  //
  // ...

  // Note that the callback will no longer fire after we reset the token.
  //
  t.reset ();
}

// The token destructor is a no-op since we already called reset().
//
```

### Multiple subscribers, defined order

Note that subscribers fire in the exact order they were registered. This guarantees that your layers, for example, render in a predictable sequence.

```cpp
auto t1 (device.on_end_scene ([] (IDirect3DDevice9&) { l1 (); }));
auto t2 (device.on_end_scene ([] (IDirect3DDevice9&) { l2 (); })); // Fires after t1.
auto t3 (device.on_end_scene ([] (IDirect3DDevice9&) { l3 (); })); // Fires after t2.
```

## API Reference

### Intercepted vtable slots

We intercept the following slots in the `IDirect3DDevice9` vtable. The dispatch mechanism translates these raw calls into our typed events.

| Vtable index | Method                 | Events dispatched                |
|:------------:|:-----------------------|:---------------------------------|
|      3       | `TestCooperativeLevel` | `device_lost`, `device_restored` |
|      16      | `Reset`                | `pre_reset`, `post_reset`        |
|      17      | `Present`              | `present`                        |
|      41      | `BeginScene`           | `begin_scene`                    |
|      42      | `EndScene`             | `end_scene`                      |

### Dispatch ordering per event

It is important to understand when your callback actually runs relative to the original DirectX call.

| Event             | Relative to original call | Result                                    |
|:------------------|:--------------------------|:------------------------------------------|
| `begin_scene`     | **After**                 | device is in a valid scene context        |
| `end_scene`       | **Before**                | subscribers may draw into the open scene  |
| `present`         | **Before**                | subscribers may modify state before flip  |
| `pre_reset`       | **Before**                | subscribers must release resources        |
| `post_reset`      | **After**                 | subscribers may recreate resources        |
| `device_lost`     | **After**                 | TCL returns `DEVICELOST`/`DEVICENOTRESET` |
| `device_restored` | **After**                 | TCL returns `S_OK` following loss         |

### `event_id`

We use a scoped enumeration to identify each interceptable event. This is primarily used as a template argument for `event_traits`.

```cpp
enum class event_id : unsigned int
{
  begin_scene,
  end_scene,
  present,
  pre_reset,
  post_reset,
  device_lost,
  device_restored
};
```

### `event_traits<E>`

We map each `event_id` to its exact callback signature using `event_traits`. Note that the primary template is intentionally left undefined. This design turns invalid event types into a compile-time error.

```cpp
template <event_id E>
struct event_traits
{
  using callback_type = /* event-specific std::function<...> */;
};
```

| `E`               | `callback_type`                                                           |
|:------------------|:--------------------------------------------------------------------------|
| `begin_scene`     | `void(IDirect3DDevice9&)`                                                 |
| `end_scene`       | `void(IDirect3DDevice9&)`                                                 |
| `present`         | `void(IDirect3DDevice9&, const RECT*, const RECT*, HWND, const RGNDATA*)` |
| `pre_reset`       | `void(IDirect3DDevice9&, D3DPRESENT_PARAMETERS&)`                         |
| `post_reset`      | `void(IDirect3DDevice9&, D3DPRESENT_PARAMETERS&, HRESULT)`                |
| `device_lost`     | `void(IDirect3DDevice9&)`                                                 |
| `device_restored` | `void(IDirect3DDevice9&)`                                                 |

### `subscription_token`

Tokens represent a single, active subscription. They are move-only RAII handles. Destroying the token automatically unsubscribes the callback.

### `device`

You create one instance per device. It takes care of installing the vtable interception on construction and properly restoring them on destruction. Naturally, it is neither copyable nor movable.

## Contributing

Contributions are always welcome. If you are interested in helping expand these editing facilities, please feel free to submit a pull request or open an issue to discuss changes.

## License

libd3d9 is released under the [GNU General Public License v3](LICENSE.md).
