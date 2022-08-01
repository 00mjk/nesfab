#ifndef DECL_HPP
#define DECL_HPP

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <deque>
#include <type_traits>

#include "robin/map.hpp"

#include "phase.hpp"
#include "handle.hpp"
#include "bitset.hpp"

constexpr unsigned MAX_FN_ARGS = 32;
constexpr unsigned MAX_MEMBERS = 256;
constexpr unsigned MAX_ATOMS = 8;

struct group_t;
class global_t;
class fn_t;
class gvar_t;
class gmember_t;
class const_t;
class struct_t;
class group_vars_t;
class group_data_t;
struct field_t;
struct lt_value_t;

#define GLOBAL_CLASS_XENUM \
    X(GLOBAL_UNDEFINED) \
    X(GLOBAL_FN) \
    X(GLOBAL_VAR) \
    X(GLOBAL_CONST) \
    X(GLOBAL_STRUCT)

enum global_class_t : std::uint8_t
{
#define X(x) x,
    GLOBAL_CLASS_XENUM
#undef X
};

enum group_class_t : std::uint8_t
{
    GROUP_UNDEFINED = 0,
    GROUP_VARS,
    GROUP_DATA,
};

struct lt_ht : pool_handle_t<lt_ht, std::deque<lt_value_t>, PHASE_COMPILE> {};

struct global_ht : pool_handle_t<global_ht, std::deque<global_t>, PHASE_PARSE> {};
struct fn_ht : pool_handle_t<fn_ht, std::deque<fn_t>, PHASE_PARSE> {};
struct gvar_ht : pool_handle_t<gvar_ht, std::deque<gvar_t>, PHASE_PARSE> {};
struct const_ht : pool_handle_t<const_ht, std::deque<const_t>, PHASE_PARSE> {};
struct struct_ht : pool_handle_t<struct_ht, std::deque<struct_t>, PHASE_PARSE> {};
struct gmember_ht : pool_handle_t<gmember_ht, std::vector<gmember_t>, PHASE_COUNT_MEMBERS> {};

struct group_ht : pool_handle_t<group_ht, std::deque<group_t>, PHASE_PARSE> 
{
    group_data_t* data() const; // Defined in group.cpp
};
struct group_vars_ht : pool_handle_t<group_vars_ht, std::deque<group_vars_t>, PHASE_PARSE> {};
struct group_data_ht : pool_handle_t<group_data_ht, std::deque<group_data_t>, PHASE_PARSE> {};

DEF_HANDLE_HASH(fn_ht);
DEF_HANDLE_HASH(gvar_ht);
DEF_HANDLE_HASH(const_ht);
DEF_HANDLE_HASH(struct_ht);
DEF_HANDLE_HASH(gmember_ht);
DEF_HANDLE_HASH(group_ht);
DEF_HANDLE_HASH(group_vars_ht);
DEF_HANDLE_HASH(group_data_ht);

/* TODO: remove?
template<typename, typename = void>
struct is_global_handle 
: std::false_type {};

template<typename t>
struct is_global_handle<t, std::void_t<typename t::global_handle_tag>>
: std::true_type {};

template<typename, typename = void>
struct is_global_impl 
: std::false_type {};

template<typename t>
struct is_global_impl<t, std::void_t<typename t::global_impl_tag>>
: std::true_type {};

template<typename, typename = void>
struct is_group_handle 
: std::false_type {};

template<typename t>
struct is_group_handle<t, std::void_t<typename t::group_handle_tag>>
: std::true_type {};

template<typename, typename = void>
struct is_group_impl 
: std::false_type {};

template<typename t>
struct is_group_impl<t, std::void_t<typename t::group_impl_tag>>
: std::true_type {};
*/

enum fclass_t : char
{
    FN_FN,
    FN_CT,
    FN_MODE,
};

#endif
