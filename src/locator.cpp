#include "locator.hpp"

#include "format.hpp"
#include "globals.hpp"
#include "group.hpp"
#include "type.hpp"
#include "ir.hpp"
#include "lt.hpp"
#include "rom.hpp"
#include "runtime.hpp"
#include "fnv1a.hpp"
#include "compiler_error.hpp"

std::string to_string(locator_t loc)
{
    std::string str;

    switch(loc.lclass())
    {
    default: 
        return fmt("unknown locator %", (int)loc.lclass());
    case LOC_NONE:
        return "none";
    case LOC_GMEMBER:
        str = fmt("gmember % %", loc.gmember()->gvar.global.name, loc.gmember()->member()); break;
    case LOC_GMEMBER_SET:
        str = fmt("gset %", loc.handle()); break;
    case LOC_PTR_SET:
        str = fmt("pset %", loc.handle()); break;
    case LOC_FN:
        str = fmt("fn %", loc.fn()->global.name); break;
    case LOC_STMT:
        str = fmt("stmt %", loc.handle()); break;
    case LOC_ARG:
        str = fmt("arg %", loc.fn()->global.name); break;
    case LOC_RETURN:
        str = fmt("ret %", loc.fn()->global.name); break;
    case LOC_CFG_LABEL:
        str = fmt("cfg label %", loc.cfg_node()); break;
    case LOC_MINOR_LABEL:
        str = fmt("minor label"); break;
    case LOC_NAMED_LABEL:
        str = fmt("named label %", loc.global()->name); break;
    case LOC_CONST_BYTE:
        str = "const byte"; break;
    case LOC_ADDR:
        str = "addr $" + to_hex_string(loc.data() + loc.offset()); break;
    case LOC_INDEX:
        str = "index $" + loc.data(); break;
    case LOC_SWITCH_LO_TABLE:
        str = fmt("switch_lo_table %", loc.handle()); break;
    case LOC_SWITCH_HI_TABLE:
        str = fmt("switch_hi_table %", loc.handle()); break;
    case LOC_SSA:
        str = fmt("ssa %", loc.handle()); break;
    case LOC_PHI:
        str = fmt("phi %", loc.handle()); break;
    case LOC_MINOR_VAR:
        str = fmt("minor var %", loc.fn()->global.name); break;
    case LOC_ASM_LOCAL_VAR:
        str = fmt("asm_local_var", loc.fn()->global.name); break;
    case LOC_ASM_GOTO_MODE:
        str = fmt("asm_goto_mode", loc.fn()->global.name); break;
    case LOC_ROM_ARRAY:
        str = fmt("rom_array %", loc.handle()); break;
    case LOC_GCONST:
        str = fmt("gconst %", loc.const_()->global.name); break;
    case LOC_DPCM:
        str = fmt("dpcm %", loc.const_()->global.name); break;
    case LOC_LT_EXPR:
        str = fmt("lt_expr % %", loc.handle(), loc.lt().safe().type); break;
    case LOC_THIS_BANK:
        str = "this bank"; break;
    case LOC_RESET_PROC:
        str = "reset proc"; break;
    case LOC_MAIN_MODE:
        str = "main mode"; break;
    case LOC_RESET_GROUP_VARS:
        str = fmt("reset group vars %", loc.group_vars()->group.name); break;
    case LOC_RUNTIME_ROM:
        str = fmt("runtime_rom %", loc.runtime_rom()); break;
    case LOC_RUNTIME_RAM:
        str = fmt("runtime_ram %", loc.runtime_ram()); break;
    case LOC_NMI_INDEX:
        str = fmt("nmi_index %", loc.fn()->global.name); break;
    case LOC_CARRY_PAIR:
        str = fmt("carry_pair % %", loc.first_carry(), loc.second_carry()); break;
    }

    if(has_arg_member_atom(loc.lclass()))
        str += fmt(" arg:% member:% atom:% offset:%", (int)loc.arg(), (int)loc.member(), (int)loc.atom(), (int)loc.offset());
    else
        str += fmt(" data:% offset:%", (int)loc.data(), (int)loc.offset());

    str += fmt(" byteified:% is:%", (int)loc.byteified(), (int)loc.is());

    return str;
}

std::ostream& operator<<(std::ostream& o, locator_t loc)
{
    o << to_string(loc);
    return o;
}

locator_t locator_t::from_ssa_value(ssa_value_t v)
{
    if(v.holds_ref())
        return ssa(v.handle());
    else if(v.is_num())
        return const_byte(v.whole());
    else if(v.is_locator())
        return v.locator();
    else
        return LOC_NONE;
}

std::size_t locator_t::mem_size() const
{
    return with_byteified(true).type().size_of();
}

mods_t const* locator_t::mods() const
{
    if(lclass() == LOC_ASM_LOCAL_VAR)
        return fn().safe().def().local_vars[arg()].mods();
    if(has_const(lclass()))
        return const_()->mods();
    if(has_gmember(lclass()))
        return gmember()->gvar.mods();
    if(has_fn(lclass()))
        return fn()->mods();
    return nullptr;
}

