#pragma once

#include <cassert>
#include <cstddef>
#include <system_error>
#include <utility>

#include <libd3d9/d3d9-win32.hxx>

namespace d3d9
{
  namespace detail
  {
    // RAII guard to patch a single slot in a COM interface vtable. We restore
    // the original pointer when the guard goes out of scope.
    //
    // We achieve this by temporarily marking the vtable memory page as
    // PAGE_EXECUTE_READWRITE using VirtualProtect. Then we do an atomic pointer
    // swap and revert the page protection. Note that this is Windows-specific
    // (x86 and x64).
    //
    // A few invariants to keep in mind:
    //
    // 1. slot_ is not NULL if and only if the guard is active.
    // 2. original_ holds the original pointer that resided in *slot_ before we
    //    patched it.
    // 3. *slot_ holds the replacement pointer for as long as we are active.
    //
    class vtable_guard
    {
    public:
      // Create an empty, inactive guard.
      //
      vtable_guard () noexcept;

      // Patch vtable[index] with replacement and save the original pointer.
      //
      // Note that vtable and replacement must not be NULL. Throws
      // std::system_error if VirtualProtect fails to change the page
      // permissions.
      //
      explicit
      vtable_guard (void** vtable, std::size_t index, void* replacement);

      // Restore the original pointer. If we are already inactive, this is just
      // a no-op.
      //
      ~vtable_guard ();

      vtable_guard (vtable_guard&& other) noexcept;
      vtable_guard& operator= (vtable_guard&& other) noexcept;

      vtable_guard (const vtable_guard&) = delete;
      vtable_guard& operator= (const vtable_guard&) = delete;

      // Return the original function pointer we captured at construction.
      // Returns NULL if we are currently inactive.
      //
      void*
      original () const noexcept;

      // Return true if we have patched the vtable and haven't restored it yet.
      //
      bool
      active () const noexcept;

      // Manually restore the original pointer and deactivate the guard. It is
      // perfectly safe to call this multiple times.
      //
      void
      restore () noexcept;

    private:
      // Write value into slot, wrapping the assignment in VirtualProtect calls
      // so we guarantee we have write access to the page.
      //
      // Note that we don't throw here. If this fails during stack unwinding
      // (e.g., inside restore()), we are dealing with a severe system error and
      // need to tolerate the failure gracefully.
      //
      static void
      write_ptr (void** slot, void* value) noexcept;

      void** slot_;
      void* original_;
    };

    inline
    vtable_guard::vtable_guard () noexcept
      : slot_ (nullptr),
        original_ (nullptr)
    {
    }

    inline void*
    vtable_guard::original () const noexcept
    {
      return original_;
    }

    inline bool
    vtable_guard::active () const noexcept
    {
      return slot_ != nullptr;
    }

    inline
    vtable_guard::vtable_guard (vtable_guard&& other) noexcept
      : slot_ (other.slot_),
        original_ (other.original_)
    {
      other.slot_ = nullptr;
      other.original_ = nullptr;
    }

    inline vtable_guard&
    vtable_guard::operator = (vtable_guard&& other) noexcept
    {
      if (this != &other)
      {
        restore ();

        slot_ = other.slot_;
        original_ = other.original_;

        other.slot_ = nullptr;
        other.original_ = nullptr;
      }

      return *this;
    }
  }
}
