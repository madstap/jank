#include <iostream>
#include <atomic>
#include <set>

#include <boost/core/demangle.hpp>

#include <fmt/core.h>

#include <jank/runtime/obj/vector.hpp>
#include <jank/runtime/obj/persistent_array_map.hpp>
#include <jank/runtime/behavior/numberable.hpp>
#include <jank/analyze/processor.hpp>
#include <jank/analyze/expr/primitive_literal.hpp>
#include <jank/analyze/step/force_boxed.hpp>
#include <jank/result.hpp>

namespace jank::analyze
{
  processor::processor
  (
    runtime::context &rt_ctx
  )
    : rt_ctx{ rt_ctx }
    , root_frame{ make_box<local_frame>(local_frame::frame_type::root, rt_ctx, none) }
  {
    using runtime::obj::symbol;
    auto const make_fn = [this](auto const fn) -> decltype(specials)::mapped_type
    {
      return [this, fn](auto const &list, auto &current_frame, auto const expr_type, auto const &fn_ctx, auto const needs_box)
      { return (this->*fn)(list, current_frame, expr_type, fn_ctx, needs_box); };
    };
    specials =
    {
      { jank::make_box<symbol>("def"), make_fn(&processor::analyze_def) },
      { jank::make_box<symbol>("fn*"), make_fn(&processor::analyze_fn) },
      { jank::make_box<symbol>("recur"), make_fn(&processor::analyze_recur) },
      { jank::make_box<symbol>("do"), make_fn(&processor::analyze_do) },
      { jank::make_box<symbol>("let*"), make_fn(&processor::analyze_let) },
      { jank::make_box<symbol>("if"), make_fn(&processor::analyze_if) },
      { jank::make_box<symbol>("quote"), make_fn(&processor::analyze_quote) },
      { jank::make_box<symbol>("var"), make_fn(&processor::analyze_var) },
      { jank::make_box<symbol>("native/raw"), make_fn(&processor::analyze_native_raw) },
    };
  }

  processor::expression_result processor::analyze
  (
    read::parse::processor::iterator parse_current,
    read::parse::processor::iterator const &parse_end
  )
  {
    if(parse_current == parse_end)
    { return err(error{ "already retrieved result" }); }

    /* We wrap all of the expressions we get in an anonymous fn so that we can call it easily.
     * This also simplifies codegen, since we only ever codegen a single fn, even if that fn
     * represents a ns, a single REPL expression, or an actual source fn. */
    runtime::detail::native_transient_vector fn;
    fn.push_back(make_box<runtime::obj::symbol>("fn*"));
    fn.push_back(make_box<runtime::obj::vector>());
    for(; parse_current != parse_end; ++parse_current)
    {
      if(parse_current->is_err())
      { return err(parse_current->expect_err_move()); }
      fn.push_back(parse_current->expect_ok());
    }
    auto fn_list(make_box<runtime::obj::list>(fn.rbegin(), fn.rend()));
    return analyze(fn_list, expression_type::expression);
  }

  processor::expression_result processor::analyze_def
  (
    runtime::obj::list_ptr const &l,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const
  )
  {
    auto const length(l->count());
    if(length != 2 && length != 3)
    {
      /* TODO: Error handling. */
      return err(error{ "invalid def" });
    }

    auto const sym_obj(l->data.rest().first().unwrap());
    if(sym_obj->type != runtime::object_type::symbol)
    {
      /* TODO: Error handling. */
      return err(error{ "invalid def: name must be a symbol" });
    }

    auto const sym(runtime::expect_object<runtime::obj::symbol>(sym_obj));
    if(!sym->ns.empty())
    {
      /* TODO: Error handling. */
      return err(error{ "invalid def: name must not be qualified" });
    }

    bool has_value{ true };
    auto const value_opt(l->data.rest().rest().first());
    if(value_opt.is_none())
    { has_value = false; }

    auto const qualified_sym(current_frame->lift_var(sym));
    auto const var(rt_ctx.intern_var(qualified_sym));
    if(var.is_err())
    { return var.expect_err(); }

    option<native_box<expression>> value_expr;

    if(has_value)
    {
      auto value_result(analyze(value_opt.unwrap(), current_frame, expression_type::expression, fn_ctx, true));
      if(value_result.is_err())
      { return value_result; }
      value_expr = some(value_result.expect_ok());

      vars.insert_or_assign(var.expect_ok(), value_expr.unwrap());
    }

    return make_box<expression>
    (
      expr::def<expression>
      {
        expression_base{ {}, expr_type, current_frame, true },
        qualified_sym,
        value_expr
      }
    );
  }

