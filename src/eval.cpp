#include "eval.hpp"

#include <chrono>

#include <boost/container/small_vector.hpp>

#include "alloca.hpp"
#include "cval.hpp"
#include "decl.hpp"
#include "globals.hpp"
#include "file.hpp"
#include "options.hpp"
#include "ir_decl.hpp"
#include "rpn.hpp"
#include "stmt.hpp"

namespace sc = std::chrono;
namespace bc = boost::container;

// Data associated with each block node, to be used when making IRs.
struct block_d
{
    // Variables are mapped to a range in this order:
    // 1) local variables
    // 2) global variables
    // 3) global variable sets
    // The following sets use this order.

    // An array of size 'num_fn_vars()'
    // Keeps track of which ssa node a var refers to.
    // A handle of {0} means the local var isn't in the block.
    ssa_value_t* fn_vars = nullptr;

    // An array of size 'num_fn_vars()'
    // Phi nodes in the block which have yet to be sealed.
    ssa_value_t* unsealed_phis = nullptr;

    // Only used for labels.
    pstring_t label_name = {};

    // A CFG node is sealed when all its predecessors are set.
    constexpr bool sealed() const { return unsealed_phis == nullptr; }
};

class eval_t
{
private:
    pstring_t pstring = {};
    fn_t const* fn = nullptr;
    stmt_t const* stmt = nullptr;
    ir_t* ir = nullptr;
    bc::small_vector<cval_t, 8> interpret_locals;
    bc::small_vector<cval_t, 8> compile_locals;
    bc::small_vector<type_t, 8> local_types;

    using clock = sc::steady_clock;
    sc::time_point<clock> start_time;

    inline static thread_local array_pool_t<ssa_value_t> input_pool;
public:
    cpair_t final_result;

    enum do_t
    {
        CHECK,        // Resolves types, but not values.
        INTERPRET_CE, // Like INTERPRET, but can't read/write locals.
        INTERPRET,    // Calculates values at compile time.
        COMPILE       // Generates the SSA IR.
    };

    static constexpr bool is_interpret(do_t d) { return d == INTERPRET_CE || d == INTERPRET; }

    template<do_t Do>
    struct do_wrapper_t { static constexpr auto D = Do; };

    template<eval_t::do_t D>
    eval_t(do_wrapper_t<D>, pstring_t pstring, token_t const* expr, type_t expected_type = TYPE_VOID);

    template<do_t D>
    eval_t(do_wrapper_t<D>, pstring_t pstring, fn_t const& fn, cval_t const* args);

    struct access_t
    {
        type_t type = {};
        unsigned member = 0;
        int index = -1;
    };

    cval_t to_cval(rpn_value_t const& rpn_value) const;
    cval_t const& root_cval(rpn_value_t const& rpn_value) const;
    type_t root_type(rpn_value_t const& rpn_value) const;
    ssa_value_t local_leaf(rpn_value_t const& rpn_value, access_t a) const;

    void check_time();

    template<do_t D>
    void interpret_stmts();

    template<do_t D>
    void do_expr(rpn_stack_t& rpn_stack, token_t const* expr);

    template<do_t D>
    void do_expr_result(token_t const* expr, type_t expected_result);

    template<do_t D>
    void do_assign(rpn_stack_t& rpn_stack, token_t const& token);

    template<typename Policy>
    void do_compare(rpn_stack_t& rpn_stack, token_t const& token);

    template<typename Policy>
    void do_arith(rpn_stack_t& rpn_stack, token_t const& token);

    template<typename Policy>
    void do_shift(rpn_stack_t& rpn_stack, token_t const& token);

    template<typename Policy>
    void interpret_shift(rpn_stack_t& rpn_stack, token_t const& token);

    template<typename Policy>
    void do_assign_arith(rpn_stack_t& rpn_stack, token_t const& token);

    template<typename Policy>
    void do_assign_shift(rpn_stack_t& rpn_stack, token_t const& token);

    template<typename Policy>
    void do_logical_begin(rpn_stack_t& rpn_stack, token_t const*& token);

    template<typename Policy>
    void do_logical_end(rpn_stack_t& rpn_stack);

    void req_quantity(token_t const& token, rpn_value_t const& value);
    void req_quantity(token_t const& token, rpn_value_t const& lhs, rpn_value_t const& rhs);

    // Cast-related
    template<do_t D>
    void force_truncate(rpn_value_t& rpn_value, type_t to_type, pstring_t pstring = {});

    template<do_t D>
    void force_promote(rpn_value_t& rpn_value, type_t to_type, pstring_t pstring = {});

    template<do_t D>
    void force_convert_int(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t pstring = {});

    template<do_t D>
    void force_round_real(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t pstring = {});

    template<do_t D>
    void force_boolify(rpn_value_t& rpn_value, pstring_t pstring = {});

    template<do_t D>
    bool cast(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t pstring = {});

    template<do_t D>
    void throwing_cast(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t pstring = {});

    template<do_t D>
    int cast_args(pstring_t pstring, rpn_value_t* begin, rpn_value_t* end, type_t const* type_begin, bool implicit);

    std::size_t num_locals() const;
    type_t var_i_type(unsigned var_i) const;
    void init_cval(access_t a, cval_t& cval);
    access_t access(rpn_value_t const& rpn_value) const;
    ssa_value_t const& get_local(pstring_t pstring, unsigned var_i, unsigned member, unsigned index) const;
    ssa_value_t& get_local(pstring_t pstring, unsigned var_i, unsigned member, unsigned index);
};


cpair_t interpret_expr(pstring_t pstring, token_t const* expr, type_t expected_type, eval_t* env)
{
    if(env)
    {
        env->do_expr_result<eval_t::INTERPRET_CE>(expr, expected_type);
        return env->final_result;
    }
    else
    {
        eval_t i(eval_t::do_wrapper_t<eval_t::INTERPRET>{}, pstring, expr, expected_type);
        return i.final_result;
    }
}

template<eval_t::do_t D>
eval_t::eval_t(do_wrapper_t<D>, pstring_t pstring, token_t const* expr, type_t expected_type)
: pstring(pstring)
, start_time(clock::now())
{
    do_expr_result<D>(expr, expected_type);
}

