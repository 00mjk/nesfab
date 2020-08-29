#include "constraints.hpp"

#include <algorithm>
#include <string>
#include <ostream> // TODO

#include "builtin.hpp"
#include "format.hpp"

extern std::uint8_t const add_constraints_table[1024];

// For debugging mostly
std::string to_string(bounds_t const& b)
{
    return fmt("[%, %]", to_double(fixed_t{ b.min }), 
                         to_double(fixed_t{ b.max }));
}

// For debugging mostly
std::string to_string(known_bits_t const& b)
{
    fixed_int_t const known = b.known();
    std::string str;
    for(fixed_int_t i = 0; i < sizeof_bits<fixed_int_t>; ++i)
    {
        if(i > 0 && i % 8 == 0)
            str.push_back(' ');
        fixed_int_t bit = 1ull << i;
        if(known & bit)
        {
            if(b.known0 & b.known1 & bit)
                str.push_back('T');
             else if(b.known0 & bit)
                str.push_back('0');
             else
                str.push_back('1');
        }
        else
            str.push_back('?');
    }
    std::reverse(str.begin(), str.end());
    return str;
}

// For debugging mostly
std::string to_string(constraints_t const& c) 
{
    return fmt("{ %, %, % }%", 
               to_string(c.bits), 
               to_string(c.bounds), 
               to_string(c.carry), 
               c.is_val_const() ? " (CONST)" : "");
}

// For debugging mostly
std::ostream& operator<<(std::ostream& o, bounds_t const& b)
{
    o << to_string(b);
    return o;
}

// For debugging mostly
std::ostream& operator<<(std::ostream& o, known_bits_t const& b)
{
    o << to_string(b);
    return o;
}

// For debugging mostly
std::ostream& operator<<(std::ostream& o, constraints_t const& c)
{
    o << to_string(c);
    return o;
}

bounds_t apply_mask(fixed_int_t mask, bounds_t b)
{
    if(b.is_top())
        return bounds_t::top();
    if(b.max > mask)
    {
        fixed_int_t span = b.max - b.min;
        b.min &= mask;
        b.max = b.min + span;
        if(b.max > mask)
            b = { 0, mask };
    }
    assert(!b.is_top());
    return b;
}

known_bits_t apply_mask(fixed_int_t mask, known_bits_t b)
{
    if(b.is_top())
        return known_bits_t::top();
    b.known0 |= ~mask;
    b.known1 &= mask;
    assert(!b.is_top());
    return b;
}

constraints_t apply_mask(fixed_int_t mask, constraints_t c)
{
    return { apply_mask(mask, c.bounds), apply_mask(mask, c.bits), c.carry };
}

bounds_t from_bits(known_bits_t bits)
{
    if(bits.is_top())
        return bounds_t::top();
    bounds_t ret = { bits.known1, ~bits.known0 };
    assert(!ret.is_top());
    return ret;
}

known_bits_t from_bounds(bounds_t bounds, fixed_int_t first_bit)
{
    if(bounds.is_top())
        return known_bits_t::top();
    // Find upper bits that are the same.
    fixed_int_t x = bounds.min ^ bounds.max;
    fixed_int_t mask = ~0ull;
    if(x)
        mask = ~((1ull << (fixed_int_t)builtin::rclz(x)) - 1ull);
    assert((x & mask) == 0ull);
    known_bits_t ret;
    ret.known0 = ~bounds.min & mask;
    ret.known1 = bounds.min & mask;
    assert(!ret.is_top());
    return ret;
}

bounds_t intersect(bounds_t a, bounds_t b)
{
    return { std::max(a.min, b.min), std::min(a.max, b.max) };
}

known_bits_t intersect(known_bits_t a, known_bits_t b)
{
    return { a.known0 | b.known0, a.known1 | b.known1 };
}

carry_t intersect(carry_t a, carry_t b)
{
    return (carry_t)(a | b);
}

constraints_t intersect(constraints_t a, constraints_t b)
{
    return 
    { 
        intersect(a.bounds, b.bounds),
        intersect(a.bits, b.bits),
        intersect(a.carry, b.carry),
    };
}

bounds_t union_(bounds_t a, bounds_t b)
{
    if(a.is_top())
        return b;
    if(b.is_top())
        return a;
    return { std::min(a.min, b.min), std::max(a.max, b.max) };
}

known_bits_t union_(known_bits_t a, known_bits_t b)
{
    if(a.is_top())
        return b;
    if(b.is_top())
        return a;
    return { a.known0 & b.known0, a.known1 & b.known1 };
}

