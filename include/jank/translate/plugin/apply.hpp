#pragma once

#include <jank/translate/environment/scope.hpp>
#include <jank/translate/plugin/io/print.hpp>

namespace jank
{
  namespace translate
  {
    namespace plugin
    {
      /* TODO: Move to cpp */
      inline auto apply(std::shared_ptr<environment::scope> const &scope)
      {
        /* TODO: Refactor to some shared collection of plugins; shared with interpret. */
        std::vector
        <
          std::function<void (std::shared_ptr<environment::scope> const&)>
        > const plugins
        {
          &io::print
        };

        for(auto const &plugin : plugins)
        { plugin(scope); }

        return scope;
      }
    }
  }
}