template<eval_t::do_t D>
eval_t::eval_t(do_wrapper_t<D>, pstring_t pstring, fn_t const& fn_ref, cval_t const* args)
: pstring(pstring)
, fn(&fn_ref)
, stmt(fn_ref.def().stmts.data())
, start_time(clock::now())
{
    // TODO
    interpret_locals.resize(fn->def().local_vars.size());
    local_types.resize(fn->def().local_vars.size());

    unsigned const argn = fn->type().num_params();
    for(unsigned i = 0; i < argn; ++i)
    {
        local_types[i] = ::dethunkify(fn->def().local_vars[i].type, this);
        interpret_locals[i] = args[i];
    }


    if(D == COMPILE)
    {
        assert(false);
        assert(ir);

        ir->gvar_loc_manager.init(fn->handle());
        input_pool.clear(); // TODO: make sure this isn't called in recursion

        ir->root = insert_cfg(true);

        ssa_ht const entry = ir.root->emplace_ssa(SSA_entry, TYPE_VOID);
        entry->append_daisy();

        // Insert nodes for the arguments
        for(unsigned i = 0; i < fn->def().num_params; ++i)
        {
            ir->root.data<block_d>().fn_vars[i] = ir->root->emplace_ssa(
                SSA_read_global, fn->def().local_vars[i].type, entry, 
                locator_t::arg(fn->handle(), i, 0));
        }

        // Insert nodes for gvar reads
        ir->gvar_loc_manager.for_each_locator([&](locator_t loc, gvar_loc_manager_t::index_t i)
        {
            ir->root.data<block_d>().fn_vars[to_var_i(i)] = 
                ir->root->emplace_ssa(
                    SSA_read_global, ir->gvar_loc_manager.type(i), 
                    entry, loc);
        });

        // Create all of the SSA graph, minus the exit node:
        cfg_ht const end = compile_block(ir.root);
        exits_with_jump(*end);

        // Now create the exit block.
        // All return statements create a jump, which will jump to the exit node.
        type_t const return_type = fn->type().return_type();
        if(return_type != TYPE_VOID)
            return_values.push_back(
                end->emplace_ssa(SSA_uninitialized, return_type));

        ir->exit = insert_cfg(true);

        for(cfg_ht node : return_jumps)
            node->build_set_output(0, ir->exit);
        end->build_set_output(0, ir->exit);

        // Write all globals at the exit:
        std::vector<ssa_value_t> return_inputs;
        return_inputs.reserve(ir->gvar_loc_manager.num_unique_locators() * 2);

        ir->gvar_loc_manager.for_each_locator([&](locator_t loc, gvar_loc_manager_t::index_t i)
        {
            return_inputs.push_back(var_lookup(ir->exit, to_var_i(i)));
            return_inputs.push_back(loc);
        });

        ssa_ht ret = ir.exit->emplace_ssa(SSA_return, TYPE_VOID);

        // Append the return value, if it exists:
        if(return_type != TYPE_VOID)
        {
            ssa_ht phi = ir.exit->emplace_ssa(SSA_phi, return_type);
            phi->assign_input(&*return_values.begin(), &*return_values.end());
            return_inputs.push_back(phi);
            return_inputs.push_back(locator_t::ret(fn.handle()));
        }

        assert(return_inputs.size() % 2 == 0);
        ret->assign_input(&*return_inputs.begin(), &*return_inputs.end());
        ret->append_daisy();

#ifndef NDEBUG
        for(cfg_ht h = ir.cfg_begin(); h; ++h)
            assert(h.data<block_d>().sealed());
#endif
    }
    else
        interpret_stmts<D>();
}

template<eval_t::do_t D>
void eval_t::do_expr_result(token_t const* expr, type_t expected_type)
{
    rpn_stack_t rpn_stack;
    do_expr<D>(rpn_stack, expr);

    if(expected_type.name() != TYPE_VOID)
        throwing_cast<D>(rpn_stack.only1(), expected_type, true);
    if(is_interpret(D))
        final_result.value = to_cval(rpn_stack.only1());

    final_result.type = rpn_stack.only1().type;
}

std::size_t eval_t::num_locals() const 
{ 
    return fn ? fn->def().local_vars.size() : 0; 
}

/* TODO: remove?
type_t eval_t::var_i_type(unsigned var_i) const
{
    assert(var_i < local_types.size());
    return local_types[var_i];
}
*/

cval_t const& eval_t::root_cval(rpn_value_t const& rpn_value) const
{
    if(rpn_value.value.is_locator() && rpn_value.value.locator().lclass() == LOC_GLOBAL_CONST)
        return rpn_value.value.locator().const_()->cval();
    assert(rpn_value.var_i < interpret_locals.size());
    return interpret_locals[rpn_value.var_i];
}


type_t eval_t::root_type(rpn_value_t const& rpn_value) const
{
    if(rpn_value.value.is_locator() && rpn_value.value.locator().lclass() == LOC_GLOBAL_CONST)
        return rpn_value.value.locator().const_()->type();
    assert(rpn_value.var_i < local_types.size());
    return local_types[rpn_value.var_i];
}

ssa_value_t eval_t::local_leaf(rpn_value_t const& rpn_value, access_t a) const
{
    assert(rpn_value.var_i < local_types.size());
    assert(a.member < interpret_locals[rpn_value.var_i].size());

    cval_t const& cval = root_cval(rpn_value);

    if(a.member >= cval.size()) // Hit during INTERPRET_CE
        compiler_error(rpn_value.pstring, "Unable to access during constant evaluation. ");

    auto const& array = cval[a.member];

    if((unsigned)a.index >= array.size())
    {
        compiler_error(rpn_value.pstring, 
                       fmt("Array index is out of bounds. (index of % >= size of %)", 
                       a.index, array.size()));
    }

    return array[a.index];
}

/********
ssa_value_t& eval_t::get_local(pstring_t pstring, unsigned var_i, unsigned member, unsigned index)
{
    return const_cast<ssa_value_t&>(static_cast<eval_t const*>(this)->get_local(pstring, var_i, member, index));
}
*/

auto eval_t::access(rpn_value_t const& rpn_value) const -> access_t
{
    access_t a = { root_type(rpn_value) };
    assert(a.index < 0);
    assert(!is_thunk(a.type.name()));

    for(unsigned m = 0;;)
    {
        if(a.type.name() == TYPE_ARRAY)
        {
            if(!rpn_value.index)
            {
                assert(m == rpn_value.members.size());
                return a;
            }

            assert(rpn_value.index.is_num());
            assert(a.index < 0);

            a.index = rpn_value.index.whole();
            a.type = a.type.elem_type();
        }
        else if(m == rpn_value.members.size())
            return a;
        else
        {
            assert(a.type.name() == TYPE_STRUCT);

            struct_t const& s = a.type.struct_();
            a.member += s.member(rpn_value.members[m]);
            a.type = s.field(rpn_value.members[m]).type;

            ++m;
        }
    }
}

cval_t eval_t::to_cval(rpn_value_t const& rpn_value) const
{
    if(is_aggregate(rpn_value.type.name()))
    {
        auto a = access(rpn_value);

        cval_t cval;
        cval.resize(num_members(a.type));

        cval_t const& from = root_cval(rpn_value);

        if(a.index < 0)
            for(unsigned i = 0; i < cval.size(); ++i)
                cval[i] = from[a.member + i];
        else
            for(unsigned i = 0; i < cval.size(); ++i)
                cval[i] = { from[a.member + i][a.index] };

        return cval;
    }
    else
        return cval_t(1, { rpn_value.value });
}

