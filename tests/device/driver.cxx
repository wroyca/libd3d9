#include "../test-utils.hxx"

#include <stdexcept>
#include <vector>

#include <libd3d9/d3d9-device.hxx>
#include <libd3d9/d3d9-subscription-token.hxx>

using d3d9::device;
using d3d9::subscription_token;

struct test_fixture
{
  void* vtable[k_mock_vtable_size];
  mock_device dev;

  test_fixture ()
  {
    init_mock_vtable (vtable);
    dev.vtable_ptr = vtable;
  }

  IDirect3DDevice9* ptr () noexcept { return as_d3d9 (&dev); }
};

static void
test_null_device_throws ()
{
  bool threw (false);

  try
  {
    device d (nullptr);
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }

  check (threw);
}

static void
test_double_managed_throws ()
{
  test_fixture fix;
  device d1 (fix.ptr ());

  bool threw (false);

  try
  {
    device d2 (fix.ptr ());
  }
  catch (const std::logic_error&)
  {
    threw = true;
  }

  check (threw);
}

static void
test_managed_device_accessor ()
{
  test_fixture fix;
  device d (fix.ptr ());

  check (d.managed_device () == fix.ptr ());
}

static void
test_destructor_restores_vtable_slots ()
{
  test_fixture fix;

  // Save the original pointers before construction.
  //
  void* orig_3  = fix.vtable[3];
  void* orig_16 = fix.vtable[16];
  void* orig_17 = fix.vtable[17];
  void* orig_41 = fix.vtable[41];
  void* orig_42 = fix.vtable[42];

  {
    device d (fix.ptr ());

    // Slots must have been replaced by the library's thunks.
    //
    check (fix.vtable[3]  != orig_3);
    check (fix.vtable[16] != orig_16);
    check (fix.vtable[17] != orig_17);
    check (fix.vtable[41] != orig_41);
    check (fix.vtable[42] != orig_42);
  }

  // After destruction all five slots are restored.
  //
  check (fix.vtable[3]  == orig_3);
  check (fix.vtable[16] == orig_16);
  check (fix.vtable[17] == orig_17);
  check (fix.vtable[41] == orig_41);
  check (fix.vtable[42] == orig_42);
}

static void
test_begin_scene_fires_after_original_on_success ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int subscriber_fires (0);

  // Intercept the mock so we can observe the ordering.
  //
  fix.dev.on_bs_called = [&] { check (subscriber_fires == 0); };

  auto t (d.on_begin_scene ([&] (IDirect3DDevice9&)
  {
    // At this point the original BeginScene has already been called.
    //
    check (fix.dev.bs_call_count == 1);
    ++subscriber_fires;
  }));

  fix.ptr ()->BeginScene ();

  check (subscriber_fires == 1);
}

static void
test_begin_scene_does_not_fire_on_original_failure ()
{
  test_fixture fix;
  fix.dev.bs_result = E_FAIL;
  device d (fix.ptr ());

  int subscriber_fires (0);
  auto t (d.on_begin_scene ([&] (IDirect3DDevice9&) { ++subscriber_fires; }));

  fix.ptr ()->BeginScene ();

  check (subscriber_fires == 0);
}

static void
test_begin_scene_fifo_order ()
{
  test_fixture fix;
  device d (fix.ptr ());

  std::vector<int> order;

  auto t1 (d.on_begin_scene ([&] (IDirect3DDevice9&) { order.push_back (1); }));
  auto t2 (d.on_begin_scene ([&] (IDirect3DDevice9&) { order.push_back (2); }));
  auto t3 (d.on_begin_scene ([&] (IDirect3DDevice9&) { order.push_back (3); }));

  fix.ptr ()->BeginScene ();

  check (order.size () == 3);
  check (order[0] == 1 && order[1] == 2 && order[2] == 3);
}

static void
test_begin_scene_reentrancy_guard ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int subscriber_fires (0);

  auto t (d.on_begin_scene ([&] (IDirect3DDevice9& dev)
  {
    ++subscriber_fires;

    if (subscriber_fires == 1)
    {
      // Recursive call: depth guard should bypass dispatch.
      //
      dev.BeginScene ();
    }
  }));

  fix.ptr ()->BeginScene ();

  // Subscriber fired once (outer dispatch). Inner call bypassed dispatch.
  //
  check (subscriber_fires == 1);

  // The original mock was called twice: once by the outer thunk, once by the
  // inner thunk which passed through directly.
  //
  check (fix.dev.bs_call_count == 2);
}

