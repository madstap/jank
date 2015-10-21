#pragma once

#include <jank/translate/environment/scope.hpp>

namespace jank
{
  namespace translate
  {
    namespace plugin
    {
      namespace macro
      {
        void emit(std::shared_ptr<environment::scope> const &scope);
      }
    }
  }
}