// TODO
void eval_t::init_cval(access_t a, cval_t& cval)
{
    if(a.type.name() == TYPE_STRUCT)
    {
        struct_t const& s = a.type.struct_();
        for(unsigned i = 0; i < s.fields().size(); ++i)
            init_cval({ s.field(i).type, a.member + s.member(i) , a.index }, cval);
    }
    else if(a.type.name() == TYPE_ARRAY)
        init_cval({ a.type.elem_type(), a.member, a.type.size() }, cval);
    else
    {
        assert(a.member < cval.size());
        a.index = std::max(a.index, 1);
        cval[a.member].resize(a.index);
    }
}

void eval_t::check_time()
{
    auto elapsed = clock::now() - start_time;
    if(compiler_options().time_limit > 0)
    {
        if(elapsed > sc::milliseconds(compiler_options().time_limit))
        {
            file_contents_t file(this->pstring.file_i);
            throw out_of_time_t(
                fmt_error(file, this->pstring, "Ran out of time executing expression.")
                + fmt_note("Computation is likely divergent.\n")
                + fmt_note(fmt("Use compiler flag --timelimit 0 to ignore this error.\n", compiler_options().time_limit))
                );
        }
    }
}

template<eval_t::do_t D>
void eval_t::interpret_stmts()
{
    static_assert(D != COMPILE);

    rpn_stack_t rpn_stack;

    while(true)
    {
        check_time();

        // Temporary locals can be allocated in 'do_expr'; this resets that.
        interpret_locals.resize(num_locals());
        local_types.resize(num_locals());

        switch(stmt->name)
        {
        default: // Handles var inits
            if(is_var_init(stmt->name))
            {
                if(D == INTERPRET_CE)
                    compiler_error(stmt->pstring, "Expression cannot be evaluated at compile time.");

                unsigned const var_i = ::get_local_var_i(stmt->name);

                // Prepare the type.
                assert(var_i < local_types.size());
                if(local_types[var_i].name() == TYPE_VOID)
                    local_types[var_i] = dethunkify(fn->def().local_vars[var_i].type, this);

                if(stmt->expr)
                {
                    do_expr<D>(rpn_stack, stmt->expr);
                    throwing_cast<D>(rpn_stack.peek(0), local_types[var_i], true);

                    if(D == INTERPRET)
                        interpret_locals[var_i] = to_cval(rpn_stack.only1());
                }
                else
                {
                    type_t const t = local_types[var_i];

                    switch(t.name())
                    {
                    case TYPE_ARRAY:
                        interpret_locals[var_i].resize(num_members(t));
                        for(auto& v : interpret_locals[var_i])
                            v.resize(t.size());
                        break;
                    case TYPE_STRUCT:
                        interpret_locals[var_i].resize(num_members(t));
                        init_cval({ t }, interpret_locals[var_i]);
                        break;
                    default:
                        interpret_locals[var_i].resize(1);
                        interpret_locals[var_i][0].resize(1);
                        break;
                    }
                }

                ++stmt;
            }
            else
                compiler_error(stmt->pstring, "Statement cannot appear in constant evaluation.");
            break;

        case STMT_EXPR:
        case STMT_FOR_EFFECT:
            do_expr<D>(rpn_stack, stmt->expr);
            ++stmt;
            break;

        case STMT_DO:
        case STMT_END_BLOCK:
        case STMT_LABEL:
            ++stmt;
            break;

        case STMT_ELSE:
        case STMT_END_WHILE:
        case STMT_END_FOR:
        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_GOTO:
            if(is_interpret(D))
                stmt = &fn->def()[stmt->link];
            else
                ++stmt;
            break;

        case STMT_IF:
            do_expr<D>(rpn_stack, stmt->expr);
            throwing_cast<D>(rpn_stack.only1(), TYPE_BOOL, true);
            if(!is_interpret(D) || rpn_stack.only1().fixed())
                ++stmt;
            else
            {
                stmt = &fn->def()[stmt->link];
                if(stmt->name == STMT_ELSE)
                    ++stmt;
            }
            break;

        case STMT_WHILE:
        case STMT_FOR:
            do_expr<D>(rpn_stack, stmt->expr);
            throwing_cast<D>(rpn_stack.only1(), TYPE_BOOL, true);
            if(!is_interpret(D) || rpn_stack.only1().fixed())
                ++stmt;
            else
                stmt = &fn->def()[stmt->link];
            break;

        case STMT_END_DO:
            do_expr<D>(rpn_stack, stmt->expr);
            throwing_cast<D>(rpn_stack.only1(), TYPE_BOOL, true);
            if(is_interpret(D) && rpn_stack.only1().fixed())
                stmt = &fn->def()[stmt->link];
            else
                ++stmt;
            break;

        case STMT_RETURN:
            {
                type_t return_type = fn->type().return_type();
                if(stmt->expr)
                {
                    do_expr<D>(rpn_stack, stmt->expr);
                    throwing_cast<D>(rpn_stack.only1(), return_type, true);
                    if(is_interpret(D))
                        final_result.value = to_cval(rpn_stack.only1());
                    final_result.type = rpn_stack.only1().type;
                }
                else if(return_type.name() != TYPE_VOID)
                {
                    compiler_error(stmt->pstring, fmt(
                        "Expecting return expression of type %.", return_type));
                }
            }
            return;

        case STMT_END_FN:
            type_t return_type = fn->type().return_type();
            if(return_type.name() != TYPE_VOID)
            {
                compiler_error(stmt->pstring, fmt(
                    "Interpreter reached end of function without returning %.", return_type));
            }
            return;
        }
    }
    assert(false);
}