carry_t union_(carry_t a, carry_t b)
{
    return (carry_t)(a & b);
}

constraints_t union_(constraints_t a, constraints_t b)
{
    return 
    { 
        union_(a.bounds, b.bounds),
        union_(a.bits, b.bits),
        union_(a.carry, b.carry),
    };
}

bool is_subset(bounds_t small, bounds_t big)
{
    if(big.is_top())
        return small.is_top();
    if(small.is_top())
        return true;
    return small.min >= big.min && small.max <= big.max;
}

bool is_subset(known_bits_t small, known_bits_t big)
{
    if(big.is_top())
        return small.is_top();
    if(small.is_top())
        return true;
    return (intersect(big, small).bit_eq(small)
            && union_(small, big).bit_eq(big));
}

bool is_subset(carry_t small, carry_t big)
{
    return small == big || big == CARRY_BOTTOM;
}

bool is_subset(constraints_t small, constraints_t big)
{
    return (intersect(big, small).normal_eq(small)
            && union_(small, big).normal_eq(big));
}

// Narrows bounds based on the constraint's bits.
// e.g. if bounds are [1, 5] and bit 0 is known to be 0, narrows to [2, 4].
bounds_t tighten_bounds(constraints_t c)
{
    fixed_int_t const known = c.bits.known();
    c.bounds.max = ~c.bounds.max;

    fixed_int_t const bit_min = builtin::ctz(known);
    fixed_int_t const bit_max = builtin::rclz(known);

    for(fixed_int_t i = bit_min; i < bit_max; ++i)
    {
        fixed_int_t const bit = (1ull << i) & known;
        if((c.bounds.min ^ c.bits.known1) & bit)
        {
            c.bounds.min += bit;
            c.bounds.min &= ~(bit - 1) | known;
        }
        if((c.bounds.max ^ c.bits.known0) & bit)
        {
            c.bounds.max += bit;
            c.bounds.max &= ~(bit - 1) | known;
        }
    }

    c.bounds.max = ~c.bounds.max;
    return c.bounds;
}

static constraints_t normalize_impl(constraints_t c)
{
    c.bounds = intersect(c.bounds, from_bits(c.bits));
    c.bits = intersect(c.bits, from_bounds(c.bounds));
    c.bounds = tighten_bounds(c);
    c.bits = intersect(c.bits, from_bounds(c.bounds));
    if(c.is_top())
        return constraints_t::top();
    return c;
}

constraints_t normalize(constraints_t c)
{
    constraints_t ret = normalize_impl(c);
    assert(ret.bit_eq(normalize_impl(ret)));
    return ret;
}

template<ssa_op_t>
static constexpr abstract_fn_t* abstract_fn_v = nullptr;

template<ssa_op_t>
static constexpr narrow_fn_t* narrow_fn_v = nullptr;

#define ABSTRACT(op) template<> constexpr abstract_fn_t* abstract_fn_v<op> = \
[](fixed_int_t mask, constraints_t const* c, unsigned argn) -> constraints_t

#define NARROW(op) template<> constexpr narrow_fn_t* narrow_fn_v<op> = \
[](fixed_int_t mask, constraints_t result, constraints_t* c, unsigned argn)

ABSTRACT(SSA_phi)
{
    constraints_t ret = constraints_t::top();
    for(unsigned i = 0; i < argn; ++i)
        ret = union_(ret, c[i]);
    return ret;
};

ABSTRACT(SSA_read_global)
{
    assert(argn == 2);
    return constraints_t::bottom(mask);
};

ABSTRACT(SSA_fn_call)
{
    return constraints_t::bottom(mask);
};

ABSTRACT(SSA_uninitialized)
{
    assert(argn == 0);
    return constraints_t::bottom(mask);
};

ABSTRACT(SSA_cast)
{
    assert(argn == 1);
    return apply_mask(mask, c[0]);
};
#include <iostream> // TODO

