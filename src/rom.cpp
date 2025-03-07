#include "rom.hpp"

#include "globals.hpp"
#include "group.hpp"
#include "asm_proc.hpp"
#include "ir.hpp"

//////////////////
// rom_array_ht //
//////////////////


/////////////////
// rom_array_t //
/////////////////

rom_array_t::rom_array_t(loc_vec_t&& vec, romv_allocs_t const& a, rom_key_t const&, bool align)
: rom_data_t(a, ROMVF_IN_MODE, align)
, m_used_in_group_data(group_data_ht::bitset_size())
{
    assert(compiler_phase() <= rom_array_ht::phase);

    // Just to be safe, we'll strip byteify information:
    for(locator_t& loc : vec)
        loc.set_byteified(false);

    m_data = std::move(vec);
}

void rom_array_t::mark_used_by(group_data_ht gd)
{
    assert(gd);
    assert(compiler_phase() < rom_array_ht::phase);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_used_in_group_data.set(gd.id);
}

void rom_array_t::for_each_locator(std::function<void(locator_t)> const& fn) const
{
    for(locator_t loc : data())
        fn(loc);
}

rom_array_ht rom_array_t::make(loc_vec_t&& vec, bool align, rom_rule_t rule, group_data_ht gd, romv_allocs_t const& a)
{
    std::hash<loc_vec_t> hasher;
    auto const hash = hasher(vec);

    rom_array_ht ret = rom_array_ht::with_pool([&, hash](auto& pool)
    {
        rh::apair<rom_array_ht*, bool> result = m_pool_map.emplace(hash,
            [&](rom_array_ht h) -> bool
            {
                return std::equal(pool[h.id].m_data.begin(), pool[h.id].m_data.end(), 
                                  vec.begin(), vec.end());
            },
            [&]()
            { 
                rom_array_ht const ret = { pool.size() };
                pool.emplace_back(std::move(vec), a, rom_key_t(), align);
                return ret;
            });

        return *result.first;
    });

    if(align)
        ret.safe().mark_aligned();

    if(rule)
        ret.safe().mark_rule(rule);

    if(gd)
        ret.safe().mark_used_by(gd);

    return ret;
}

void locate_rom_arrays(ir_t& ir, rom_proc_ht rom_proc)
{
    assert(compiler_phase() <= PHASE_ALLOC_ROM);

    for(cfg_node_t const& cfg : ir)
    for(ssa_ht ssa_it = cfg.ssa_begin(); ssa_it;)
    {
        unsigned const input_size = ssa_it->input_size();
        unsigned begin = 0;
        unsigned end = input_size;

        auto const is_uninitialized = [](ssa_value_t v)
        {
            return v.holds_ref() && v->op() == SSA_uninitialized;
        };

        if(ssa_it->op() != SSA_init_array)
            goto next_iter;
        
        // We're looking for SSA_init_arrays of all constants
        // TODO: Also handle arrays of *mostly* constants.
        for(unsigned i = 0; i < input_size; ++i)
        {
            ssa_value_t const input = ssa_it->input(i);
            if(!input.is_const() && !is_uninitialized(input))
                goto next_iter;
        }

        // If the beginning and/or end of the array is uninitialized,
        // we don't have to include it.

        for(unsigned i = 0; i < input_size; ++i)
        {
            ssa_value_t const input = ssa_it->input(i);
            if(!is_uninitialized(input))
                break;
            ++begin;
        }

        for(unsigned i = input_size-1; i < input_size; --i)
        {
            ssa_value_t const input = ssa_it->input(i);
            if(!is_uninitialized(input))
                break;
            --end;
        }

        // Now build the rom_array_t
        {
            unsigned const size = end - begin;
            loc_vec_t vec;
            vec.resize(size);

            for(unsigned i = 0; i < size; ++i)
            {
                ssa_value_t const input = ssa_it->input(begin + i);

                if(input.is_locator())
                    vec[i] = input.locator();
                else if(input.is_num())
                {
                    passert((input.whole() & 0xFF) == input.whole(), input.whole());
                    vec[i] = locator_t::const_byte(input.whole());
                }
                else if(is_uninitialized(input))
                    vec[i] = locator_t::const_byte(0);
                else
                    assert(false);
            }

            locator_t loc = locator_t::rom_array(rom_array_t::make(std::move(vec), false, ROMR_NORMAL));
            loc.advance_offset(-begin);

            ssa_it->replace_with(loc);
            ssa_it = ssa_it->prune();
            continue;
        }
    next_iter:
        ++ssa_it;
    }
}

////////////////
// rom_proc_t //
////////////////

void rom_proc_t::assign(asm_proc_t&& asm_proc)
{
    assert(compiler_phase() <= rom_proc_ht::phase);
    m_asm_proc = std::move(asm_proc);
    m_max_size = m_asm_proc.size();
}