template<eval_t::do_t D>
void eval_t::do_expr(rpn_stack_t& rpn_stack, token_t const* expr)
{
    using namespace std::literals::chrono_literals;
    using S = sfixed_int_t;

    check_time();

    rpn_stack.clear(); // Reset the stack.

    for(token_t const* token = expr; token->type; ++token)
    {
        // Declare cross-label vars before switch.
        ssa_value_t common_value;
        type_t common_type;

        switch(token->type)
        {
        default:
            throw std::runtime_error(fmt("Invalid token '%' in expression.", token_string(token->type)));

        case TOK_ident:
            assert(token->value < num_locals());

            if(D == COMPILE)
            {
                assert(false);
            }
            else if(D == INTERPRET_CE)
                compiler_error(token->pstring, "Expression cannot be evaluated at compile time.");
            else
            {
                assert(D == CHECK || D == INTERPRET);
                assert(token->value < local_types.size());

                rpn_value_t new_top =
                {
                    .category = LVAL, 
                    .type = local_types[token->value],
                    .pstring = token->pstring,
                    .var_i = token->value,
                };

                if(D == INTERPRET && !is_aggregate(new_top.type.name()))
                {
                    if(interpret_locals[token->value].empty())
                        compiler_error(token->pstring, "Variable is invalid because goto jumped past its initialization.");
                    new_top.value = interpret_locals[token->value][0][0];
                }

                rpn_stack.push(std::move(new_top));
            }

            break;

        case TOK_global_ident:
            {
                global_t* global = token->ptr<global_t>();
                switch(global->gclass())
                {
                default: 
                    assert(false);
                    std::terminate();

                case GLOBAL_CONST:
                    {
                        const_t const& c = global->impl<const_t>();
                        assert(!is_thunk(c.type().name()));
                        assert(c.cval().size());

                        if(D == COMPILE)
                        {
                            assert(false);
                        }
                        else
                        {
                            rpn_value_t new_top =
                            {
                                .category = RVAL, 
                                .type = c.type(),
                                .pstring = token->pstring,
                            };

                            if(is_aggregate(c.type().name()))
                                new_top.value = locator_t::global_const(global->handle<const_ht>());
                            else
                                new_top.value = c.cval()[0][0];

                            rpn_stack.push(std::move(new_top));
                        }
                    }
                    break;

                case GLOBAL_FN:
                    rpn_stack.push({ 
                        .value = ssa_value_t(locator_t::fn(global->handle<fn_ht>())),
                        .category = RVAL, 
                        .type = global->impl<fn_t>().type(), 
                        .pstring = token->pstring });
                    break;
                }
            }
            break;

        case TOK_int:
            common_value.set(mask_numeric(fixed_t{ token->value }, TYPE_INT));
        push_int:
            rpn_stack.push({
                .value = common_value,
                .category = RVAL, 
                .type = { TYPE_INT }, 
                .pstring = token->pstring });
            break;

        case TOK_real:
            common_value.set(mask_numeric(fixed_t{ token->value }, TYPE_REAL));
            rpn_stack.push({
                .value = common_value,
                .category = RVAL, 
                .type = { TYPE_REAL }, 
                .pstring = token->pstring });
            break;


        case TOK_period:
            {
                // Periods represent struct member access.

                rpn_value_t& struct_val = rpn_stack.peek(0);

                if(struct_val.type.name() != TYPE_STRUCT)
                {
                    compiler_error(struct_val.pstring, fmt(
                        "Expecting struct type. Got %.", struct_val.type));
                }

                struct_t const& s = struct_val.type.struct_();

                std::uint64_t const hash = token->value;
                auto const it = s.fields().find(hash);

                if(!it)
                {
                    file_contents_t file(token->pstring.file_i);
                    compiler_error(file, token->pstring, fmt(
                        "% isn't a member of %.", 
                        token->pstring.view(file.source()), s.global.name));
                }

                unsigned const field_i = it - s.fields().begin();

                struct_val.type = it->second.type;
                struct_val.members.push_back(field_i);
                struct_val.pstring = concat(struct_val.pstring, token->pstring);
                
                if(is_interpret(D))
                {
                    if(!is_aggregate(struct_val.type.name()))
                    {
                        access_t a = access(struct_val);
                        a.index = std::max(a.index, 0);
                        struct_val.value = local_leaf(struct_val, a);
                    }
                }

                break;
            }

        case TOK_apply:
            {
                // TOK_apply is a psuedo token used to represent application. 
                // The token's 'value' stores the application's arity:
                std::size_t const num_args = token->value;

                // The eval stack contains the arguments to be applied.
                // Right beneath those it contains the fn value to be called.
                rpn_value_t& fn_val = rpn_stack.peek(num_args);

                if(fn_val.type.name() != TYPE_FN)
                {
                    compiler_error(fn_val.pstring, fmt(
                        "Expecting function type. Got %.", fn_val.type));
                }

                std::size_t const num_params = fn_val.type.num_params();
                type_t const* const params = fn_val.type.types();

                if(num_args != num_params)
                {
                    compiler_error(
                        fn_val.pstring, fmt(
                        "Passed % arguments to a function of type %. "
                        "Expecting % arguments.",
                        num_args, fn_val.type, num_params));
                }

                // Now for the arguments.
                // Cast all arguments to match the fn signature.
                rpn_value_t* const args = &rpn_stack.peek(num_args - 1);

                int const cast_result = cast_args<D>(fn_val.pstring, args, rpn_stack.past_top(), params, true);

                if(cast_result >= 0)
                {
                    compiler_error(
                        args[cast_result].pstring, fmt(
                        "Unable to convert type % "
                        "to type % in function application.\n"
                        "Expected signature: % ",
                        args[cast_result].type, params[cast_result], fn_val.type));
                }

                // For now, only const fns are allowed.
                // In the future, fn pointers may be supported.
                assert(fn_val.value.is_locator());
                fn_ht fn = fn_val.value.locator().fn();

                pstring_t const call_pstring = concat(fn_val.pstring, token->pstring);

                // Now do the call!

                if(D == CHECK)
                {
                    rpn_value_t new_top =
                    {
                        .category = RVAL, 
                        .type = fn_val.type.return_type(), 
                        .pstring = call_pstring,
                    };

                    rpn_stack.pop(num_args + 1);
                    rpn_stack.push(std::move(new_top));
                }
                else if(is_interpret(D))
                {
                    bc::small_vector<cval_t, 8> cval_args(num_args);
                    for(unsigned i = 0; i < num_args; ++i)
                        cval_args[i] = to_cval(args[i]);

                    try
                    {
                        // NOTE: call as INTERPRET, not D.
                        eval_t call(do_wrapper_t<INTERPRET>{}, call_pstring, *fn, cval_args.data());

                        // Allocate a temporary local var for the return:
                        unsigned const return_i = interpret_locals.size();
                        assert(return_i == local_types.size());
                        interpret_locals.push_back(std::move(call.final_result.value));
                        local_types.push_back(std::move(call.final_result.type));

                        // Update the eval stack.
                        rpn_value_t new_top =
                        {
                            .category = RVAL, 
                            .type = fn_val.type.return_type(), 
                            .pstring = call_pstring,
                            .var_i = return_i
                        };

                        if(!is_aggregate(new_top.type.name()))
                            new_top.value = interpret_locals.back()[0][0];

                        rpn_stack.pop(num_args + 1);
                        rpn_stack.push(std::move(new_top));
                    }
                    catch(out_of_time_t& e)
                    {
                        file_contents_t file(this->pstring.file_i);
                        e.msg += fmt_note(file, this->pstring, "Backtrace:");
                        throw;
                    }
                }
                else if(D == COMPILE)
                {
                    assert(false);
                }

                break;
            }

        case TOK_cast_argn:
            {
                // TOK_cast are pseudo tokens used to implement type casts.

                // Extract how many args this cast parsed:
                unsigned const argn = token->value;
                rpn_value_t* const args = &rpn_stack.peek(argn - 1);
                assert(rpn_stack.past_top() - args == argn);

                // Advance the token.
                ++token;
                assert(token->type == TOK_cast_type);
                type_t const type = dethunkify(*token->ptr<type_t const>(), this);

                auto const check_argn = [&](unsigned size)
                { 
                    if(argn != size)
                        compiler_error(token->pstring, fmt(
                            "Too % arguments to %. Expecting %.", 
                            argn < size ? "few" : "many", type, size));
                };

                cval_t new_cval;

                if(type.name() == TYPE_STRUCT)
                {
                    struct_t const& s = type.struct_();
                    check_argn(s.fields().size());

                    type_t* const types = ALLOCA_T(type_t, s.fields().size());
                    for(unsigned i = 0; i < s.fields().size(); ++i)
                    {
                        types[i] = s.field(i).type;
                        assert(!is_thunk(types[i].name()));
                    }

                    int const cast_result = cast_args<D>(token->pstring, args, rpn_stack.past_top(), types, false);

                    if(cast_result >= 0)
                    {
                        assert(cast_result < (int)s.fields().size());
                        compiler_error(
                            args[cast_result].pstring, fmt(
                            "Unable to convert type % to type % in cast to %.\n",
                            args[cast_result].type, types[cast_result], s.global.name));
                    }

                    // Create a new cval.
                    new_cval.resize(num_members(type));
                    init_cval({ type }, new_cval);

                    for(unsigned i = 0, j = 0; i < s.fields().size(); ++i)
                    {
                        cval_t arg = to_cval(args[i]);

                        for(unsigned k = 0; k < arg.size(); ++k)
                        {
                            assert((j + k) < new_cval.size());
                            new_cval[j + k] = std::move(arg[k]);
                        }

                        j += arg.size();
                    }

                    goto add_new_cval;

                }
                else if(type.name() == TYPE_ARRAY)
                {
                    check_argn(type.array_length());

                    for(unsigned i = 0; i < type.array_length(); ++i)
                        throwing_cast<D>(args[i], type.elem_type(), false);

                    // Create a new cval.
                    new_cval.resize(num_members(type));
                    for(auto& vec : new_cval)
                        vec.resize(type.array_length());

                    for(unsigned i = 0; i < type.array_length(); ++i)
                    {
                        cval_t arg = to_cval(args[i]);
                        assert(arg.size() == new_cval.size());

                        for(unsigned j = 0; j < arg.size(); ++j)
                        {
                            assert(arg[j].size() == 1);
                            new_cval[j][i] = arg[j][0];
                        }
                    }

                add_new_cval:

                    // Allocate a temporary.
                    unsigned const temp_i = interpret_locals.size();
                    assert(temp_i == local_types.size());
                    interpret_locals.push_back(std::move(new_cval));
                    local_types.push_back(type);

                    // Update the stack.
                    rpn_stack.pop(argn);
                    rpn_stack.push({
                        .category = RVAL, 
                        .type = type, 
                        .pstring = token->pstring,
                        .var_i = temp_i });
                }
                else if(is_scalar(type.name()))
                {
                    check_argn(1);
                    throwing_cast<D>(rpn_stack.peek(0), type, false);
                }
                else
                    compiler_error(token->pstring, fmt("Unable to cast to %.", type));

                rpn_stack.peek(0).pstring = concat(token->pstring, rpn_stack.peek(0).pstring);
            }
            break;


        case TOK_index:
            {
                // TOK_index is a psuedo token used to implement array indexing. 

                // The eval stack contains the index on top.
                // Right beneath it contains the array.
                rpn_value_t& array_val = rpn_stack.peek(1);

                if(array_val.type.name() != TYPE_ARRAY)
                {
                    compiler_error(array_val.pstring, fmt(
                        "Expecting array type. Got %.", array_val.type));
                }

                rpn_value_t& array_index = rpn_stack.peek(0);

                // Array indexes are always bytes.
                throwing_cast<D>(array_index, TYPE_U, true);

                array_val.pstring = concat(array_val.pstring, token->pstring);

                if(is_interpret(D))
                {
                    // Set before calling 'access'
                    array_val.index = array_index.whole();

                    if(!is_aggregate(array_val.type.elem_type().name()))
                    {
                        access_t const a = access(array_val);
                        assert(a.index >= 0);
                        array_val.value = local_leaf(array_val, a);
                    }
                    else
                        array_val.value = {};
                }
                else if(D == COMPILE)
                {
                    assert(false);
                }

                // Set after calling 'access':
                array_val.type = array_val.type.elem_type();
                rpn_stack.pop(1);
                break;
            }

        case TOK_sizeof_expr:
            {
                rpn_stack_t sub_stack;
                do_expr<CHECK>(sub_stack, token->ptr<token_t const>());
                common_type = sub_stack.peek(0).type;
                goto do_sizeof;
            }

        case TOK_sizeof:
            {
                common_type = dethunkify(*token->ptr<type_t const>(), this);
            do_sizeof:
                unsigned const size = common_type.size_of();

                if(size == 0)
                    compiler_error(token->pstring, fmt("Type % has no size.", common_type));

                common_value.set(size);
                goto push_int;
            }

        case TOK_len_expr:
            {
                rpn_stack_t sub_stack;
                do_expr<CHECK>(sub_stack, token->ptr<token_t const>());
                common_type = sub_stack.peek(0).type;
                goto do_len;
            }

        case TOK_len:
            {
                common_type = dethunkify(*token->ptr<type_t const>(), this);
            do_len:
                unsigned const size = common_type.array_length();

                if(size == 0)
                    compiler_error(token->pstring, fmt("Type % isn't an array.", common_type));

                common_value.set(size);
                goto push_int;
            }

        case TOK_assign:
            do_assign<D>(rpn_stack, *token);
            break;

        case TOK_logical_and:
            struct logical_and_p : do_wrapper_t<D>
            {
                static token_type_t logical_token() { return TOK_logical_and; }
                static token_type_t end_logical_token() { return TOK_end_logical_and; }
            };
            do_logical_begin<logical_and_p>(rpn_stack, token);
            break;
        case TOK_end_logical_and:
            do_logical_end<logical_and_p>(rpn_stack);
            break;
        case TOK_logical_or:
            struct logical_or_p : do_wrapper_t<D>
            {
                static token_type_t logical_token() { return TOK_logical_or; }
                static token_type_t end_logical_token() { return TOK_end_logical_or; }
            };
            do_logical_begin<logical_or_p>(rpn_stack, token);
            break;
        case TOK_end_logical_or:
            do_logical_end<logical_or_p>(rpn_stack);
            break;

        case TOK_eq:
            struct eq_p : do_wrapper_t<D>
            {
                static bool interpret(S lhs, S rhs) { return lhs == rhs; }
            };
            do_compare<eq_p>(rpn_stack, *token);
            break;
        case TOK_not_eq:
            struct not_eq_p : do_wrapper_t<D>
            {
                static bool interpret(S lhs, S rhs) { return lhs != rhs; }
            };
            do_compare<not_eq_p>(rpn_stack, *token);
            break;
        case TOK_gt:
            struct gt_p : do_wrapper_t<D>
            {
                static bool interpret(S lhs, S rhs) { return lhs > rhs; }
            };
            do_compare<gt_p>(rpn_stack, *token);
            break;
        case TOK_lt:
            struct lt_p : do_wrapper_t<D>
            {
                static bool interpret(S lhs, S rhs) { return lhs < rhs; }
            };
            do_compare<lt_p>(rpn_stack, *token);
            break;
        case TOK_gte:
            struct gte_p : do_wrapper_t<D>
            {
                static bool interpret(S lhs, S rhs) { return lhs >= rhs; }
            };
            do_compare<gte_p>(rpn_stack, *token);
            break;
        case TOK_lte:
            struct lte_p : do_wrapper_t<D>
            {
                static bool interpret(S lhs, S rhs) { return lhs <= rhs; }
            };
            do_compare<lte_p>(rpn_stack, *token);
            break;

        case TOK_plus:
            struct plus_p : do_wrapper_t<D>
            {
                static S interpret(S lhs, S rhs) { return lhs + rhs; }
            };
            do_arith<plus_p>(rpn_stack, *token);
            break;
        case TOK_plus_assign:
            do_assign_arith<plus_p>(rpn_stack, *token);
            break;

        case TOK_minus: 
            struct minus_p : do_wrapper_t<D>
            {
                static S interpret(S lhs, S rhs) { return lhs - rhs; }
            };
            do_arith<minus_p>(rpn_stack, *token);
            break;
        case TOK_minus_assign:
            do_assign_arith<minus_p>(rpn_stack, *token);
            break;

        case TOK_bitwise_and: 
            struct bitwise_and_p : do_wrapper_t<D>
            {
                static S interpret(S lhs, S rhs) { return lhs & rhs; }
            };
            do_arith<bitwise_and_p>(rpn_stack, *token);
            break;
        case TOK_bitwise_and_assign:
            do_assign_arith<bitwise_and_p>(rpn_stack, *token);
            break;

        case TOK_bitwise_or:  
            struct bitwise_or_p : do_wrapper_t<D>
            {
                static S interpret(S lhs, S rhs) { return lhs | rhs; }
            };
            do_arith<bitwise_or_p>(rpn_stack, *token);
            break;
        case TOK_bitwise_or_assign:
            do_assign_arith<bitwise_or_p>(rpn_stack, *token);
            break;

        case TOK_bitwise_xor:
            struct bitwise_xor_p : do_wrapper_t<D>
            {
                static S interpret(S lhs, S rhs) { return lhs ^ rhs; }
            };
            do_arith<bitwise_xor_p>(rpn_stack, *token);
            break;
        case TOK_bitwise_xor_assign:
            do_assign_arith<bitwise_xor_p>(rpn_stack, *token);
            break;

        case TOK_lshift:
            struct lshift_p : do_wrapper_t<D>
            {
                static S interpret(S lhs, std::uint8_t shift) { return lhs << shift; }
            };
            do_shift<lshift_p>(rpn_stack, *token);
            break;
        case TOK_lshift_assign:
            do_assign_shift<lshift_p>(rpn_stack, *token);
            break;

        case TOK_rshift:
            struct rshift_p : do_wrapper_t<D>
            {
                static S interpret(S lhs, std::uint8_t shift) { return lhs >> shift; }
            };
            do_shift<rshift_p>(rpn_stack, *token);
            break;
        case TOK_rshift_assign:
            do_assign_shift<rshift_p>(rpn_stack, *token);
            break;

        case TOK_unary_negate:
            {
                rpn_value_t& top = rpn_stack.peek(0);
                throwing_cast<D>(top, { TYPE_BOOL }, true);

                if(is_interpret(D))
                    top.value.set((unsigned)!top.whole());
                else if(D == COMPILE)
                    assert(false);

                break;
            }

        case TOK_unary_minus:
            {
                rpn_value_t& top = rpn_stack.peek(0);
                req_quantity(*token, top);

                if(is_interpret(D))
                    top.value.set(mask_numeric(fixed_t{ -top.sfixed() }, top.type.name()));
                else if(D == COMPILE)
                    assert(false);

                break;
            }

        case TOK_unary_xor:
            {
                rpn_value_t& top = rpn_stack.peek(0);
                req_quantity(*token, top);

                std::printf("top = %i\n", (int)top.sfixed());
                std::printf("top = %i\n", (int)~top.sfixed());

                if(is_interpret(D))
                    top.value.set(mask_numeric(fixed_t{ ~top.fixed().value }, top.type.name()));
                else if(D == COMPILE)
                    assert(false);

                break;
            }
        }
    }
}