ABSTRACT(SSA_add)
{
    assert(argn == 3);

    constexpr unsigned C = 0;
    constexpr unsigned L = 1;
    constexpr unsigned R = 2;

    if(c[L].is_top() || c[R].is_top() || c[C].is_top())
        return constraints_t::top();

    std::cout << "L = " << c[L] << '\n';
    std::cout << "R = " << c[R] << '\n';
    std::cout << "C = " << c[C] << '\n';
    std::cout << "M = " << mask << '\n';

    // If we know bits in c[L] and c[R], we can determine which bits are
    // known in the output.
    //
    // SLOW BUT ACCURATE TECHNIQUE (used)
    // - Treat constraints as ternary. Trits can be 0, 1, or ? (unknown value).
    // - Add using the standard arithmetic algorithm, starting at the
    //   rightmost bit and working left, carrying as you go.
    // - When adding to a ?, the output trit is always ?, and the carry 
    //   depends on the other value. Carrying a ? is allowed!
    // - Performance is improved slightly by using lookup tables.
    //
    // FAST BUT INACCURATE TECHNIQUE (not used)
    // - If the rightmost bits of both c[L] and c[R] are known, we know
    //   the rightmost bits of the output.
    // - If we know both bits along with the carry, we know the outpit bit.
    //   (The carry is known when c[L] bit == c[R] bit.)
    //
    // (FAST is about 10x faster than SLOW)

    constraints_t ret = {};

    fixed_int_t const neg_mask = ~(c[L].bits.known0 & c[R].bits.known0) & mask;
    std::uint64_t const start_i = neg_mask ? builtin::ctz(neg_mask) : 0;
    std::uint64_t const end_i = ((2 + (neg_mask ? builtin::rclz(neg_mask) 
                                                : sizeof_bits<fixed_int_t>))
                                 & ~1ull);
    ret.bits.known0 = (1ull << start_i) - 1ull;

    known_bits_t lhs_bits = c[L].bits;
    known_bits_t rhs_bits = c[R].bits;

    lhs_bits.known0 >>= start_i;
    lhs_bits.known1 >>= start_i;
    rhs_bits.known0 >>= start_i;
    rhs_bits.known1 >>= start_i;

    fixed_int_t i = start_i;
    // 'j' holds the carry.
    fixed_int_t j = c[C].carry;
    assert(c[C].carry != CARRY_TOP);
    for(; i < end_i; i += 2ull)
    {
        j |= (lhs_bits.known0 & 0b11) << 2ull;
        j |= (lhs_bits.known1 & 0b11) << 4ull;
        j |= (rhs_bits.known0 & 0b11) << 6ull;
        j |= (rhs_bits.known1 & 0b11) << 8ull;

        j = add_constraints_table[j];
        ret.bits.known0 |= ((j >> 2ull) & 0b11ull) << i;
        ret.bits.known1 |= ((j >> 4ull) & 0b11ull) << i;
        j &= 0b11;

        lhs_bits.known0 >>= 2ull;
        lhs_bits.known1 >>= 2ull;
        rhs_bits.known0 >>= 2ull;
        rhs_bits.known1 >>= 2ull;
    }
    ret.carry = (carry_t)j;
    std::cout << "RET = " << ret << '\n';
    assert(ret.carry != CARRY_TOP);
    if(i < sizeof_bits<fixed_int_t>)
        ret.bits.known0 |= ~((1ull << i) - 1ull);

    ret.bits = apply_mask(mask, ret.bits);
    std::cout << "RET = " << ret << '\n';
    assert(!ret.bits.is_top());

    // OK! Now for the bounds.
    // Calculate 'min' and 'max.
    if(builtin::add_overflow(c[L].bounds.max, c[R].bounds.max, ret.bounds.max))
    {
        // Value overflowed. Derive min/max from known bits.
        ret.bounds = apply_mask(mask, from_bits(ret.bits));
        assert(!ret.is_top());
        return normalize(ret);
    }
    ret.bounds.min = c[L].bounds.min + c[R].bounds.min;
    ret.bounds = apply_mask(mask, ret.bounds);

    // 'min' and 'max' put constraints on the bits - neat!
    // Calculate those constraints here.
    assert(apply_mask(mask, ret.bits).bit_eq(ret.bits));
    known_bits_t bounds_bits = apply_mask(mask, from_bounds(ret.bounds));
    assert(apply_mask(mask, bounds_bits).bit_eq(bounds_bits));
    ret.bits.known0 |= bounds_bits.known0;
    ret.bits.known1 |= bounds_bits.known1;
    std::cout << "RET = " << ret << '\n';

    assert(apply_mask(mask, ret.bits).bit_eq(ret.bits));
    assert(!ret.bounds.is_top());
    assert(!ret.bits.is_top());
    return normalize(ret);
};