  processor::expression_result processor::analyze_symbol
  (
    runtime::obj::symbol_ptr const &sym,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const&,
    native_bool needs_box
  )
  {
    /* TODO: Assert it doesn't start with __. */
    auto found_local(current_frame->find_local_or_capture(sym));
    if(found_local.is_some())
    {
      local_frame::register_captures(found_local.unwrap());

      /* Since we're referring to a local, we're boxed if it is boxed. */
      needs_box |= found_local.unwrap().binding.needs_box;

      /* Captured locals are always boxed, even if the originating local is not. */
      if(!found_local.unwrap().crossed_fns.empty())
      {
        needs_box = true;

        /* Capturing counts as a boxed usage for the originating local. */
        found_local.unwrap().binding.has_boxed_usage = true;

        /* The first time we reference a captured local from within a function, we get here.
         * We determine that we had to cross one or more function scopes to find the relevant
         * local, so it's a new capture. We register the capture above, but we need to search
         * again to get the binding within our current function, since the one we have now
         * is the originating binding.
         *
         * All future lookups for this capatured local, in this function, will skip this branch. */
        found_local = current_frame->find_local_or_capture(sym);
      }

      if(needs_box)
      { found_local.unwrap().binding.has_boxed_usage = true; }
      else
      { found_local.unwrap().binding.has_unboxed_usage = true; }

      return make_box<expression>
      (
        expr::local_reference
        {
          expression_base{ {}, expr_type, current_frame, needs_box },
          sym,
          found_local.unwrap().binding
        }
      );
    }

    auto const qualified_sym(rt_ctx.qualify_symbol(sym));
    auto const var(rt_ctx.find_var(qualified_sym));
    if(var.is_none())
    { return err(error{ "unbound symbol: " + sym->to_string() }); }

    /* Macros aren't lifted, since they're not used during runtime. */
    auto const unwrapped_var(var.unwrap());
    auto const macro_kw(rt_ctx.intern_keyword("", "macro", true));
    if
    (
      unwrapped_var->meta.is_none() ||
      get(unwrapped_var->meta.unwrap(), macro_kw) == runtime::obj::nil::nil_const()
    )
    { current_frame->lift_var(qualified_sym); }
    return make_box<expression>
    (
      expr::var_deref<expression>
      {
        expression_base{ {}, expr_type, current_frame },
        qualified_sym,
        unwrapped_var
      }
    );
  }

