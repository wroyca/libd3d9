#include "../test-utils.hxx"

#include <cstring>
#include <memory>

#include <libd3d9/detail/d3d9-vtable-guard.hxx>

using d3d9::detail::vtable_guard;

// Heap-allocated fake vtable. We use the heap rather than the stack so that
// VirtualProtect does not accidentally change protection on a stack page that
// spans other test-local variables.
//
static constexpr std::size_t k_vt_size = 50;

using fake_vtable = std::unique_ptr<void* []>;

static fake_vtable
make_vtable ()
{
  auto vt (std::make_unique<void* []> (k_vt_size));
  std::memset (vt.get (), 0, k_vt_size * sizeof (void*));
  return vt;
}

// Stable, non-null sentinel pointers used as "original" and "replacement"
// values. We use function addresses so they are in the process address space
// but are never actually called.
//
static void dummy_a () {}
static void dummy_b () {}
static void dummy_c () {}

static void
test_default_is_inactive ()
{
  vtable_guard g;

  check (!g.active ());
  check (g.original () == nullptr);
}

static void
test_construction_patches_slot ()
{
  auto vt (make_vtable ());
  vt[5] = reinterpret_cast<void*> (&dummy_a);

  vtable_guard g (vt.get (), 5, reinterpret_cast<void*> (&dummy_b));

  check (vt[5] == reinterpret_cast<void*> (&dummy_b));
}

static void
test_original_is_saved ()
{
  auto vt (make_vtable ());
  vt[7] = reinterpret_cast<void*> (&dummy_a);

  vtable_guard g (vt.get (), 7, reinterpret_cast<void*> (&dummy_b));

  check (g.original () == reinterpret_cast<void*> (&dummy_a));
}

static void
test_active_after_construction ()
{
  auto vt (make_vtable ());
  vt[0] = reinterpret_cast<void*> (&dummy_a);

  vtable_guard g (vt.get (), 0, reinterpret_cast<void*> (&dummy_b));

  check (g.active ());
}

static void
test_destructor_restores_slot ()
{
  auto vt (make_vtable ());
  vt[10] = reinterpret_cast<void*> (&dummy_a);

  {
    vtable_guard g (vt.get (), 10, reinterpret_cast<void*> (&dummy_b));
    check (vt[10] == reinterpret_cast<void*> (&dummy_b));
  } // destructor restores

  check (vt[10] == reinterpret_cast<void*> (&dummy_a));
}

static void
test_restore_deactivates_guard ()
{
  auto vt (make_vtable ());
  vt[3] = reinterpret_cast<void*> (&dummy_a);

  vtable_guard g (vt.get (), 3, reinterpret_cast<void*> (&dummy_b));
  g.restore ();

  check (!g.active ());
  check (g.original () == nullptr);
  check (vt[3] == reinterpret_cast<void*> (&dummy_a));
}

static void
test_restore_is_idempotent ()
{
  auto vt (make_vtable ());
  vt[1] = reinterpret_cast<void*> (&dummy_a);

  vtable_guard g (vt.get (), 1, reinterpret_cast<void*> (&dummy_b));
  g.restore ();
  g.restore (); // second call must be a no-op, not a crash

  check (vt[1] == reinterpret_cast<void*> (&dummy_a));
  check (!g.active ());
}

static void
test_move_constructor_transfers_ownership ()
{
  auto vt (make_vtable ());
  vt[2] = reinterpret_cast<void*> (&dummy_a);

  vtable_guard src (vt.get (), 2, reinterpret_cast<void*> (&dummy_b));
  vtable_guard dst (std::move (src));

  // Source is stripped.
  //
  check (!src.active ());
  check (src.original () == nullptr);

  // Destination owns the patch.
  //
  check (dst.active ());
  check (dst.original () == reinterpret_cast<void*> (&dummy_a));
  check (vt[2] == reinterpret_cast<void*> (&dummy_b));

  // Destination restores on destruction.
  //
  dst.restore ();
  check (vt[2] == reinterpret_cast<void*> (&dummy_a));
}

static void
test_move_constructor_from_inactive_yields_inactive ()
{
  vtable_guard src; // inactive
  vtable_guard dst (std::move (src));

  check (!dst.active ());
  check (dst.original () == nullptr);
}

static void
test_move_assign_restores_target_and_transfers ()
{
  auto vt_a (make_vtable ());
  auto vt_b (make_vtable ());

  vt_a[5] = reinterpret_cast<void*> (&dummy_a);
  vt_b[5] = reinterpret_cast<void*> (&dummy_b);

  vtable_guard ga (vt_a.get (), 5, reinterpret_cast<void*> (&dummy_c));
  vtable_guard gb (vt_b.get (), 5, reinterpret_cast<void*> (&dummy_c));

  // Verify both patches are live.
  //
  check (vt_a[5] == reinterpret_cast<void*> (&dummy_c));
  check (vt_b[5] == reinterpret_cast<void*> (&dummy_c));

  ga = std::move (gb);

  // ga's previous slot (vt_a[5]) must now hold the original value.
  //
  check (vt_a[5] == reinterpret_cast<void*> (&dummy_a));

  // vt_b[5] is still patched — ownership transferred to ga.
  //
  check (vt_b[5] == reinterpret_cast<void*> (&dummy_c));

  // ga now carries vt_b[5]'s state.
  //
  check (ga.original () == reinterpret_cast<void*> (&dummy_b));
  check (ga.active ());

  // gb is inactive.
  //
  check (!gb.active ());

  // Restoring ga restores vt_b[5].
  //
  ga.restore ();
  check (vt_b[5] == reinterpret_cast<void*> (&dummy_b));
}

static void
test_move_assign_from_inactive_deactivates_target ()
{
  auto vt (make_vtable ());
  vt[4] = reinterpret_cast<void*> (&dummy_a);

  vtable_guard ga (vt.get (), 4, reinterpret_cast<void*> (&dummy_b));
  check (vt[4] == reinterpret_cast<void*> (&dummy_b));

  vtable_guard gb; // inactive
  ga = std::move (gb);

  // ga's slot must have been restored.
  //
  check (vt[4] == reinterpret_cast<void*> (&dummy_a));
  check (!ga.active ());
}

static void
test_independent_patches_on_same_vtable ()
{
  auto vt (make_vtable ());
  vt[10] = reinterpret_cast<void*> (&dummy_a);
  vt[20] = reinterpret_cast<void*> (&dummy_b);

  {
    vtable_guard g10 (vt.get (), 10, reinterpret_cast<void*> (&dummy_c));
    vtable_guard g20 (vt.get (), 20, reinterpret_cast<void*> (&dummy_c));

    check (vt[10] == reinterpret_cast<void*> (&dummy_c));
    check (vt[20] == reinterpret_cast<void*> (&dummy_c));

    g10.restore ();

    // Only slot 10 restored; slot 20 still patched.
    //
    check (vt[10] == reinterpret_cast<void*> (&dummy_a));
    check (vt[20] == reinterpret_cast<void*> (&dummy_c));

  } // g20 destructor restores slot 20

  check (vt[20] == reinterpret_cast<void*> (&dummy_b));
}

int
main ()
{
  test_default_is_inactive ();
  test_construction_patches_slot ();
  test_original_is_saved ();
  test_active_after_construction ();
  test_destructor_restores_slot ();
  test_restore_deactivates_guard ();
  test_restore_is_idempotent ();
  test_move_constructor_transfers_ownership ();
  test_move_constructor_from_inactive_yields_inactive ();
  test_move_assign_restores_target_and_transfers ();
  test_move_assign_from_inactive_deactivates_target ();
  test_independent_patches_on_same_vtable ();
}