ABSTRACT(SSA_and)
{
    assert(argn == 2);
    if(c[0].is_top() || c[1].is_top())
        return constraints_t::top();
    constraints_t ret;
    ret.bits.known0 = c[0].bits.known0 | c[1].bits.known0 | ~mask;
    ret.bits.known1 = (c[0].bits.known1 & c[1].bits.known1) & mask;
    ret.bounds = from_bits(ret.bits);
    ret.carry = CARRY_BOTTOM;
    assert(ret.bounds.max <= mask);
    assert(!ret.is_top() && ret.is_normalized());
    return ret;
};

ABSTRACT(SSA_or)
{
    assert(argn == 2);
    if(c[0].is_top() || c[1].is_top())
        return constraints_t::top();
    constraints_t ret;
    ret.bits.known0 = (c[0].bits.known0 & c[1].bits.known0) | ~mask;
    ret.bits.known1 = (c[0].bits.known1 | c[1].bits.known1) & mask;
    ret.bounds = from_bits(ret.bits);
    ret.carry = CARRY_BOTTOM;
    assert(ret.bounds.max <= mask);
    assert(!ret.is_top() && ret.is_normalized());
    return ret;
};

ABSTRACT(SSA_xor)
{
    assert(argn == 2);
    if(c[0].is_top() || c[1].is_top())
        return constraints_t::top();
    fixed_int_t const known = c[0].bits.known() & c[1].bits.known() & mask;
    fixed_int_t const x = c[0].bits.known1 ^ c[1].bits.known1;
    constraints_t ret;
    ret.bits = { (~x & known) | ~mask, x & known };
    ret.bounds = from_bits( ret.bits);
    ret.carry = CARRY_BOTTOM;
    assert(ret.bounds.max <= mask);
    assert(!ret.is_top() && ret.is_normalized());
    return ret;
};

ABSTRACT(SSA_eq)
{
    assert(argn == 2);
    if(c[0].is_top() || c[1].is_top())
        return constraints_t::top();
    if(c[0].bits.known0 & c[1].bits.known1)
        return constraints_t::whole(0, CARRY_BOTTOM);
    if(c[0].bits.known1 & c[1].bits.known0)
        return constraints_t::whole(0, CARRY_BOTTOM);
    if(c[0].bounds.min > c[1].bounds.max 
       || c[0].bounds.max < c[1].bounds.min)
        return constraints_t::whole(0, CARRY_BOTTOM);
    if(c[0].is_val_const() && c[1].is_val_const() 
       && c[0].get_val_const() == c[1].get_val_const())
        return constraints_t::whole(1, CARRY_BOTTOM);
    return constraints_t::any_bool(CARRY_BOTTOM);
};

ABSTRACT(SSA_not_eq)
{
    assert(argn == 2);
    if(c[0].is_top() || c[1].is_top())
        return constraints_t::top();
    if(c[0].bits.known0 & c[1].bits.known1)
        return constraints_t::whole(1, CARRY_BOTTOM);
    if(c[0].bits.known1 & c[1].bits.known0)
        return constraints_t::whole(1, CARRY_BOTTOM);
    if(c[0].bounds.min > c[1].bounds.max 
       || c[0].bounds.max < c[1].bounds.min)
        return constraints_t::whole(1, CARRY_BOTTOM);
    if(c[0].is_val_const() && c[1].is_val_const() 
       && c[0].get_val_const() == c[1].get_val_const())
        return constraints_t::whole(0, CARRY_BOTTOM);
    return constraints_t::any_bool(CARRY_BOTTOM);
};

ABSTRACT(SSA_lt)
{
    assert(argn == 2);
    if(c[0].is_top() || c[1].is_top())
        return constraints_t::top();
    if(c[0].bounds.max < c[1].bounds.min)
        return constraints_t::whole(1, CARRY_BOTTOM);
    if(c[1].bounds.max <= c[0].bounds.min)
        return constraints_t::whole(0, CARRY_BOTTOM);
    return constraints_t::any_bool(CARRY_BOTTOM);
};

ABSTRACT(SSA_lte)
{
    assert(argn == 2);
    if(c[0].is_top() || c[1].is_top())
        return constraints_t::top();
    if(c[0].bounds.max <= c[1].bounds.min)
        return constraints_t::whole(1, CARRY_BOTTOM);
    if(c[1].bounds.max < c[0].bounds.min)
        return constraints_t::whole(0, CARRY_BOTTOM);
    return constraints_t::any_bool(CARRY_BOTTOM);
};

NARROW(SSA_phi)
{
    for(unsigned i = 0; i < argn; ++i)
        c[i] = intersect(c[i], result);
};

NARROW(SSA_uninitialized)
{
    assert(argn == 0);
    c[0] = result;
};

