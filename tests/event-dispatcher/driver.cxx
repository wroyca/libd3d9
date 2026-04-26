#include "../test-utils.hxx"

#include <functional>
#include <string>
#include <vector>

#include <libd3d9/detail/d3d9-event-dispatcher.hxx>

using d3d9::detail::event_dispatcher;

static void
test_starts_empty ()
{
  event_dispatcher<std::function<void ()>> d;
  check (d.subscriber_count () == 0);
}

static void
test_subscribe_increments_count ()
{
  event_dispatcher<std::function<void ()>> d;

  d.subscribe ([] {});
  check (d.subscriber_count () == 1);

  d.subscribe ([] {});
  check (d.subscriber_count () == 2);
}

static void
test_unsubscribe_decrements_count ()
{
  event_dispatcher<std::function<void ()>> d;

  const auto id (d.subscribe ([] {}));
  check (d.subscriber_count () == 1);

  const bool ok (d.unsubscribe (id));
  check (ok);
  check (d.subscriber_count () == 0);
}

static void
test_unsubscribe_unknown_id_returns_false ()
{
  event_dispatcher<std::function<void ()>> d;

  d.subscribe ([] {});
  check (d.subscriber_count () == 1);

  const bool ok (d.unsubscribe (9999));
  check (!ok);
  check (d.subscriber_count () == 1); // count unchanged
}

static void
test_dispatch_invokes_all_subscribers ()
{
  event_dispatcher<std::function<void ()>> d;

  int a (0), b (0), c (0);

  d.subscribe ([&a] { ++a; });
  d.subscribe ([&b] { ++b; });
  d.subscribe ([&c] { ++c; });

  d.dispatch ();

  check (a == 1);
  check (b == 1);
  check (c == 1);
}

static void
test_dispatch_fifo_order ()
{
  event_dispatcher<std::function<void ()>> d;

  std::vector<int> order;

  d.subscribe ([&order] { order.push_back (1); });
  d.subscribe ([&order] { order.push_back (2); });
  d.subscribe ([&order] { order.push_back (3); });

  d.dispatch ();

  check (order.size () == 3);
  check (order[0] == 1);
  check (order[1] == 2);
  check (order[2] == 3);
}

static void
test_dispatch_returns_subscriber_count ()
{
  event_dispatcher<std::function<void ()>> d;

  d.subscribe ([] {});
  d.subscribe ([] {});

  const std::size_t n (d.dispatch ());
  check (n == 2);
}

static void
test_dispatch_empty_returns_zero ()
{
  event_dispatcher<std::function<void ()>> d;
  check (d.dispatch () == 0);
}

static void
test_dispatch_forwards_arguments ()
{
  event_dispatcher<std::function<void (int, float, const std::string&)>> d;

  int   received_i (0);
  float received_f (0.0f);
  std::string received_s;

  d.subscribe ([&] (int i, float f, const std::string& s)
  {
    received_i = i;
    received_f = f;
    received_s = s;
  });

  d.dispatch (42, 3.14f, std::string ("hello"));

  check (received_i == 42);
  check (received_f == 3.14f);
  check (received_s == "hello");
}

static void
test_dispatch_multiple_args_to_multiple_subscribers ()
{
  event_dispatcher<std::function<void (int, int)>> d;

  int sum_a (0), sum_b (0);

  d.subscribe ([&sum_a] (int x, int y) { sum_a = x + y; });
  d.subscribe ([&sum_b] (int x, int y) { sum_b = x + y; });

  d.dispatch (10, 20);

  check (sum_a == 30);
  check (sum_b == 30); // would be wrong if dispatch moved args after first sub
}

static void
test_subscribe_during_dispatch_deferred ()
{
  event_dispatcher<std::function<void ()>> d;

  int inner_fires (0);

  d.subscribe ([&d, &inner_fires]
  {
    // Add a second subscriber mid-dispatch.
    //
    d.subscribe ([&inner_fires] { ++inner_fires; });
  });

  d.dispatch (); // snapshot = 1 entry; inner sub added but not in snapshot

  check (inner_fires == 0); // not called this cycle

  d.dispatch (); // now snapshot = 2 entries

  check (inner_fires == 1); // called in the second cycle
}

static void
test_unsubscribe_during_dispatch_does_not_suppress_current_cycle ()
{
  event_dispatcher<std::function<void ()>> d;

  int fires (0);

  // We need the ID to unsubscribe from inside the callback, so we use an
  // indirection via a pointer.
  //
  decltype (d)::subscription_id id (0);

  id = d.subscribe ([&d, &fires, &id]
  {
    ++fires;
    d.unsubscribe (id); // remove itself
  });

  d.dispatch ();
  check (fires == 1); // callback ran despite removing itself

  d.dispatch ();
  check (fires == 1); // not called again since removed
}

static void
test_subscribe_then_unsubscribe_leaves_zero_count ()
{
  event_dispatcher<std::function<void ()>> d;

  const auto id (d.subscribe ([] {}));
  d.unsubscribe (id);

  check (d.subscriber_count () == 0);
  check (d.dispatch () == 0);
}

static void
test_ids_are_monotonically_increasing ()
{
  event_dispatcher<std::function<void ()>> d;

  auto id1 (d.subscribe ([] {}));
  auto id2 (d.subscribe ([] {}));
  auto id3 (d.subscribe ([] {}));

  check (id2 > id1);
  check (id3 > id2);
}

static void
test_dispatch_after_all_unsubscribed_returns_zero ()
{
  event_dispatcher<std::function<void ()>> d;

  auto id1 (d.subscribe ([] {}));
  auto id2 (d.subscribe ([] {}));

  d.unsubscribe (id1);
  d.unsubscribe (id2);

  check (d.subscriber_count () == 0);
  check (d.dispatch () == 0);
}

int
main ()
{
  test_starts_empty ();
  test_subscribe_increments_count ();
  test_unsubscribe_decrements_count ();
  test_unsubscribe_unknown_id_returns_false ();
  test_dispatch_invokes_all_subscribers ();
  test_dispatch_fifo_order ();
  test_dispatch_returns_subscriber_count ();
  test_dispatch_empty_returns_zero ();
  test_dispatch_forwards_arguments ();
  test_dispatch_multiple_args_to_multiple_subscribers ();
  test_subscribe_during_dispatch_deferred ();
  test_unsubscribe_during_dispatch_does_not_suppress_current_cycle ();
  test_subscribe_then_unsubscribe_leaves_zero_count ();
  test_ids_are_monotonically_increasing ();
  test_dispatch_after_all_unsubscribed_returns_zero ();
}