static void
test_end_scene_fires_before_original ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int subscriber_fires (0);

  fix.dev.on_es_called = [&] { check (subscriber_fires == 1); };

  auto t (d.on_end_scene ([&] (IDirect3DDevice9&)
  {
    // At this point the original EndScene has NOT been called yet.
    //
    check (fix.dev.es_call_count == 0);
    ++subscriber_fires;
  }));

  fix.ptr ()->EndScene ();

  check (subscriber_fires == 1);
  check (fix.dev.es_call_count == 1);
}

static void
test_end_scene_fifo_order ()
{
  test_fixture fix;
  device d (fix.ptr ());

  std::vector<int> order;

  auto t1 (d.on_end_scene ([&] (IDirect3DDevice9&) { order.push_back (1); }));
  auto t2 (d.on_end_scene ([&] (IDirect3DDevice9&) { order.push_back (2); }));
  auto t3 (d.on_end_scene ([&] (IDirect3DDevice9&) { order.push_back (3); }));

  fix.ptr ()->EndScene ();

  check (order.size () == 3);
  check (order[0] == 1 && order[1] == 2 && order[2] == 3);
}

static void
test_end_scene_reentrancy_guard ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int subscriber_fires (0);

  auto t (d.on_end_scene ([&] (IDirect3DDevice9& dev)
  {
    ++subscriber_fires;

    if (subscriber_fires == 1)
      dev.EndScene (); // inner call — bypasses dispatch
  }));

  fix.ptr ()->EndScene ();

  check (subscriber_fires == 1);
  check (fix.dev.es_call_count == 2); // outer + inner
}

static void
test_present_fires_before_original ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int subscriber_fires (0);

  fix.dev.on_present_called = [&] { check (subscriber_fires == 1); };

  auto t (d.on_present ([&] (IDirect3DDevice9&,
                              const RECT*,
                              const RECT*,
                              HWND,
                              const RGNDATA*)
  {
    check (fix.dev.present_call_count == 0);
    ++subscriber_fires;
  }));

  fix.ptr ()->Present (nullptr, nullptr, nullptr, nullptr);

  check (subscriber_fires == 1);
}

static void
test_present_arguments_forwarded ()
{
  test_fixture fix;
  device d (fix.ptr ());

  RECT src_rect  {1, 2, 3, 4};
  RECT dst_rect  {5, 6, 7, 8};
  HWND wnd       (reinterpret_cast<HWND> (static_cast<uintptr_t> (0xDEAD)));
  RGNDATA region {};

  const RECT*   seen_sr  (nullptr);
  const RECT*   seen_dr  (nullptr);
  HWND          seen_wnd (nullptr);
  const RGNDATA* seen_rg (nullptr);

  auto t (d.on_present ([&] (IDirect3DDevice9&,
                              const RECT*    sr,
                              const RECT*    dr,
                              HWND           dw,
                              const RGNDATA* rg)
  {
    seen_sr  = sr;
    seen_dr  = dr;
    seen_wnd = dw;
    seen_rg  = rg;
  }));

  fix.ptr ()->Present (&src_rect, &dst_rect, wnd, &region);

  check (seen_sr  == &src_rect);
  check (seen_dr  == &dst_rect);
  check (seen_wnd == wnd);
  check (seen_rg  == &region);
}

static void
test_present_reentrancy_guard ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int subscriber_fires (0);

  auto t (d.on_present ([&] (IDirect3DDevice9& dev,
                              const RECT*,
                              const RECT*,
                              HWND,
                              const RGNDATA*)
  {
    ++subscriber_fires;

    if (subscriber_fires == 1)
      dev.Present (nullptr, nullptr, nullptr, nullptr);
  }));

  fix.ptr ()->Present (nullptr, nullptr, nullptr, nullptr);

  check (subscriber_fires == 1);
  check (fix.dev.present_call_count == 2);
}

static void
test_pre_reset_fires_before_original ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int pre_fires (0);

  fix.dev.on_reset_called = [&] { check (pre_fires == 1); };

  auto t (d.on_pre_reset ([&] (IDirect3DDevice9&, D3DPRESENT_PARAMETERS&)
  {
    check (fix.dev.reset_call_count == 0);
    ++pre_fires;
  }));

  D3DPRESENT_PARAMETERS pp {};
  fix.ptr ()->Reset (&pp);

  check (pre_fires == 1);
}