NARROW(SSA_cast)
{
    assert(argn == 1);
    c[0] = result;
};

NARROW(SSA_add)
{
    assert(argn == 3);
    if(result.is_top())
        return;

    constexpr unsigned C = 0;
    constexpr unsigned L = 1;
    constexpr unsigned R = 2;

    // We use an approximation approach.
    // We can solve bit equations of the form KNOWN ^ KNOWN ^ UNKNOWN = KNOWN
    // (Three arguments because of carries).

    // Determine some of the carried bits:
    fixed_int_t carry0 = (c[L].bits.known0 & c[R].bits.known0) << 1ull;
    fixed_int_t carry1 = (c[L].bits.known1 & c[R].bits.known1) << 1ull;

    fixed_int_t const carry_i = ~mask ? ((mask | (mask >> 1)) ^ mask) : 1;
    
    // First do the carry. If we know the lowest bit of c[L], c[R], and result
    // we can infer the required carry.
    if(result.bits.known() & c[L].bits.known() & c[R].bits.known() & carry_i)
    {
        if((result.bits.known1 ^ c[L].bits.known1 ^ c[R].bits.known1)& carry_i)
            c[C].carry = CARRY_SET;
        else
            c[C].carry = CARRY_CLEAR;
    }

    // If the SSA op has a carry input, use it in the lowest bit:
    switch(c[C].carry)
    {
    case CARRY_BOTTOM: break;
    case CARRY_CLEAR:  carry0 |= 0ull; break;
    case CARRY_SET:    carry1 |= 1ull; break;
    case CARRY_TOP:    return;
    }

    fixed_int_t const solvable = result.bits.known() & (carry0 | carry1);
    fixed_int_t const lsolvable = c[R].bits.known() & solvable;
    fixed_int_t const rsolvable = c[L].bits.known() & solvable;

    c[L].bits.known1 |= ((carry1 ^ c[R].bits.known1 ^ result.bits.known1) 
                         & lsolvable);
    c[R].bits.known1 |= ((carry1 ^ c[L].bits.known1 ^ result.bits.known1) 
                         & rsolvable);
    c[L].bits.known0 |= ~c[L].bits.known1 & lsolvable;
    c[R].bits.known0 |= ~c[R].bits.known1 & rsolvable;

    // Move the bounds in after calculating bits.
    c[L].bounds = intersect(c[L].bounds, from_bits(c[L].bits));
    c[R].bounds = intersect(c[R].bounds, from_bits(c[R].bits));

    fixed_int_t max_sum;
    if(builtin::add_overflow(c[L].bounds.max, c[R].bounds.max, max_sum))
    {
        assert(c[L].is_normalized() && c[R].is_normalized());
        return;
    }

    if(max_sum > mask)
    {
        // The original add can overflow! This complicates things.
        fixed_int_t const min_sum = c[L].bounds.min + c[R].bounds.min;
        fixed_int_t const span = max_sum - min_sum;
        fixed_int_t const masked_min_sum = min_sum & mask;

        if(masked_min_sum + span > mask)
        {
            assert(c[L].is_normalized() && c[R].is_normalized());
            return;
        }

        // Un-mask the result.
        fixed_int_t const masked_diff = min_sum - masked_min_sum;
        result.bounds.min += masked_diff;
        result.bounds.max += masked_diff;
    }

    // If the result's max is less than expected, try lowering
    // c[L] and c[R]'s max bound.
    c[L].bounds.max = std::min(c[L].bounds.max, 
                               result.bounds.max - c[R].bounds.min);
    c[R].bounds.max = std::min(c[R].bounds.max, 
                               result.bounds.max - c[L].bounds.min);

    // If the result's min is greater than expected, try raising
    // c[L] and c[R]'s min bound.
    if(result.bounds.min > c[R].bounds.max)
        c[L].bounds.min = std::max(c[L].bounds.min, 
                                   result.bounds.min - c[R].bounds.max);
    if(result.bounds.min > c[R].bounds.max)
        c[R].bounds.min = std::max(c[R].bounds.min,
                                   result.bounds.min - c[L].bounds.max);
};

NARROW(SSA_and)
{
    assert(argn == 2);
    if(result.is_top())
        return;

    // If output bit is true, both inputs must be true.
    c[0].bits.known1 |= result.bits.known1;
    c[1].bits.known1 |= result.bits.known1;

    // If we know the output is 0, and we know one input bit is 1,
    // the other input bit must be 0.
    c[0].bits.known0 |= result.bits.known0 & c[1].bits.known1;
    c[1].bits.known0 |= result.bits.known0 & c[0].bits.known1;
};