  result<expr::function_arity<expression>, error> processor::analyze_fn_arity
  (
    runtime::obj::list_ptr const &list,
    local_frame_ptr &current_frame
  )
  {
    auto const params_obj(list->data.first().unwrap());
    if(params_obj->type != runtime::object_type::vector)
    { return err(error{ "invalid fn parameter vector" }); }

    auto const params(runtime::expect_object<runtime::obj::vector>(params_obj));

    local_frame_ptr frame
    { make_box<local_frame>(local_frame::frame_type::fn, current_frame->rt_ctx, current_frame) };
    native_vector<runtime::obj::symbol_ptr> param_symbols;
    param_symbols.reserve(params->data.size());
    std::set<runtime::obj::symbol> unique_param_symbols;

    bool is_variadic{};
    for(auto it(params->data.begin()); it != params->data.end(); ++it)
    {
      auto const p(*it);
      if(p->type != runtime::object_type::symbol)
      { return err(error{ "invalid parameter; must be a symbol" }); }

      auto const sym(runtime::expect_object<runtime::obj::symbol>(p));
      if(!sym->ns.empty())
      { return err(error{ "invalid parameter; must be unqualified" }); }
      else if(sym->name == "&")
      {
        if(is_variadic)
        { return err(error{ "invalid function; parameters contain mutliple &" }); }
        else if(it + 1 == params->data.end())
        { return err(error{ "invalid function; missing symbol after &" }); }
        else if(it + 2 != params->data.end())
        { return err(error{ "invalid function; param after rest args" }); }

        is_variadic = true;
        continue;
      }

      auto const unique_res(unique_param_symbols.emplace(*sym));
      if(!unique_res.second)
      {
        /* TODO: Output a warning here. */
        for(auto const &param : param_symbols)
        {
          if(param->equal(*sym))
          {
            /* C++ doesn't allow multiple params with the same name, but it does allow params
             * without any name. So, if we have a param shadowing another, we just remove the
             * name of the one being shadowed. This is better than generating a new name for
             * it, since we don't want it referenced at all. */
            param->name = "";
            break;
          }
        }
      }

      frame->locals.emplace(sym, local_binding{ sym, none });
      param_symbols.emplace_back(sym);
    }

    /* We do this after building the symbols vector, since the & symbol isn't a param
     * and would cause an off-by-one error. */
    if(param_symbols.size() > runtime::max_params)
    {
      return err
      (
        error
        {
          fmt::format
          (
            "invalid parameter count; must be <= {}; use & args to capture the rest",
            runtime::max_params
          )
        }
      );
    }

    auto fn_ctx(make_box<expr::function_context>());
    fn_ctx->is_variadic = is_variadic;
    fn_ctx->param_count = param_symbols.size();
    expr::do_<expression> body_do{ expression_base{ {}, expression_type::return_statement, frame } };
    size_t const form_count{ list->count() - 1 };
    size_t i{};
    for(auto const &item : list->data.rest())
    {
      auto const expr_type
      ((++i == form_count) ? expression_type::return_statement : expression_type::statement);
      auto form(analyze(item, frame, expr_type, fn_ctx, expr_type != expression_type::statement));
      if(form.is_err())
      { return form.expect_err_move(); }
      body_do.body.emplace_back(form.expect_ok());
    }

    /* If it turns out this function uses recur, we need to ensure that its tail expression
     * is boxed. This is because unboxed values may use IIFE for initialization, which will
     * not work with the generated while/continue we use for recursion. */
    if(fn_ctx->is_tail_recursive)
    { body_do = step::force_boxed(std::move(body_do)); }

    return
    {
      expr::function_arity<expression>
      {
        std::move(param_symbols),
        std::move(body_do),
        std::move(frame),
        std::move(fn_ctx)
      }
    };
  }