template<eval_t::do_t D>
void eval_t::do_assign(rpn_stack_t& rpn_stack, token_t const& token)
{
    rpn_value_t& assignee = rpn_stack.peek(1);
    rpn_value_t& assignment = rpn_stack.peek(0);

    pstring_t const pstring = concat(assignee.pstring, assignee.pstring);

    if(assignee.category == RVAL)
        compiler_error(pstring, "Expecting lvalue on left side of assignment.");

    throwing_cast<D>(assignment, assignee.type, true);

    assert(assignee.var_i < interpret_locals.size());

    // Remap the identifier to point to the new value.
    if(D == INTERPRET)
    {
        cval_t cval = to_cval(assignment);

        auto a = access(assignee);
        assert(cval.size() == num_members(a.type));

        for(unsigned i = 0; i < cval.size(); ++i)
        {
            if(a.index < 0)
                interpret_locals[assignee.var_i][a.member + i] = std::move(cval[i]);
            else
            {
                assert(cval[i].size() == 1);
                interpret_locals[assignee.var_i][a.member + i][a.index] = cval[i][0];
            }
        }
    }
    else if(D == COMPILE)
    {
        assert(false);
    }

    // Leave the assignee on the stack, slightly modified.
    assignee.category = RVAL;
    rpn_stack.pop();
}

