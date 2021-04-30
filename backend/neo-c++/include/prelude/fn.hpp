#pragma once

#include <prelude/object.hpp>

namespace jank
{
  namespace detail
  {
    struct function_type
    {
      template <typename T>
      using value_type = std::function<T>;

      template <typename R, typename... Args>
      function_type(R (* const f)(Args...)) : function_type(value_type<R (Args...)>{ f })
      { }
      template <typename R, typename... Args>
      function_type(value_type<R (Args...)> &&f) : value{ std::move(f) }
      { }
      template <typename R, typename... Args>
      function_type(value_type<R (Args...)> const &f) : value{ f }
      { }

      template <typename F>
      F const* get() const
      { return std::any_cast<F>(&value); }

      std::any value;
    };
  }

  template <size_t Arity>
  struct invalid_arity
  { };

  struct callable
  {
    virtual object_ptr call() const;
    virtual object_ptr call(object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const;
    virtual object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const;
  };

  struct function : object, callable
  {
    function() = default;
    function(function &&) = default;
    function(function const &) = default;
    function(detail::function_type &&d)
      : data{ std::move(d) }
    { }
    function(detail::function_type const &d)
      : data{ d }
    { }

    detail::boolean_type equal(object const &) const override;
    detail::string_type to_string() const override;
    detail::integer_type to_hash() const override;

    function const* as_function() const override;
    callable const* as_callable() const override;

    object_ptr call() const override;
    object_ptr call(object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const override;
    object_ptr call(object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&, object_ptr const&) const override;

    detail::function_type data;
  };

  namespace detail
  {
    template <typename F, typename... Args>
    object_ptr invoke(F const &f, Args &&... args)
    {
      if constexpr(std::is_function_v<std::remove_pointer_t<std::decay_t<decltype(f)>>>)
      { return f(std::forward<Args>(args)...); }
      else
      {
        auto const * const c((*f)->as_callable());

        if(c)
        { return c->call(std::forward<Args>(args)...); }
        else
        {
          /* TODO: Throw error. */
          std::cout << "(invoke) object is not callable: " << **f << std::endl;
          return JANK_NIL;
        }
      }
    }
  }
}