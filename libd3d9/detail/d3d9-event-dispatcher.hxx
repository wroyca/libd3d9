#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace d3d9
{
  namespace detail
  {
    // Thread-safe, ordered event dispatcher.
    //
    // The Callback template parameter is the callable type stored per
    // subscriber. It must be copy-constructible because we snapshot the list
    // before dispatching. Typically this is std::function<Signature>.
    //
    // Ordering:
    //
    // Subscribers are invoked in the exact order they subscribed (FIFO) on
    // every dispatch cycle.
    //
    // Threading model:
    //
    // The subscribe() and unsubscribe() functions can be called from any
    // thread. That is, they both acquire an internal mutex. The dispatch()
    // function snapshots the subscriber list under the mutex and then invokes
    // all callbacks *without* holding the lock.
    //
    // This implies a few things:
    //
    // 1. A subscription made during a dispatch takes effect on the next cycle.
    // 2. An unsubscription made during a dispatch does not affect the current
    //    cycle (the callback is still invoked this time).
    //
    // Note that dispatch() itself is not re-entrant with respect to another
    // dispatch() on the same dispatcher from the same thread. Callers are
    // responsible for this at a higher architectural level.
    //
    template <typename Callback>
    class event_dispatcher
    {
    public:
      using callback_type = Callback;
      using subscription_id = std::uint64_t;

      event_dispatcher ()
        : next_id_ (1) {}

      event_dispatcher (const event_dispatcher&) = delete;
      event_dispatcher& operator = (const event_dispatcher&) = delete;

      // Subscription.
      //

      // Add a callback and return its id. We assert that it is not empty
      // because dispatching a null function would just crash.
      //
      subscription_id
      subscribe (callback_type cb);

      // Drop a subscription by its id.
      //
      // We silently ignore the request if the id is not found and just
      // return false.
      //
      bool
      unsubscribe (subscription_id id) noexcept;

      // Check how many subscribers we currently have.
      //
      std::size_t
      subscriber_count () const noexcept;

      // Dispatch.
      //

      // Fire the event to everyone in the snapshot.
      //
      // Keep in mind that arguments are passed by reference to each
      // callback. If you forward an rvalue, the first callback might
      // steal it and leave the rest observing a moved-from state.
      //
      template <typename... Args>
      std::size_t
      dispatch (Args&&... args) const;

    private:
      struct entry
      {
        subscription_id id;
        callback_type callback;

        entry (subscription_id i, callback_type c)
          : id (i), callback (std::move (c)) {}
      };

      mutable std::mutex mutex_;
      std::uint64_t next_id_;
      std::vector<entry> entries_;
    };

    // Template implementation.
    //

    template <typename Callback>
    typename event_dispatcher<Callback>::subscription_id
    event_dispatcher<Callback>::subscribe (callback_type cb)
    {
      assert (cb && "event_dispatcher::subscribe: callback must not be empty");

      std::lock_guard<std::mutex> lock (mutex_);

      const subscription_id id (next_id_++);
      entries_.emplace_back (id, std::move (cb));

      return id;
    }

    template <typename Callback>
    bool
    event_dispatcher<Callback>::unsubscribe (subscription_id id) noexcept
    {
      std::lock_guard<std::mutex> lock (mutex_);

      const auto it (std::find_if (entries_.begin (),
                                   entries_.end (),
                                   [id] (const entry& e) noexcept
      {
        return e.id == id;
      }));

      if (it == entries_.end ())
        return false;

      entries_.erase (it);
      return true;
    }

    template <typename Callback>
    std::size_t
    event_dispatcher<Callback>::subscriber_count () const noexcept
    {
      std::lock_guard<std::mutex> lock (mutex_);
      return entries_.size ();
    }

    template <typename Callback>
    template <typename... Args>
    std::size_t
    event_dispatcher<Callback>::dispatch (Args&&... args) const
    {
      // Grab a snapshot of the list while holding the lock. This lets us
      // iterate without keeping the mutex, meaning callbacks can freely add
      // or remove subscriptions without deadlocks.
      //
      std::vector<entry> snapshot;
      {
        std::lock_guard<std::mutex> lock (mutex_);
        snapshot = entries_;
      }

      // We explicitly avoid std::forward here. Since we are calling multiple
      // subscribers, moving the arguments on the first call would ruin them
      // for the subsequent ones.
      //
      for (const entry& e : snapshot)
        e.callback (args...);

      return snapshot.size ();
    }
  }
}
