#pragma once

#include <iosfwd>
#include <string>

#include <libd3d9/export.hxx>

namespace d3d9
{
  // Print a greeting for the specified name into the specified
  // stream. Throw std::invalid_argument if the name is empty.
  //
  LIBD3D9_SYMEXPORT void
  say_hello (std::ostream&, const std::string& name);
}