xbitset_t<group_ht> const* rom_proc_t::uses_groups() const 
{ 
    assert(compiler_phase() > rom_proc_ht::phase);
    return m_asm_proc.fn ? &m_asm_proc.fn->ir_deref_groups() : nullptr; 
}

bool rom_proc_t::for_each_group_test(std::function<bool(group_ht)> const& fn)
{
    if(auto const* bs = uses_groups())
        return bs->for_each_test([&](group_ht group){ return fn(group); });
    return true;
}

void rom_proc_t::for_each_locator(std::function<void(locator_t)> const& fn) const
{
    for(asm_inst_t const& inst : asm_proc().code)
    {
        if(inst.arg)
            fn(inst.arg);
        if(inst.alt)
            fn(inst.alt);
    }
}

//////////////////////
// rom data generic //
//////////////////////

rom_data_ht to_rom_data(loc_vec_t&& data, bool align, romv_allocs_t const& a)
{
    return rom_array_t::make(std::move(data), align, ROMR_NORMAL, {}, a);
}

rom_data_ht to_rom_data(asm_proc_t&& asm_proc, bool align, romv_allocs_t const& a, romv_flags_t desired_romv)
{
    return rom_proc_ht::pool_make(std::move(asm_proc), a, desired_romv, align);
}

/////////////////
// rom_data_ht //
/////////////////

rom_data_ht::rom_data_ht(rom_array_ht h) { assign(ROMD_ARRAY, h.id); }
rom_data_ht::rom_data_ht(rom_proc_ht h) { assign(ROMD_PROC, h.id); }

rom_data_t* rom_data_ht::get() const
{
    switch(rclass())
    {
    default: return nullptr;
    case ROMD_ARRAY: return &rom_array_ht{handle()}.safe();
    case ROMD_PROC: return &rom_proc_ht{handle()}.safe();
    }
}

unsigned rom_data_ht::max_size() const
{
    switch(rclass())
    {
    default: 
        return 0;
    case ROMD_ARRAY:
        assert(rom_array_ht{ handle() }->data().size() < 1 << 16);
        return rom_array_ht{ handle() }->data().size();
    case ROMD_PROC:
        return rom_proc_ht{ handle() }->max_size();
    }
}


void rom_data_ht::visit(std::function<void(rom_array_ht)> const& array_fn, 
                        std::function<void(rom_proc_ht)> const& proc_fn) const
{
    switch(rclass())
    {
    default: return;
    case ROMD_ARRAY: return array_fn(rom_array_ht{ handle() });
    case ROMD_PROC: return proc_fn(rom_proc_ht{ handle() });
    }
}

void rom_data_ht::for_each_locator(std::function<void(locator_t)> const& fn) const
{
    switch(rclass())
    {
    default: 
        return;
    case ROMD_ARRAY:
        rom_array_ht{ handle() }->for_each_locator(fn);
        return;
    case ROMD_PROC:
        rom_proc_ht{ handle() }->for_each_locator(fn);
        return;
    }
}

//////////////////
// rom_alloc_ht //
//////////////////

rom_alloc_ht::rom_alloc_ht(rom_static_ht h) { assign(ROMA_STATIC, h.id); }
rom_alloc_ht::rom_alloc_ht(rom_many_ht h) { assign(ROMA_MANY, h.id); }
rom_alloc_ht::rom_alloc_ht(rom_once_ht h) { assign(ROMA_ONCE, h.id); }

rom_alloc_t* rom_alloc_ht::get() const
{
    switch(rclass())
    {
    default: return nullptr;
    case ROMA_STATIC: return &rom_static_ht{handle()}.safe();
    case ROMA_MANY: return &rom_many_ht{handle()}.safe();
    case ROMA_ONCE: return &rom_once_ht{handle()}.safe();
    }
}

int rom_alloc_ht::first_bank() const
{
    switch(rclass())
    {
    default: 
        return -1;
    case ROMA_STATIC:
        return 0;
    case ROMA_MANY: 
        return rom_many_ht{handle()}->in_banks.lowest_bit_set(); // This returns -1 on error
    case ROMA_ONCE: 
        if(!rom_once_ht{handle()}->span)
            return -1;
        return rom_once_ht{handle()}->bank;
    }
}

////////////////
// rom_many_t //
////////////////

rom_many_t::rom_many_t(romv_t romv, rom_data_ht data, std::uint16_t desired_alignment)
{
    assert(data.rclass());
    this->desired_alignment = desired_alignment;
    this->romv = romv;
    this->data = data;
}

////////////////
// rom_once_t //
////////////////

rom_once_t::rom_once_t(romv_t romv, rom_data_ht data, std::uint16_t desired_alignment)
{
    assert(data.rclass());
    this->desired_alignment = desired_alignment;
    this->romv = romv;
    this->data = data;
}
