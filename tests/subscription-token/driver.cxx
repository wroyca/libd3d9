#include "../test-utils.hxx"

#include <utility>

#include <libd3d9/d3d9-subscription-token.hxx>

using d3d9::subscription_token;

static void
test_default_constructed_is_inactive ()
{
  subscription_token t;

  check (!t);
  check (!static_cast<bool> (t));
}

static void
test_make_yields_active_token ()
{
  auto t (subscription_token::make ([] {}));

  check (t);
  check (static_cast<bool> (t));
}

static void
test_cancel_fn_fires_on_reset ()
{
  int count (0);
  auto t (subscription_token::make ([&count] { ++count; }));

  t.reset ();

  check (count == 1);
  check (!t); // token is now inactive
}

static void
test_cancel_fn_fires_on_destructor ()
{
  int count (0);

  {
    auto t (subscription_token::make ([&count] { ++count; }));
    check (count == 0);
  } // destructor fires here

  check (count == 1);
}

static void
test_cancel_fn_fires_exactly_once ()
{
  int count (0);
  auto t (subscription_token::make ([&count] { ++count; }));

  t.reset ();
  t.reset (); // second call must be a no-op
  t.reset (); // third call must be a no-op

  check (count == 1);
}

static void
test_reset_on_inactive_token_is_noop ()
{
  subscription_token t; // inactive
  t.reset ();           // must not crash or throw
  check (!t);
}

static void
test_move_constructor_transfers_ownership ()
{
  int count (0);
  auto src (subscription_token::make ([&count] { ++count; }));

  subscription_token dst (std::move (src));

  check (!src);             // source has been stripped
  check (dst);              // destination is active
  check (count == 0);       // cancel not yet fired

  dst.reset ();
  check (count == 1);       // cancel fired exactly once from dst
}

static void
test_move_assign_cancels_existing_then_transfers ()
{
  int cancel_a (0);
  int cancel_b (0);

  auto ta (subscription_token::make ([&cancel_a] { ++cancel_a; }));
  auto tb (subscription_token::make ([&cancel_b] { ++cancel_b; }));

  ta = std::move (tb);

  // ta's original subscription should have been cancelled.
  //
  check (cancel_a == 1);

  // tb's cancel should not have fired yet (it transferred to ta).
  //
  check (cancel_b == 0);
  check (!tb);
  check (ta);

  ta.reset ();
  check (cancel_b == 1);
}

static void
test_move_assign_from_inactive_cancels_target ()
{
  int count (0);
  auto ta (subscription_token::make ([&count] { ++count; }));
  subscription_token tb; // inactive

  ta = std::move (tb);

  check (count == 1);  // ta's cancel fired
  check (!ta);
  check (!tb);
}

static void
test_destructor_of_inactive_token_is_noop ()
{
  int count (0);

  {
    subscription_token t (subscription_token::make ([&count] { ++count; }));
    t.reset (); // cancel fires here
    check (count == 1);
  } // destructor of inactive token — must not fire again

  check (count == 1);
}

static void
test_bool_operator_reflects_state_changes ()
{
  subscription_token t;
  check (!t);

  t = subscription_token::make ([] {});
  check (t);

  t.reset ();
  check (!t);
}

int
main ()
{
  test_default_constructed_is_inactive ();
  test_make_yields_active_token ();
  test_cancel_fn_fires_on_reset ();
  test_cancel_fn_fires_on_destructor ();
  test_cancel_fn_fires_exactly_once ();
  test_reset_on_inactive_token_is_noop ();
  test_move_constructor_transfers_ownership ();
  test_move_assign_cancels_existing_then_transfers ();
  test_move_assign_from_inactive_cancels_target ();
  test_destructor_of_inactive_token_is_noop ();
  test_bool_operator_reflects_state_changes ();
}