bool locator_t::mem_zp_only() const
{
    if(!mem_zp_valid())
        return false;

    if(mod_test(mods(), MOD_zero_page))
        return true;

    if(lclass() == LOC_ROM_ARRAY || is_lt(lclass()))
        return false;

    type_t const t = with_byteified(false).with_is(IS_DEREF).type();
    return is_ptr(t.name()) && (!has_arg_member_atom(lclass()) || member() == 0);
}

bool locator_t::mem_zp_valid() const
{
    return !mod_test(mods(), MOD_zero_page, false);
}

type_t locator_t::type() const
{
    auto const byteify = [&](type_t type) -> type_t
    {
        if(is_banked_ptr(type.name()))
        {
            if(member() == 1)
                type = TYPE_U;
            else
            {
                assert(member() == 0);
                type = type.with_banked(false);
            }
        }

        if(byteified())
        {
            if(type.name() == TYPE_TEA)
            {
                if(type.elem_type().size_of() > 1)
                    return type_t::tea(TYPE_U, type.array_length());
            }
            else if(is_scalar(type.name()))
            {
                if(type.size_of() > 1)
                    return TYPE_U;
            }
        }

        return type;
    };

    switch(is())
    {
    case IS_PTR:
        return byteify(TYPE_APTR);
    case IS_PTR_HI:
    case IS_BANK:
        return TYPE_U;
    default:
        break;
    }

    switch(lclass())
    {
    case LOC_GCONST:
        if(const_ht const c = const_())
            c->type();
        break;
    case LOC_ROM_ARRAY:
        if(rom_array_ht const a = rom_array())
            return type_t::tea(TYPE_U, a.safe().data().size()); // TODO: use a proper pointer, with groups
        break;
    case LOC_LT_EXPR:
        assert(lt());
        return byteify(lt().safe().type);
    case LOC_GMEMBER: 
        return byteify(gmember()->type());
    case LOC_ARG:
        return byteify(fn().safe().type().type(arg()));
    case LOC_RETURN:
        return byteify(fn().safe().type().return_type());
    case LOC_ASM_LOCAL_VAR:
        return byteify(fn().safe().def().local_vars[arg()].type());
    case LOC_CONST_BYTE:
        return TYPE_U;
    case LOC_SSA:
    case LOC_PHI:
        assert(compiler_phase() == PHASE_COMPILE);
        return byteify(ssa_node()->type());
    case LOC_NAMED_LABEL:
        if(auto const* lc = global()->local_consts())
        {
            assert(data() < lc->size());
            return (*lc)[data()].type();
        }
        break;
    case LOC_DPCM:
        return TYPE_U;
    default:
        break;
    }
    return TYPE_VOID;
}