static void
test_post_reset_fires_after_original ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int post_fires (0);

  auto t (d.on_post_reset ([&] (IDirect3DDevice9&,
                                 D3DPRESENT_PARAMETERS&,
                                 HRESULT)
  {
    check (fix.dev.reset_call_count == 1);
    ++post_fires;
  }));

  D3DPRESENT_PARAMETERS pp {};
  fix.ptr ()->Reset (&pp);

  check (post_fires == 1);
}

static void
test_post_reset_receives_original_hresult ()
{
  test_fixture fix;
  fix.dev.reset_result = D3DERR_DEVICELOST;
  device d (fix.ptr ());

  HRESULT seen (S_OK);

  auto t (d.on_post_reset ([&] (IDirect3DDevice9&,
                                 D3DPRESENT_PARAMETERS&,
                                 HRESULT r)
  {
    seen = r;
  }));

  D3DPRESENT_PARAMETERS pp {};
  fix.ptr ()->Reset (&pp);

  check (seen == D3DERR_DEVICELOST);
}

static void
test_reset_pp_is_same_object ()
{
  test_fixture fix;
  device d (fix.ptr ());

  const D3DPRESENT_PARAMETERS* pre_addr  (nullptr);
  const D3DPRESENT_PARAMETERS* post_addr (nullptr);

  auto t1 (d.on_pre_reset ([&] (IDirect3DDevice9&, D3DPRESENT_PARAMETERS& pp)
  {
    pre_addr = &pp;
  }));

  auto t2 (d.on_post_reset ([&] (IDirect3DDevice9&,
                                  D3DPRESENT_PARAMETERS& pp,
                                  HRESULT)
  {
    post_addr = &pp;
  }));

  D3DPRESENT_PARAMETERS pp {};
  fix.ptr ()->Reset (&pp);

  check (pre_addr  == &pp);
  check (post_addr == &pp);
}

static void
test_reset_reentrancy_guard ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int pre_fires (0);

  auto t (d.on_pre_reset ([&] (IDirect3DDevice9& dev,
                                D3DPRESENT_PARAMETERS& pp)
  {
    ++pre_fires;

    if (pre_fires == 1)
      dev.Reset (&pp); // inner call — depth guard bypasses dispatch
  }));

  D3DPRESENT_PARAMETERS pp {};
  fix.ptr ()->Reset (&pp);

  check (pre_fires == 1);
  check (fix.dev.reset_call_count == 2);
}

static void
test_device_lost_fires_on_first_devicelost ()
{
  test_fixture fix;
  fix.dev.tcl_result = D3DERR_DEVICELOST;
  device d (fix.ptr ());

  int lost_fires (0);
  auto t (d.on_device_lost ([&] (IDirect3DDevice9&) { ++lost_fires; }));

  fix.ptr ()->TestCooperativeLevel ();

  check (lost_fires == 1);
}

static void
test_device_lost_fires_on_devicenotreset ()
{
  test_fixture fix;
  fix.dev.tcl_result = D3DERR_DEVICENOTRESET;
  device d (fix.ptr ());

  int lost_fires (0);
  auto t (d.on_device_lost ([&] (IDirect3DDevice9&) { ++lost_fires; }));

  fix.ptr ()->TestCooperativeLevel ();

  check (lost_fires == 1);
}

static void
test_device_lost_does_not_refire_while_still_lost ()
{
  test_fixture fix;
  fix.dev.tcl_result = D3DERR_DEVICELOST;
  device d (fix.ptr ());

  int lost_fires (0);
  auto t (d.on_device_lost ([&] (IDirect3DDevice9&) { ++lost_fires; }));

  fix.ptr ()->TestCooperativeLevel ();
  fix.ptr ()->TestCooperativeLevel ();
  fix.ptr ()->TestCooperativeLevel ();

  check (lost_fires == 1);
}

static void
test_device_restored_fires_after_recovery ()
{
  test_fixture fix;
  fix.dev.tcl_result = D3DERR_DEVICELOST;
  device d (fix.ptr ());

  int restored_fires (0);
  auto t (d.on_device_restored ([&] (IDirect3DDevice9&) { ++restored_fires; }));

  fix.ptr ()->TestCooperativeLevel (); // lost transition
  check (restored_fires == 0);

  fix.dev.tcl_result = S_OK;
  fix.ptr ()->TestCooperativeLevel (); // recovery transition

  check (restored_fires == 1);
}