void eval_t::req_quantity(token_t const& token, rpn_value_t const& value)
{
    if(!is_quantity(value.type.name()))
    {
        compiler_error(value.pstring, fmt("% expects arithmetic quantity inputs. (Input is %)", 
                                          token_string(token.type), value.type));
    }
}
    
void eval_t::req_quantity(token_t const& token, rpn_value_t const& lhs, rpn_value_t const& rhs)
{
    if(!is_quantity(lhs.type.name()) || !is_arithmetic(rhs.type.name()))
    {
        pstring_t pstring = concat(lhs.pstring, rhs.pstring);
        compiler_error(pstring, fmt("% expects arithmetic quantity inputs. (Inputs are % and %)", 
                                    token_string(token.type), lhs.type, rhs.type));
    }
}


// Applies an operator to the top two values on the eval stack,
// turning them into a single value.
void eval_t::compile_binary_operator(cfg_node_t& cfg_node, ssa_op_t op, type_t result_type, bool carry)
{
    rpn_value_t& lhs = rpn_stack.peek(1);
    rpn_value_t& rhs = rpn_stack.peek(0);

    // Result will remain in 'lhs'.
    ssa_value_t result;
    if(carry)
        result = cfg_node.emplace_ssa(op, result_type, lhs.value, rhs.value, 0);
    else
        result = cfg_node.emplace_ssa(op, result_type, lhs.value, rhs.value);

    rpn_value_t new_top =
    {
        .value = result,
        .category = RVAL, 
        .type = result_type, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };
    
    rpn_stack.pop(2);
    rpn_stack.push(std::move(new_top));
}


