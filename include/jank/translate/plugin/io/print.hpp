#pragma once

#include <jank/translate/environment/scope.hpp>

namespace jank
{
  namespace translate
  {
    namespace plugin
    {
      namespace io
      {
        /* TODO: move to cpp. */
        inline void print(std::shared_ptr<environment::scope> const &scope)
        {
          static_cast<void>(scope);
          /* TODO: DSL for specifying things? */
        }
      }
    }
  }
}