static void
test_device_restored_does_not_fire_if_never_lost ()
{
  test_fixture fix; // tcl_result defaults to S_OK
  device d (fix.ptr ());

  int restored_fires (0);
  auto t (d.on_device_restored ([&] (IDirect3DDevice9&) { ++restored_fires; }));

  fix.ptr ()->TestCooperativeLevel ();
  fix.ptr ()->TestCooperativeLevel ();

  check (restored_fires == 0);
}

static void
test_device_restored_does_not_refire_while_still_ok ()
{
  test_fixture fix;
  fix.dev.tcl_result = D3DERR_DEVICELOST;
  device d (fix.ptr ());

  int restored_fires (0);
  auto t (d.on_device_restored ([&] (IDirect3DDevice9&) { ++restored_fires; }));

  fix.ptr ()->TestCooperativeLevel (); // lost

  fix.dev.tcl_result = S_OK;
  fix.ptr ()->TestCooperativeLevel (); // restored
  fix.ptr ()->TestCooperativeLevel (); // still OK — must not refire
  fix.ptr ()->TestCooperativeLevel ();

  check (restored_fires == 1);
}

static void
test_full_lost_restored_cycle ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int lost_fires     (0);
  int restored_fires (0);

  auto tl (d.on_device_lost     ([&] (IDirect3DDevice9&) { ++lost_fires; }));
  auto tr (d.on_device_restored ([&] (IDirect3DDevice9&) { ++restored_fires; }));

  // First lost.
  //
  fix.dev.tcl_result = D3DERR_DEVICELOST;
  fix.ptr ()->TestCooperativeLevel ();
  check (lost_fires == 1 && restored_fires == 0);

  // First recovery.
  //
  fix.dev.tcl_result = S_OK;
  fix.ptr ()->TestCooperativeLevel ();
  check (lost_fires == 1 && restored_fires == 1);

  // Second lost.
  //
  fix.dev.tcl_result = D3DERR_DEVICELOST;
  fix.ptr ()->TestCooperativeLevel ();
  check (lost_fires == 2 && restored_fires == 1);

  // Second recovery.
  //
  fix.dev.tcl_result = S_OK;
  fix.ptr ()->TestCooperativeLevel ();
  check (lost_fires == 2 && restored_fires == 2);
}

static void
test_token_cancel_prevents_future_dispatch ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int fires (0);
  auto t (d.on_begin_scene ([&] (IDirect3DDevice9&) { ++fires; }));

  fix.ptr ()->BeginScene ();
  check (fires == 1);

  t.reset (); // cancel

  fix.ptr ()->BeginScene ();
  check (fires == 1); // not called again
}

static void
test_token_cancel_during_dispatch_does_not_suppress ()
{
  test_fixture fix;
  device d (fix.ptr ());

  int fires (0);
  subscription_token t;

  // We use a raw pointer because the lambda needs to reference the token
  // before it is constructed. The pointer is set immediately after.
  //
  subscription_token* tp (&t);

  t = d.on_begin_scene ([&fires, tp] (IDirect3DDevice9&)
  {
    ++fires;
    tp->reset (); // cancel self
  });

  fix.ptr ()->BeginScene ();
  check (fires == 1); // still ran this cycle

  fix.ptr ()->BeginScene ();
  check (fires == 1); // not called after cancellation
}

int
main ()
{
  test_null_device_throws ();
  test_double_managed_throws ();
  test_managed_device_accessor ();
  test_destructor_restores_vtable_slots ();

  test_begin_scene_fires_after_original_on_success ();
  test_begin_scene_does_not_fire_on_original_failure ();
  test_begin_scene_fifo_order ();
  test_begin_scene_reentrancy_guard ();

  test_end_scene_fires_before_original ();
  test_end_scene_fifo_order ();
  test_end_scene_reentrancy_guard ();

  test_present_fires_before_original ();
  test_present_arguments_forwarded ();
  test_present_reentrancy_guard ();

  test_pre_reset_fires_before_original ();
  test_post_reset_fires_after_original ();
  test_post_reset_receives_original_hresult ();
  test_reset_pp_is_same_object ();
  test_reset_reentrancy_guard ();

  test_device_lost_fires_on_first_devicelost ();
  test_device_lost_fires_on_devicenotreset ();
  test_device_lost_does_not_refire_while_still_lost ();
  test_device_restored_fires_after_recovery ();
  test_device_restored_does_not_fire_if_never_lost ();
  test_device_restored_does_not_refire_while_still_ok ();
  test_full_lost_restored_cycle ();

  test_token_cancel_prevents_future_dispatch ();
  test_token_cancel_during_dispatch_does_not_suppress ();
}