template<typename Policy>
void eval_t::do_compare(rpn_stack_t& rpn_stack, token_t const& token)
{
    rpn_value_t& lhs = rpn_stack.peek(1);
    rpn_value_t& rhs = rpn_stack.peek(0);
    req_quantity(token, lhs, rhs);

    rpn_value_t new_top =
    {
        .category = RVAL, 
        .type = { TYPE_BOOL }, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };

    if(is_interpret(Policy::D))
    {
        bool const result = Policy::interpret(lhs.sfixed(), rhs.sfixed());
        new_top.value.set((unsigned)result);
    }
    else if(Policy::D == COMPILE)
    {
        assert(false);
    }

    rpn_stack.pop(2);
    rpn_stack.push(std::move(new_top));
}

template<typename Policy>
void eval_t::do_arith(rpn_stack_t& rpn_stack, token_t const& token)
{
    rpn_value_t& lhs = rpn_stack.peek(1);
    rpn_value_t& rhs = rpn_stack.peek(0);
    req_quantity(token, lhs, rhs);

    type_t result_type;

    if(lhs.type != rhs.type)
    {
        if(is_ct(lhs.type.name()) && can_cast(lhs.type, rhs.type, true))
        {
            result_type = rhs.type;
            throwing_cast<Policy::D>(lhs, result_type, true);
        }
        else if(is_ct(rhs.type.name()) && can_cast(rhs.type, lhs.type, true))
        {
            result_type = lhs.type;
            throwing_cast<Policy::D>(rhs, result_type, true);
        }
        else
        {
        //bad_types: TODO
            pstring_t pstring = concat(lhs.pstring, rhs.pstring);
            compiler_error(pstring, fmt("% isn't defined for this type combination. (% and %)",
                                        token_string(token.type), lhs.type, rhs.type));
        }
    }
    else
        result_type = lhs.type;

    assert(is_arithmetic(result_type.name()));
    assert(lhs.type == result_type);
    assert(rhs.type == result_type);

    rpn_value_t new_top =
    {
        .category = RVAL, 
        .type = result_type, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };

    if(is_interpret(Policy::D))
    {
        assert(is_masked(lhs.fixed(), lhs.type.name()));
        assert(is_masked(rhs.fixed(), rhs.type.name()));

        fixed_t result = { Policy::interpret(lhs.sfixed(), rhs.sfixed()) };
        result.value &= numeric_bitmask(result_type.name());
        new_top.value.set(result);
    }
    else if(Policy::D == COMPILE)
    {
        assert(false);
    }

    rpn_stack.pop(2);
    rpn_stack.push(std::move(new_top));
}

template<typename Policy>
void eval_t::do_shift(rpn_stack_t& rpn_stack, token_t const& token)
{
    rpn_value_t& lhs = rpn_stack.peek(1);
    rpn_value_t& rhs = rpn_stack.peek(0);
    req_quantity(token, lhs, rhs);

    if(rhs.type.name() == TYPE_INT)
        throwing_cast<Policy::D>(rhs, { TYPE_U }, true);
    else if(rhs.type.name() != TYPE_U)
        compiler_error(rhs.pstring, fmt("Ride-hand side of operator % must be type U or Int.", 
                                        token_string(token.type)));

    type_t const result_type = lhs.type;
    assert(is_arithmetic(result_type.name()));

    rpn_value_t new_top =
    {
        .category = RVAL, 
        .type = result_type, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };

    if(is_interpret(Policy::D))
    {
        assert(is_masked(lhs.fixed(), lhs.type.name()));
        assert(is_masked(rhs.fixed(), rhs.type.name()));

        fixed_t result = { Policy::interpret(lhs.sfixed(), rhs.whole()) };
        result.value &= numeric_bitmask(result_type.name());
        new_top.value.set(result);
    }
    else if(Policy::D == COMPILE)
    {
        assert(false);
    }

    rpn_stack.pop(2);
    rpn_stack.push(std::move(new_top));
}

template<typename Policy>
void eval_t::do_assign_arith(rpn_stack_t& rpn_stack, token_t const& token)
{
    rpn_stack.tuck(rpn_stack.peek(1), 1);
    throwing_cast<Policy::D>(rpn_stack.peek(0), rpn_stack.peek(1).type, true);
    do_arith<Policy>(rpn_stack, token);
    do_assign<Policy::D>(rpn_stack, token);
}

template<typename Policy>
void eval_t::do_assign_shift(rpn_stack_t& rpn_stack, token_t const& token)
{
    rpn_stack.tuck(rpn_stack.peek(1), 1);
    do_shift<Policy>(rpn_stack, token);
    do_assign<Policy::D>(rpn_stack, token);
}

template<typename Policy>
void eval_t::do_logical_begin(rpn_stack_t& rpn_stack, token_t const*& token)
{
    rpn_value_t& top = rpn_stack.peek(0);
    throwing_cast<Policy::D>(top, { TYPE_BOOL }, true);

    if(Policy::D == CHECK)
        rpn_stack.pop(1);
    else if(is_interpret(Policy::D))
    {
        if(bool(top.fixed()) == (Policy::logical_token() == TOK_logical_or))
        {
            for(int left = 1; left; --left)
            while(token->type != Policy::end_logical_token())
            {
                assert(token->type);
                ++token;
                if(token->type == Policy::logical_token())
                    ++left;
            }
        }
        else
            rpn_stack.pop(1);
    }
    else if(Policy::D == COMPILE)
    {
        assert(false);
    }
}

template<typename Policy>
void eval_t::do_logical_end(rpn_stack_t& rpn_stack)
{
    rpn_value_t& top = rpn_stack.peek(0);
    throwing_cast<Policy::D>(top, { TYPE_BOOL }, true);

    if(Policy::D == COMPILE)
    {
        assert(false);
    }
}

template<eval_t::do_t D>
void eval_t::force_truncate(rpn_value_t& rpn_value, type_t to_type, pstring_t cast_pstring)
{
    assert(!is_ct(rpn_value.type.name()));
    assert(is_arithmetic(to_type.name()) && is_arithmetic(rpn_value.type.name()));

    rpn_value_t new_rpn =
    {
        .category = RVAL, 
        .type = to_type, 
        .pstring = cast_pstring ? concat(rpn_value.pstring, cast_pstring) : rpn_value.pstring,
    };

    if(is_interpret(D))
        new_rpn.value.set(mask_numeric(rpn_value.fixed(), to_type.name()));
    else if(D == COMPILE)
    {
        assert(false);
    }

    rpn_value = std::move(new_rpn);
}

