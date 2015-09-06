#include <string>
#include <vector>
#include <ostream>

#include <jank/translate/function/argument/definition.hpp>

#include <jank/parse/expect/type.hpp>
#include <jank/translate/cell/cell.hpp>
#include <jank/translate/type/generic/extract.hpp>
#include <jank/translate/type/generic/parse.hpp>
#include <jank/translate/type/generic/verify.hpp>
#include <jank/translate/environment/scope.hpp>
#include <jank/translate/environment/builtin/type/normalize.hpp>
#include <jank/translate/expect/error/syntax/exception.hpp>
#include <jank/translate/expect/error/internal/unimplemented.hpp>

namespace jank
{
  namespace translate
  {
    namespace function
    {
      namespace argument
      {
        namespace detail
        {
          bool operator ==(type_list const &lhs, type_list const &rhs)
          {
            return (lhs.size() == rhs.size()) &&
                    std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                               [](auto const &lhs, auto const &rhs)
                               { return lhs.type == rhs.type; });
          }

          std::ostream& operator <<(std::ostream &os, type_list const &args)
          {
            os << "( ";
            for(auto const &a : args)
            { os << a.name << " : " << a.type.definition.name << " "; }
            os << ") ";
            return os;
          }
        }

        namespace definition
        {
          type_list parse_types
          (
            parse::cell::list const &l,
            std::shared_ptr<environment::scope> const &scope
          )
          {
            type_list ret;

            for(auto it(l.data.begin()); it != l.data.end(); ++it)
            {
              auto const &name
              (parse::expect::type<parse::cell::type::ident>(*it).data);
              if(++it == l.data.end())
              {
                throw expect::error::syntax::exception<>
                { "expected type after " + name };
              }

              auto const &type_name
              (parse::expect::type<parse::cell::type::ident>(*it).data);
              auto const &type_def(scope->find_type(type_name));
              if(!type_def)
              {
                throw expect::error::type::exception<>
                { "unknown type " + type_name };
              }

              auto type
              (
                environment::builtin::type::normalize
                (type_def.value().first.data, *scope)
              );
              std::tie(type, it) = type::generic::apply_genericity
              (std::move(type), it, l.data.end(), scope);

              ret.push_back({ name, { type } });
            }

            return ret;
          }
        }
      }
    }
  }
}