  processor::expression_result processor::analyze_fn
  (
    runtime::obj::list_ptr const &full_list,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const&,
    native_bool const
  )
  {
    auto const length(full_list->count());
    if(length < 2)
    { return err(error{ "fn missing forms" }); }
    auto list(full_list);

    native_string name;
    auto first_elem(list->data.rest().first().unwrap());
    if(first_elem->type == runtime::object_type::symbol)
    {
      auto const s(runtime::expect_object<runtime::obj::symbol>(first_elem));
      /* TODO: Remove the generated portion here once we support codegen for making all references
       * to generated code use the fully qualified name. Right now, a jank fn named `min` will
       * conflict with the RT `min` fn, for example. */
      name = runtime::context::unique_string(s->name);
      first_elem = list->data.rest().rest().first().unwrap();
      list = make_box(list->data.rest());
    }
    else
    { name = runtime::context::unique_string("fn"); }
    name = runtime::munge(name);

    native_vector<expr::function_arity<expression>> arities;

    if(first_elem->type == runtime::object_type::vector)
    {
      auto result
      (
        analyze_fn_arity
        (make_box<runtime::obj::list>(list->data.rest()), current_frame)
      );
      if(result.is_err())
      { return result.expect_err_move(); }
      arities.emplace_back(result.expect_ok_move());
    }
    else if(first_elem->type == runtime::object_type::list)
    {
      for(auto it(list->data.rest()); it.size() > 0; it = it.rest())
      {
        auto arity_list_obj(it.first().unwrap());
        if(arity_list_obj->type != runtime::object_type::list)
        { return err(error{ "invalid fn: expected arity list" }); }
        auto arity_list(runtime::expect_object<runtime::obj::list>(arity_list_obj));

        auto result(analyze_fn_arity(arity_list.data, current_frame));
        if(result.is_err())
        { return result.expect_err_move(); }
        arities.emplace_back(result.expect_ok_move());
      }
    }
    else
    { return err(error{ "invalid fn syntax" }); }

    /* There can only be one variadic arity. Clojure requires this. */
    size_t found_variadic{};
    size_t variadic_arity{};
    for(auto const &arity : arities)
    {
      found_variadic += static_cast<int>(arity.fn_ctx->is_variadic);
      variadic_arity = arity.params.size();
    }
    if(found_variadic > 1)
    { return err(error{ "invalid fn: has more than one variadic arity" }); }

    /* The variadic arity, if present, must have at least as many fixed params as the
     * highest non-variadic arity. Clojure requires this. */
    if(found_variadic > 0)
    {
      for(auto const &arity : arities)
      {
        if(!arity.fn_ctx->is_variadic && arity.params.size() >= variadic_arity)
        { return err(error{ "invalid fn: fixed arity has >= params than variadic arity" }); }
      }
    }

    /* Assert that arities are unique. Lazy implementation, but N is small anyway. */
    for(auto base(arities.begin()); base != arities.end(); ++base)
    {
      if(base + 1 == arities.end())
      { break; }

      for(auto other(base + 1); other != arities.end(); ++other)
      {
        if
        (
          base->params.size() == other->params.size()
          && base->fn_ctx->is_variadic == other->fn_ctx->is_variadic
        )
        { return err(error{ "invalid fn: duplicate arity definition" }); }
      }
    }

    auto ret
    (
      make_box<expression>
      (
        expr::function<expression>
        {
          expression_base{ {}, expr_type, current_frame },
          name,
          std::move(arities)
        }
      )
    );

    if(rt_ctx.compiling)
    {
      /* Register this module as a dependency of the current module so we can generate
       * code to load it. */
      auto const &ns_sym(make_box<runtime::obj::symbol>("clojure.core/*ns*"));
      auto const &ns_var(rt_ctx.find_var(ns_sym).unwrap());
      auto const module
      (
        runtime::module::nest_module
        (
          runtime::detail::to_string(ns_var->get_root()),
          runtime::munge(name)
        )
      );
      rt_ctx.module_dependencies[rt_ctx.current_module].emplace_back(module);
      fmt::println("module dep {} -> {}", rt_ctx.current_module, module);

      codegen::processor cg_prc{ rt_ctx, ret, module, codegen::compilation_target::function };
      rt_ctx.write_module(module, cg_prc.declaration_str());
    }

    return ret;
  }

  processor::expression_result processor::analyze_recur
  (
    runtime::obj::list_ptr const &list,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const
  )
  {
    if(fn_ctx.is_none())
    { return err(error{ "unable to use recur outside of a function or loop" }); }
    else if(expr_type != expression_type::return_statement)
    { return err(error{ "recur used outside of tail position" }); }

    /* Minus one to remove recur symbol. */
    auto const arg_count(list->count() - 1);
    if(fn_ctx.unwrap()->param_count != arg_count)
    {
      return err
      (
        error
        {
          fmt::format
          (
            "invalid number of args passed to recur; expected {}, found {}",
            fn_ctx.unwrap()->param_count,
            arg_count
          )
        }
      );
    }


    native_vector<expression_ptr> arg_exprs;
    arg_exprs.reserve(arg_count);
    for(auto const &form : list->data.rest())
    {
      auto arg_expr(analyze(form, current_frame, expression_type::expression, fn_ctx, true));
      if(arg_expr.is_err())
      { return arg_expr; }
      arg_exprs.emplace_back(arg_expr.expect_ok());
    }

    fn_ctx.unwrap()->is_tail_recursive = true;

    return make_box<expression>
    (
      expr::recur<expression>
      {
        expression_base{ {}, expr_type, current_frame },
        jank::make_box<runtime::obj::list>(list->data.rest()),
        arg_exprs
      }
    );
  }

  processor::expression_result processor::analyze_do
  (
    runtime::obj::list_ptr const &list,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const needs_box
  )
  {
    expr::do_<expression> ret{ expression_base{ {}, expr_type, current_frame }, {} };
    size_t const form_count{ list->count() - 1 };
    size_t i{};
    for(auto const &item : list->data.rest())
    {
      auto const last(++i == form_count);
      auto const form_type(last ? expr_type : expression_type::statement);
      auto form
      (
        analyze
        (
          item,
          current_frame,
          form_type,
          fn_ctx,
          form_type == expression_type::statement ? false : needs_box
        )
      );
      if(form.is_err())
      { return form.expect_err_move(); }

      if(last)
      { ret.needs_box = form.expect_ok_ptr()->data->get_base()->needs_box; }

      ret.body.emplace_back(form.expect_ok());
    }

    return make_box<expression>(std::move(ret));
  }