template<eval_t::do_t D>
void eval_t::force_promote(rpn_value_t& rpn_value, type_t to_type, pstring_t cast_pstring)
{
    assert(!is_ct(rpn_value.type.name()));
    assert(is_arithmetic(to_type.name()) && is_arithmetic(rpn_value.type.name()));

    rpn_value_t new_rpn =
    {
        .category = RVAL, 
        .type = to_type, 
        .pstring = cast_pstring ? concat(rpn_value.pstring, cast_pstring) : rpn_value.pstring,
    };

    if(is_interpret(D))
        new_rpn.value.set(mask_numeric({ rpn_value.sfixed() }, to_type.name()));
    else if(D == COMPILE)
    {
        assert(false);
    }

    rpn_value = std::move(new_rpn);
}

template<eval_t::do_t D>
void eval_t::force_convert_int(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    assert(rpn_value.type.name() == TYPE_INT);
    assert(is_arithmetic(to_type.name()) && is_arithmetic(rpn_value.type.name()));

    rpn_value_t new_rpn =
    {
        .category = RVAL, 
        .type = to_type, 
        .pstring = cast_pstring ? concat(rpn_value.pstring, cast_pstring) : rpn_value.pstring,
    };

    if(D != CHECK)
    {
        fixed_t const masked = mask_numeric(rpn_value.fixed(), to_type.name());

        if(implicit && to_signed(masked.value, to_type.name()) != rpn_value.sfixed())
        {
            file_contents_t file(rpn_value.pstring.file_i);
            throw compiler_error_t(
                fmt_error(file, rpn_value.pstring, fmt(
                    "Int value of % can't be represented in type %. (Implicit type conversion)", 
                    to_double(fixed_t{ rpn_value.sfixed() }), to_type))
                + fmt_note("Add an explicit cast operator to override.")
                );
        }

        new_rpn.value.set(masked);
    }

    rpn_value = std::move(new_rpn);
}

template<eval_t::do_t D>
void eval_t::force_round_real(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    assert(rpn_value.type.name() == TYPE_REAL);
    assert(is_arithmetic(to_type.name()));

    rpn_value_t new_rpn =
    {
        .category = RVAL, 
        .type = to_type, 
        .pstring = cast_pstring ? concat(rpn_value.pstring, cast_pstring) : rpn_value.pstring,
    };

    if(D != CHECK)
    {
        sfixed_int_t const original = to_signed(rpn_value.fixed().value, { TYPE_REAL });
        fixed_int_t value = rpn_value.fixed().value;
        fixed_int_t const mask = numeric_bitmask(to_type.name());
        if(fixed_int_t z = builtin::ctz(mask))
            value += (1ull << (z - 1)) & value;
        value &= mask;

        if(implicit)
        {
            fixed_int_t const supermask = numeric_supermask(to_type.name());
            if((to_signed(value, to_type.name()) & supermask) != (rpn_value.sfixed() & supermask))
            {
                file_contents_t file(rpn_value.pstring.file_i);
                throw compiler_error_t(
                    fmt_error(file, rpn_value.pstring, fmt(
                        "Num value of % doesn't fit in type %. (Implicit type conversion)", 
                        to_double(fixed_t{original}), to_type))
                    + fmt_note("Add an explicit cast operator to override.")
                    );
            }
        }

        assert(is_masked({value}, to_type.name()));

        new_rpn.value.set(fixed_t{ value });
        assert(new_rpn.value.fixed().value == value);
    }

    rpn_value = std::move(new_rpn);
}


// This is used to implement the other cast functions.
template<eval_t::do_t D>
void eval_t::force_boolify(rpn_value_t& rpn_value, pstring_t cast_pstring)
{
    rpn_value_t new_rpn =
    {
        .category = RVAL, 
        .type = { TYPE_BOOL }, 
        .pstring = cast_pstring ? concat(rpn_value.pstring, cast_pstring) : rpn_value.pstring,
    };


    if(is_interpret(D))
    {
        if(is_arithmetic(rpn_value.type.name()))
            new_rpn.value.set(boolify(rpn_value.fixed()));
    }
    else if(D == COMPILE)
    {
        assert(false);
    }

    rpn_value = std::move(new_rpn);
}

template<eval_t::do_t D>
bool eval_t::cast(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    switch(can_cast(rpn_value.type, to_type, implicit))
    {
    default: assert(false);
    case CAST_FAIL: 
        return false;
    case CAST_NOP:
        rpn_value.type = to_type;
        rpn_value.category = RVAL;
        return true;
    case CAST_PROMOTE:
        force_promote<D>(rpn_value, to_type, cast_pstring);
        return true;
    case CAST_TRUNCATE:
        force_truncate<D>(rpn_value, to_type, cast_pstring);
        return true;
    case CAST_BOOLIFY:
        force_boolify<D>(rpn_value, cast_pstring);
        return true;
    case CAST_CONVERT_INT:
        force_convert_int<D>(rpn_value, to_type, implicit, cast_pstring);
        return true;
    case CAST_ROUND_REAL:
        force_round_real<D>(rpn_value, to_type, implicit, cast_pstring);
        return true;
    }
}

template<eval_t::do_t D>
void eval_t::throwing_cast(rpn_value_t& rpn_value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    if(!cast<D>(rpn_value, to_type, implicit, cast_pstring))
    {
        compiler_error(rpn_value.pstring, fmt(
            "Unable to perform % type cast from % to %.", 
            (implicit ? "implicit": "explicit"), rpn_value.type, to_type));
    }
}
// Converts multiple values at once, but only if all casts are valid.
// On success, -1 is returned and 'val_begin' to 'val_end' may be modified to their casted type.
// On failure, an andex into 'begin' is return, with the failed cast.
template<eval_t::do_t D>
int eval_t::cast_args(pstring_t pstring, rpn_value_t* begin, 
                      rpn_value_t* end, type_t const* type_begin, bool implicit)
{
    assert(begin <= end);
    std::size_t const size = end - begin;

    cast_result_t* results = ALLOCA_T(cast_result_t, size);
    for(std::size_t i = 0; i != size; ++i)
        if(!(results[i] = can_cast(begin[i].type, type_begin[i], true)))
            return i;

    for(std::size_t i = 0; i != size; ++i)
    {
        /*
        if(results[i] == CAST_OP)
            force_cast<D>(begin[i], type_begin[i]);
        else if(results[i] == CAST_BOOLIFY)
            force_boolify<D>(begin[i]);
        else if(results[i] == CAST_ROUND_NUM)
            force_round_num<D>(begin[i], type_begin[i]);
            */
        assert(false); // TODO
    }

    return -1; // means no errors!
}

