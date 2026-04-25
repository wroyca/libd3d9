#pragma once

#include <functional>
#include <utility>

#include <libd3d9/export.hxx>

namespace d3d9
{
  class device;

  // RAII handle for a single event subscription.
  //
  // Destroying or resetting the token automatically cancels the subscription
  // and removes the callback from the dispatcher. We make tokens move-only so
  // we don't accidentally get double-cancellations.
  //
  // A quick note on lifetimes: the token must not outlive the object that
  // created it. It holds a raw pointer to an internal dispatcher, so if the
  // owning object is destroyed first the pointer goes dangling. In practice,
  // store tokens as members of an object whose lifetime is strictly nested
  // inside the subscription source.
  //
  // Thread safety is fairly straightforward here. You can call reset() (and
  // thus the destructor) from any thread. Just don't call it concurrently on
  // the exact same token instance.
  //
  class LIBD3D9_SYMEXPORT subscription_token
  {
  public:
    subscription_token () noexcept = default;
    ~subscription_token ();

    subscription_token (subscription_token&&) noexcept;
    subscription_token& operator= (subscription_token&&) noexcept;

    subscription_token (const subscription_token&) = delete;
    subscription_token& operator= (const subscription_token&) = delete;

    // Return true if we still have an active subscription.
    //
    explicit
    operator bool () const noexcept;

    // Cancel the subscription right now.
    //
    // Once you call this, the callback will never be invoked again (assuming
    // no concurrent dispatch is currently running). Calling it on a token
    // that's already inactive is perfectly fine and just does nothing.
    //
    void
    reset () noexcept;

    // The idea here is to provide an internal factory so that event sources
    // within the library can construct tokens. We want to achieve this without
    // exposing the constructor that takes the cancellation function directly in
    // the public interface.
    //
    using cancel_fn = std::function<void ()>;

    static subscription_token
    make (cancel_fn fn) noexcept
    {
      return subscription_token (std::move (fn));
    }

  private:
    friend class device;
    friend class factory;

    explicit subscription_token (cancel_fn) noexcept;

    cancel_fn cancel_;
  };

  // Inline implementations.
  //
  inline subscription_token::
  subscription_token (cancel_fn fn) noexcept
    : cancel_ (std::move (fn))
  {
  }

  inline subscription_token::
  ~subscription_token ()
  {
    reset ();
  }

  inline subscription_token::
  subscription_token (subscription_token&& other) noexcept
    : cancel_ (std::move (other.cancel_))
  {
  }

  inline subscription_token&
  subscription_token::operator = (subscription_token&& other) noexcept
  {
    if (this != &other)
    {
      reset ();
      cancel_ = std::move (other.cancel_);
    }

    return *this;
  }

  inline subscription_token::
  operator bool () const noexcept
  {
    return static_cast<bool> (cancel_);
  }

  inline void
  subscription_token::reset () noexcept
  {
    if (cancel_)
    {
      cancel_ ();
      cancel_ = nullptr;
    }
  }
}