NARROW(SSA_or)
{
    assert(argn == 2);
    if(result.is_top())
        return;

    // If output bit is false, both inputs must be false.
    c[0].bits.known0 |= result.bits.known0;
    c[1].bits.known0 |= result.bits.known0;

    // If we know the output is 1, and we know one input bit is 0,
    // the other input bit must be 1.
    c[0].bits.known1 |= result.bits.known1 & c[1].bits.known0;
    c[1].bits.known1 |= result.bits.known1 & c[0].bits.known0;
};

NARROW(SSA_xor)
{
    assert(argn == 2);
    if(result.is_top())
        return;

    // If we know the output is 0, and we know one input bit is 0,
    // the other input bit must be 0.
    c[0].bits.known0 |= result.bits.known0 & c[1].bits.known0;
    c[1].bits.known0 |= result.bits.known0 & c[0].bits.known0;

    // If we know the output is 0, and we know one input bit is 1,
    // the other input bit must be 1.
    c[0].bits.known1 |= result.bits.known0 & c[1].bits.known1;
    c[1].bits.known1 |= result.bits.known0 & c[0].bits.known1;

    // If we know the output is 1, and we know one input bit is 1,
    // the other input bit must be 0.
    c[0].bits.known0 |= result.bits.known1 & c[1].bits.known1;
    c[1].bits.known0 |= result.bits.known1 & c[0].bits.known1;

    // If we know the output is 1, and we know one input bit is 0,
    // the other input bit must be 1.
    c[0].bits.known1 |= result.bits.known1 & c[1].bits.known0;
    c[1].bits.known1 |= result.bits.known1 & c[0].bits.known0;
};

static void narrow_eq(fixed_int_t mask, constraints_t result, 
                      constraints_t* c, unsigned argn, bool eq)
{
    assert(argn == 2);
    if(!result.is_val_const())
        return;

    if(result.get_val_const() == fixed_t::whole(!eq).value)
    {
        for(unsigned i = 0; i < 2; ++i)
        if(c[i].is_val_const())
        {
            unsigned const o = 1 - i;
            fixed_int_t const const_ = c[i].get_val_const();
            if(c[o].bounds.min == const_)
                ++c[o].bounds.min;
            if(c[o].bounds.max == const_)
                --c[o].bounds.max;
        }
    }
    else if(result.get_val_const() == fixed_t::whole(eq).value)
        c[0] = c[1] = intersect(c[0], c[1]);
}

NARROW(SSA_eq)
{
    narrow_eq(mask, result, c, argn, true);
};

NARROW(SSA_not_eq)
{
    narrow_eq(mask, result, c, argn, false);
};

NARROW(SSA_lt)
{
    assert(argn == 2);
    if(!result.is_val_const())
        return;

    if(result.get_val_const() == fixed_t::whole(0).value)
    {
        c[0].bounds.min = std::max(c[0].bounds.min, c[1].bounds.min);
        c[1].bounds.max = std::min(c[1].bounds.max, c[0].bounds.max);
    }
    else if(result.get_val_const() == fixed_t::whole(1).value)
    {
        c[0].bounds.max = std::min(c[0].bounds.max, c[1].bounds.max - 1);
        c[1].bounds.min = std::max(c[1].bounds.min, c[0].bounds.min + 1);
    }
};

NARROW(SSA_lte)
{
    assert(argn == 2);
    if(!result.is_val_const())
        return;

    if(result.get_val_const() == fixed_t::whole(0).value)
    {
        c[0].bounds.min = std::max(c[0].bounds.min, c[1].bounds.min + 1);
        c[1].bounds.max = std::min(c[1].bounds.max, c[0].bounds.max - 1);
    }
    else if(result.get_val_const() == fixed_t::whole(1).value)
    {
        c[0].bounds.max = std::min(c[0].bounds.max, c[1].bounds.max);
        c[1].bounds.min = std::max(c[1].bounds.min, c[0].bounds.min);
    }
};

#undef ABSTRACT
#undef NARROW

extern std::array<abstract_fn_t*, NUM_SSA_OPS> const abstract_fn_table =
{{
#define SSA_DEF(name, ...) abstract_fn_v<SSA_##name>,
#include "ssa_op.inc"
}};

extern std::array<narrow_fn_t*, NUM_SSA_OPS> const narrow_fn_table =
{{
#define SSA_DEF(name, ...) narrow_fn_v<SSA_##name>,
#include "ssa_op.inc"
}};