  processor::expression_result processor::analyze_let
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const needs_box
  )
  {
    if(o->count() < 2)
    { return err(error{ "invalid let: expects bindings" }); }

    auto const bindings_obj(o->data.rest().first().unwrap());
    if(bindings_obj->type != runtime::object_type::vector)
    { return err(error{ "invalid let* bindings: must be a vector" }); }

    auto const bindings(runtime::expect_object<runtime::obj::vector>(bindings_obj));

    auto const binding_parts(bindings->data.size());
    if(binding_parts % 2 == 1)
    { return err(error{ "invalid let* bindings: must be an even number" }); }

    expr::let<expression> ret
    {
      expr_type,
      needs_box,
      make_box<local_frame>
      (local_frame::frame_type::let, current_frame->rt_ctx, current_frame)
    };
    for(size_t i{}; i < binding_parts; i += 2)
    {
      auto const &sym_obj(bindings->data[i]);
      auto const &val(bindings->data[i + 1]);

      auto const &sym(runtime::expect_object<runtime::obj::symbol>(sym_obj));
      if(sym_obj->type != runtime::object_type::symbol || !sym->ns.empty())
      { return err(error{ "invalid let* binding: left hand must be an unqualified symbol" }); }

      auto res(analyze(val, ret.frame, expression_type::expression, fn_ctx, false));
      if(res.is_err())
      { return res.expect_err_move(); }
      auto it(ret.pairs.emplace_back(sym, res.expect_ok_move()));
      ret.frame->locals.emplace
      (
        sym,
        local_binding
        { sym, some(it.second), current_frame, it.second->get_base()->needs_box }
      );
    }

    size_t const form_count{ o->count() - 2 };
    size_t i{};
    for(auto const &item : o->data.rest().rest())
    {
      auto const last(++i == form_count);
      auto const form_type(last ? expr_type : expression_type::statement);
      auto res(analyze(item, ret.frame, form_type, fn_ctx, needs_box));
      if(res.is_err())
      { return res.expect_err_move(); }

      /* Ultimately, whether or not this let is boxed is up to the last form. */
      if(last)
      { ret.needs_box = res.expect_ok_ptr()->data->get_base()->needs_box; }

      ret.body.body.emplace_back(res.expect_ok_move());
    }

    return make_box<expression>(std::move(ret));
  }

  processor::expression_result processor::analyze_if
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool needs_box
  )
  {
    /* We can't (yet) guarantee that each branch of an if returns the same unboxed type,
     * so we're unable to unbox them. */
    needs_box = true;

    auto const form_count(o->count());
    if(form_count < 3)
    { return err(error{ "invalid if: expects at least two forms" }); }
    else if(form_count > 4)
    { return err(error{ "invalid if: expects at most three forms" }); }

    auto const condition(o->data.rest().first().unwrap());
    auto condition_expr(analyze(condition, current_frame, expression_type::expression, fn_ctx, false));
    if(condition_expr.is_err())
    { return condition_expr.expect_err_move(); }

    auto const then(o->data.rest().rest().first().unwrap());
    auto then_expr(analyze(then, current_frame, expr_type, fn_ctx, needs_box));
    if(then_expr.is_err())
    { return then_expr.expect_err_move(); }

    option<expression_ptr> else_expr_opt;
    if(form_count == 4)
    {
      auto const else_(o->data.rest().rest().rest().first().unwrap());
      auto else_expr(analyze(else_, current_frame, expr_type, fn_ctx, needs_box));
      if(else_expr.is_err())
      { return else_expr.expect_err_move(); }

      else_expr_opt = else_expr.expect_ok();
    }

    return make_box<expression>
    (
      expr::if_<expression>
      {
        expression_base{ {}, expr_type, current_frame, needs_box },
        condition_expr.expect_ok(),
        then_expr.expect_ok(),
        else_expr_opt
      }
    );
  }

  processor::expression_result processor::analyze_quote
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const needs_box
  )
  {
    if(o->count() != 2)
    { return err(error{ "invalid quote: expects one argument" }); }

    return analyze_primitive_literal(o->data.rest().first().unwrap(), current_frame, expr_type, fn_ctx, needs_box);
  }

  processor::expression_result processor::analyze_var
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const&,
    native_bool const
  )
  {
    if(o->count() != 2)
    { return err(error{ "invalid var reference: expects one argument" }); }

    auto const &arg(o->data.rest().first().unwrap());
    if(arg->type != runtime::object_type::symbol)
    { return err(error{ "invalid var reference: expects a symbol" }); }

    auto const arg_sym(runtime::expect_object<runtime::obj::symbol>(arg));

    auto const qualified_sym(rt_ctx.qualify_symbol(arg_sym));
    auto const found_var(rt_ctx.find_var(qualified_sym));
    if(found_var.is_none())
    { return err(error{ "invalid var reference: var not found" }); }

    return make_box<expression>
    (
      expr::var_ref<expression>
      {
        expression_base{ {}, expr_type, current_frame, true },
        qualified_sym,
        found_var.unwrap()
      }
    );
  }

  processor::expression_result processor::analyze_native_raw
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const
  )
  {
    if(o->count() != 2)
    { return err(error{ "invalid native/raw: expects one argument" }); }

    auto const &code(o->data.rest().first().unwrap());
    if(code->type != runtime::object_type::string)
    { return err(error{ "invalid native/raw: expects string of C++ code" }); }

    auto const code_str(runtime::expect_object<runtime::obj::string>(code));
    if(code_str->data.empty())
    {
      return make_box<expression>
      (
        expr::native_raw<expression>
        {
          expression_base{ {}, expr_type, current_frame, true },
          {}
        }
      );
    }

    /* native/raw expressions are broken up into chunks of either literal C++ code or
     * interpolated jank code, the latter needing to also be analyzed. */
    decltype(expr::native_raw<expression>::chunks) chunks;
    /* TODO: Just use } for end and rely on token parsing info for when that is.
     * This requires storing line/col start/end meta in each object. */
    constexpr native_string_view interp_start{ "#{" }, interp_end{ "}#" };
    for(size_t it{}; it != native_string::npos; )
    {
      auto const next_start(code_str->data.find(interp_start.data(), it));
      if(next_start == native_string::npos)
      {
        /* This is the final chunk. */
        chunks.emplace_back(native_string_view{ code_str->data.data() + it });
        break;
      }
      auto const next_end(code_str->data.find(interp_end.data(), next_start));
      if(next_end == native_string::npos)
      { return err(error{ fmt::format("no matching {} found for native/raw interpolation", interp_end) }); }

      read::lex::processor l_prc
      {
        {
          code_str->data.data() + next_start + interp_start.size(),
          next_end - next_start - interp_end.size()
        }
      };
      read::parse::processor p_prc{ rt_ctx, l_prc.begin(), l_prc.end() };
      auto parsed_it(p_prc.begin());
      if(parsed_it->is_err())
      { return parsed_it->expect_err_move(); }
      auto result(analyze(parsed_it->expect_ok(), current_frame, expression_type::expression, fn_ctx, true));
      if(result.is_err())
      { return result.expect_err_move(); }

      if(next_start - it > 0)
      { chunks.emplace_back(native_string_view{ code_str->data.data() + it, next_start - it }); }
      chunks.emplace_back(result.expect_ok());
      it = next_end + interp_end.size();

      if(++parsed_it != p_prc.end())
      { return err(error{ "invalid native/raw: only one expression per interpolation" }); }
    }

    return make_box<expression>
    (
      expr::native_raw<expression>
      {
        expression_base{ {}, expr_type, current_frame, true },
        std::move(chunks)
      }
    );
  }

  processor::expression_result processor::analyze_primitive_literal
  (
    runtime::object_ptr o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const&,
    native_bool const needs_box
  )
  {
    current_frame->lift_constant(o);
    return make_box<expression>
    (
      expr::primitive_literal<expression>
      {
        expression_base{ {}, expr_type, current_frame, needs_box },
        o
      }
    );
  }

  /* TODO: Test for this. */
  processor::expression_result processor::analyze_vector
  (
    runtime::obj::vector_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const
  )
  {
    native_vector<expression_ptr> exprs;
    exprs.reserve(o->count());
    bool literal{ true };
    for(auto d = o->seq(); d != nullptr; d = d->next_in_place())
    {
      auto res(analyze(d->first(), current_frame, expression_type::expression, fn_ctx, true));
      if(res.is_err())
      { return res.expect_err_move(); }
      exprs.emplace_back(res.expect_ok_move());
      if(!boost::get<expr::primitive_literal<expression>>(&exprs.back()->data))
      { literal = false; }
    }

    if(literal)
    {
      /* TODO: Order lifted constants. Use sub constants during codegen. */
      current_frame->lift_constant(o);
      return make_box<expression>
      (
        expr::primitive_literal<expression>
        {
          expression_base{ {}, expr_type, current_frame, true },
          o
        }
      );
    }

    return make_box<expression>
    (
      expr::vector<expression>
      {
        expression_base{ {}, expr_type, current_frame, true },
        std::move(exprs)
      }
    );
  }

  processor::expression_result processor::analyze_map
  (
    runtime::obj::persistent_array_map_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const
  )
  {
    /* TODO: Detect literal and act accordingly. */
    native_vector<std::pair<expression_ptr, expression_ptr>> exprs;
    exprs.reserve(o->data.size());
    for(auto const &kv : o->data)
    {
      auto k_expr(analyze(kv.first, current_frame, expression_type::expression, fn_ctx, true));
      if(k_expr.is_err())
      { return k_expr.expect_err_move(); }
      auto v_expr(analyze(kv.second, current_frame, expression_type::expression, fn_ctx, true));
      if(v_expr.is_err())
      { return v_expr.expect_err_move(); }
      exprs.emplace_back(k_expr.expect_ok_move(), v_expr.expect_ok_move());
    }

    /* TODO: Uniqueness check. */
    return make_box<expression>
    (
      expr::map<expression>
      {
        expression_base{ {}, expr_type, current_frame, true },
        std::move(exprs)
      }
    );
  }

  processor::expression_result processor::analyze_call
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const &fn_ctx,
    native_bool const needs_box
  )
  {
    /* An empty list evaluates to a list, not a call. */
    auto const count(o->count());
    if(count == 0)
    { return analyze_primitive_literal(o, current_frame, expr_type, fn_ctx, needs_box); }

    auto const arg_count(count - 1);

    auto const first(o->data.first().unwrap());
    expression_ptr source{};
    native_bool needs_ret_box{ true };
    native_bool needs_arg_box{ true };
    if(first->type == runtime::object_type::symbol)
    {
      auto const sym(runtime::expect_object<runtime::obj::symbol>(first));
      auto const found_special(specials.find(sym));
      if(found_special != specials.end())
      { return found_special->second(o, current_frame, expr_type, fn_ctx, needs_box); }

      auto sym_result(analyze_symbol(sym, current_frame, expression_type::expression, fn_ctx, true));
      if(sym_result.is_err())
      { return sym_result; }

      /* If this is a macro, recur so we can start over. */
      auto const expanded(rt_ctx.macroexpand(o));
      if(expanded != o)
      { return analyze(expanded, current_frame, expr_type, fn_ctx, needs_box); }

      source = sym_result.expect_ok();
      auto var_deref(boost::get<expr::var_deref<expression>>(&source->data));

      /* If this expression doesn't need to be boxed, based on where it's called, we can dig
       * into the call details itself to see if the function supports unboxed returns. Most don't. */
      if(var_deref && var_deref->var->meta.is_some())
      {
        auto const arity_meta
        (
          runtime::get_in
          (
            var_deref->var->meta.unwrap(),
            make_box<runtime::obj::vector>
            (
              rt_ctx.intern_keyword("", "arities", true),
              make_box(arg_count)
            )
          )
        );

        native_bool const supports_unboxed_input
        (
          runtime::detail::truthy
          (get(arity_meta, rt_ctx.intern_keyword("", "supports-unboxed-input?", true)))
        );
        native_bool const supports_unboxed_output
        (
          runtime::detail::truthy
          /* TODO: Rename key. */
          (get(arity_meta, rt_ctx.intern_keyword("", "unboxed-output?", true)))
        );

        if(supports_unboxed_input || supports_unboxed_output)
        {
          auto const fn_res(vars.find(var_deref->var));
          if(fn_res == vars.end())
          { return err(error{ fmt::format("ICE: undefined var: {}", var_deref->var->to_string()) }); }

          auto const fn(boost::get<expr::function<expression>>(&fn_res->second->data));
          if(!fn)
          { return err(error{ "unsupported arity meta on non-function var" }); }

          /* We need to be sure we're calling the exact arity that has been specified. Unboxed
           * returns aren't supported for variadic calls right now. */
          for(auto const &arity : fn->arities)
          {
            if(arity.fn_ctx->param_count == arg_count && !arity.fn_ctx->is_variadic)
            {
              needs_arg_box = !supports_unboxed_input;
              needs_ret_box = needs_box | !supports_unboxed_output;
              break;
            }
          }
        }
      }
    }
    else
    {
      auto callable_expr(analyze(first, current_frame, expression_type::expression, fn_ctx, needs_box));
      if(callable_expr.is_err())
      { return callable_expr; }
      source = callable_expr.expect_ok_move();
    }

    native_vector<expression_ptr> arg_exprs;
    arg_exprs.reserve(arg_count);
    for(auto const &s : o->data.rest())
    {
      auto arg_expr(analyze(s, current_frame, expression_type::expression, fn_ctx, needs_arg_box));
      if(arg_expr.is_err())
      { return arg_expr; }
      arg_exprs.emplace_back(arg_expr.expect_ok());
    }

    return make_box<expression>
    (
      expr::call<expression>
      {
        expression_base{ {}, expr_type, current_frame, needs_ret_box },
        source,
        jank::make_box<runtime::obj::list>(o->data.rest()),
        arg_exprs
      }
    );
  }

  processor::expression_result processor::analyze
  (
    runtime::object_ptr o,
    expression_type const expr_type
  )
  { return analyze(o, root_frame, expr_type, none, true); }

  processor::expression_result processor::analyze
  (
    runtime::object_ptr o,
    local_frame_ptr &current_frame,
    expression_type const expr_type,
    option<expr::function_context_ptr> const& fn_ctx,
    native_bool const needs_box
  )
  {
    if(o == nullptr)
    { return err(error{ "unexpected nullptr" }); }

    return runtime::visit_object
    (
      [&](auto const typed_o) -> processor::expression_result
      {
        using T = typename decltype(typed_o)::value_type;

        if constexpr(std::same_as<T, runtime::obj::list>)
        { return analyze_call(typed_o, current_frame, expr_type, fn_ctx, needs_box); }
        else if constexpr(std::same_as<T, runtime::obj::vector>)
        { return analyze_vector(typed_o, current_frame, expr_type, fn_ctx, needs_box); }
        else if constexpr(std::same_as<T, runtime::obj::persistent_array_map>)
        { return analyze_map(typed_o, current_frame, expr_type, fn_ctx, needs_box); }
        else if constexpr(std::same_as<T, runtime::obj::set>)
        { return err(error{ "unimplemented analysis: set" }); }
        else if constexpr
        (
          runtime::behavior::numberable<T>
          || std::same_as<T, runtime::obj::boolean>
          || std::same_as<T, runtime::obj::keyword>
          || std::same_as<T, runtime::obj::nil>
          || std::same_as<T, runtime::obj::string>
        )
        { return analyze_primitive_literal(o, current_frame, expr_type, fn_ctx, needs_box); }
        else if constexpr(std::same_as<T, runtime::obj::symbol>)
        { return analyze_symbol(typed_o, current_frame, expr_type, fn_ctx, needs_box); }
        /* This is used when building code from macros; they may end up being other forms of sequences
         * and not just lists. */
        if constexpr(runtime::behavior::seqable<T>)
        { return analyze_call(runtime::obj::list::create(typed_o->seq()), current_frame, expr_type, fn_ctx, needs_box); }
        else
        {
          std::cerr << fmt::format
          (
            "unsupported analysis of type {} with value {}\n",
            boost::core::demangle(typeid(T).name()),
            typed_o->to_string()
          );
          return err(error{ "unimplemented analysis" });
        }
      },
      o
    );
  }
}
