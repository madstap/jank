#pragma once

#include <iostream>
#include <sstream>
#include <stdexcept>

#include "prelude/primitive.hpp"
#include "prelude/assert.hpp"

namespace jank
{
  /* -- Output -- */
  template <typename T>
  string print_gen_bang(T const &t)
  {
    std::stringstream ss;
    ss << t;
    std::cout << ss.rdbuf() << std::endl;
    return ss.str();
  }

  /* -- Input -- */
  string input_gen_bang()
  {
    string s;
    std::getline(std::cin, s);
    return s;
  }

  /* -- Primitive arithmetic -- */
  template <typename A, typename B>
  auto _gen_plus(A const &a, B const &b)
  { return a + b; }

  template <typename A, typename B>
  auto _gen_minus(A const &a, B const &b)
  { return a - b; }

  auto operator *(string s, integer const n)
  {
    assert_bang(n > 0, "invalid scalar for string repetition");
    auto const original_size(s.size());
    s.reserve(s.size() * n);

    for(integer i{ 1 }; i < n; ++i)
    { s.insert(original_size * i, s.c_str(), original_size); }

    return s;
  }

  template <typename A, typename B>
  auto _gen_asterisk(A const &a, B const &b)
  { return a * b; }

  template <typename A, typename B>
  auto _gen_slash(A const &a, B const &b)
  { return a / b; }
}