locator_t locator_t::link(romv_t romv, fn_ht fn_h, int bank) const
{
    assert(compiler_phase() == PHASE_LINK);

    auto const from_span = [&](span_t span) -> locator_t
    { 
        if(!span)
            return *this;

        span.addr += offset();

        if(is() != IS_BANK)
            return addr(span.addr).with_is(is()); 

        return *this;
    };

    auto const from_offset = [&](rom_alloc_ht a, int span_offset) -> locator_t
    {
        if(a)
        {
            if(is() == IS_BANK)
            {
                int const bank = a.first_bank();
                if(bank < 0 || bank >= 256)
                    return *this;
                return locator_t::const_byte(bank);
            }
            else if(rom_alloc_t* alloc = a.get())
                return from_span(offset_span(alloc->span, span_offset));
        }
        return *this;
    };

    int span_offset = 0;

    switch(lclass())
    {
    case LOC_DPCM:
        if(rom_alloc_t* alloc = rom_alloc(romv).get())
        {
            assert(alloc->span.addr >= 0xC000);
            return locator_t::const_byte((alloc->span.addr - 0xC000) / 64);
        }
        return *this;

    case LOC_NAMED_LABEL:
        {
            global_t const& g = *global();

            if(g.gclass() == GLOBAL_FN)
            {
                auto const& proc = g.impl<fn_t>().rom_proc()->asm_proc();

                if(auto const* info = proc.lookup_label(*this))
                    return from_offset(rom_alloc(romv), info->offset);
                else // Likely a compiler bug:
                    throw std::runtime_error(fmt("Missing label during link: %", *this));
            }
            else if(g.gclass() == GLOBAL_VAR)
            {
                auto const& proc = std::get<asm_proc_t>(g.impl<gvar_t>().init_data());

                if(auto const* info = proc.lookup_label(*this))
                    return from_offset(rom_alloc(romv), info->offset);
                else // Likely a compiler bug:
                    throw std::runtime_error(fmt("Missing label during link: %", *this));
            }
            else if(g.gclass() == GLOBAL_CONST)
            {
                auto const& c = g.impl<const_t>();
                return from_offset(c.rom_array()->find_alloc(romv), c.paa_def()->offsets[data()]);
            }
        }
        return *this;

    case LOC_FN:
        // Functions with a known first bank must be called using that bank:
        if(is() == IS_BANK && fn()->first_bank_switch())
            return fn()->first_bank_switch().link(romv, fn_h, bank);

        {
            auto const& proc = fn()->rom_proc()->asm_proc();
            locator_t const label = 
                data() == ENTRY_LABEL ? proc.entry_label : locator_t::named_label(fn()->global.handle(), data());

            if(auto const* info = proc.lookup_label(label))
                span_offset = info->offset;
            else // Likely a compiler bug:
                throw std::runtime_error(fmt("Missing label during link: % / % ", label, *this));
        }

        // fall-through
    default:
        return from_offset(rom_alloc(romv), span_offset);

    case LOC_ADDR: // Remove the offset.
        return locator_t::addr(data() + offset()).with_is(is());

    case LOC_GMEMBER:
        return from_span(gmember()->span(atom()));

    case LOC_SSA:
    case LOC_PHI:
        {
            if(!fn_h)
                return *this;
            span_t span = fn_h->lvar_span(romv, mem_head());
            for(unsigned i = 0; !span && i < NUM_ROMV; ++i)
                span = fn_h->lvar_span(romv_t(i), mem_head());
            return from_span(fn_h->lvar_span(romv, mem_head()));
        }

    case LOC_ASM_LOCAL_VAR:
    case LOC_ARG:
    case LOC_RETURN:
    case LOC_MINOR_VAR:
        {
            span_t span = fn()->lvar_span(romv, mem_head());
            for(unsigned i = 0; !span && i < NUM_ROMV; ++i)
                span = fn()->lvar_span(romv_t(i), mem_head());
            return from_span(fn()->lvar_span(romv, mem_head()));
        }

    case LOC_THIS_BANK:
        if(bank >= 0 && bank < 256)
            return locator_t::const_byte(bank);
        return *this;

    case LOC_RUNTIME_RAM:
        {
            if(is() == IS_BANK)
                return locator_t::const_byte(0);
            span_t span = runtime_span(runtime_ram(), romv);
            for(unsigned i = 0; !span && i < NUM_ROMV; ++i)
                span = runtime_span(runtime_ram(), romv_t(i));
            return from_span(span);
        }

    case LOC_RUNTIME_ROM:
        {
            if(is() == IS_BANK)
                return locator_t::const_byte(0);
            span_t span = runtime_span(runtime_rom(), romv);
            for(unsigned i = 0; !span && i < NUM_ROMV; ++i)
                span = runtime_span(runtime_rom(), romv_t(i));
            return from_span(span);
        }

    case LOC_NMI_INDEX:
        return locator_t::const_byte(fn()->nmi_index() + 1);

    case LOC_LT_EXPR:
        // Check if the LT expression has been evaluated yet:
        lt_value_t& v = *lt();

        if(!v.resolved(romv))
        {
            v.resolve(romv);
            assert(v.resolved(romv));
        }

        unsigned index = atom();
        if(member())
        {
            assert(is_banked_ptr(lt().safe().type.name()));
            assert(member() == 1);
            index += 2;
        }
        else if(!byteified() && is_ptr(lt().safe().type.name()))
        {
            try
            {
                std::uint8_t lo = linked_to_rom(v.results[romv].bytes[index].link(romv));
                std::uint8_t hi = linked_to_rom(v.results[romv].bytes[index+1].link(romv));
                return locator_t::addr(lo + (hi << 8) + offset());
            }
            catch(...)
            {
                return *this;
            }
        }

        passert(index < v.results[romv].bytes.size(), int(index), v.results[romv].bytes.size());

        return v.results[romv].bytes[index].link(romv);
    };
}

rom_data_ht locator_t::rom_data() const
{
    switch(lclass())
    {
    default:
        return {};
    case LOC_FN:
        return fn()->rom_proc();
    case LOC_ROM_ARRAY:
        return rom_array();
    case LOC_RESET_PROC:
        return ::reset_proc;
    case LOC_MAIN_MODE:
        return get_main_mode().rom_proc();
    case LOC_GCONST:
    case LOC_DPCM:
        return const_()->rom_array();
    case LOC_RESET_GROUP_VARS:
        return group_vars()->init_proc();
    case LOC_NAMED_LABEL:
        {
            global_t const& g = *global();
            switch(g.gclass())
            {
            default: 
                return {};
            case GLOBAL_FN:
                return g.impl<fn_t>().rom_proc();
            case GLOBAL_CONST:
                return g.impl<const_t>().rom_array();
            }
        }
    case LOC_ASM_GOTO_MODE:
        return fn()->asm_goto_mode_rom_proc(data());
    };
}

rom_alloc_ht locator_t::rom_alloc(romv_t romv) const
{
    if(rom_data_ht d = rom_data())
        return d.get()->find_alloc(romv);
    return {};
}

std::uint16_t linked_to_rom(locator_t linked, bool ignore_errors)
{
    if(!is_const(linked.lclass()) || linked.is() == IS_BANK)
    {
        if(ignore_errors)
            return 0;
        else
            throw std::runtime_error(fmt("Unable to link locator %", linked));
    }
    assert(!linked.offset());

    std::uint16_t data = linked.data();

    if(linked.is() == IS_PTR_HI)
        data >>= 8;

    return data;
}

