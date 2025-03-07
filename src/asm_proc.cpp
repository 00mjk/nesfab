#include "asm_proc.hpp"

#include "flat/small_set.hpp"

#include "format.hpp"
#include "globals.hpp"
#include "runtime.hpp"
#include "compiler_error.hpp"

bool is_return(asm_inst_t const& inst)
{
    return ((op_flags(inst.op) & ASMF_RETURN) 
            || ((op_flags(inst.op) & ASMF_JUMP) 
                && !(op_flags(inst.op) & ASMF_SWITCH)
                && !is_label(inst.arg.lclass())));
}

bool mem_inst(asm_inst_t const& inst)
{
    return (op_input_regs(inst.op) | op_output_regs(inst.op)) & REGF_M;
}

bool o_peephole(asm_inst_t* begin, asm_inst_t* end)
{
    bool changed = false;

    for_each_peephole(begin, end, [&](asm_inst_t& a, asm_inst_t& b, asm_inst_t* c)
    {
        auto const replace_op = [&](op_t op)
        {
            a.op = op;
            b.op = ASM_PRUNED;
            changed = true;
        };

        // Converts RMW operations to their illegal versions.
        // e.g.:
        //     DEC foo
        //     CMP foo
        // becomes:
        //     DCP foo 
        auto const peep_rmw = [&](op_name_t second, op_name_t replace)
        {
            if(b.op == get_op(second, op_addr_mode(a.op))
               && a.arg == b.arg && a.alt == b.alt)
            {
                if(op_t new_op = get_op(replace, op_addr_mode(a.op)))
                {
                    replace_op(new_op);
                    return true;
                }
            }

            return false;
        };

        // Converts load, increment, store, into a RMW operation.
        // e.g.:
        //     LDX foo
        //     INX
        //     STX foo
        // becomes:
        //     INC foo
        //     LDX foo
        auto const peep_inxy = [&](op_name_t second, op_name_t store, op_name_t replace)
        {
            if(c && op_name(b.op) == second && op_name(c->op) == store 
               && op_addr_mode(a.op) == op_addr_mode(c->op)
               && a.arg == c->arg && a.alt == c->alt
               && (!a.arg || is_var_like(a.arg.lclass()))
               && (!a.alt || is_var_like(a.alt.lclass())))
            {
                if(op_t new_op = get_op(replace, op_addr_mode(a.op)))
                {
                    c->op = a.op;
                    replace_op(new_op);
                    return true;
                }
            }

            return false;
        };

        // Converts load, load, into a transfer
        // e.g.:
        //     LDX foo
        //     LDA foo
        // becomes:
        //     LDX foo
        //     TXA
        auto const peep_transfer = [&](op_name_t second, op_t replace)
        {
            if(op_name(b.op) == second 
               && op_addr_mode(a.op) == op_addr_mode(b.op)
               && a.arg == b.arg
               && a.alt == b.alt
               && (!a.arg || is_var_like(a.arg.lclass())))
            {
                b.op = replace;
                b.arg = b.alt = {};
                changed = true;
                return true;
            }

            return false;
        };

        auto const peep_lax = [&](op_name_t second)
        {
            op_t replace = get_op(LAX, op_addr_mode(a.op));

            if(replace
               && op_name(b.op) == second 
               && op_addr_mode(a.op) == op_addr_mode(b.op)
               && a.arg == b.arg
               && a.alt == b.alt)
            {
                replace_op(replace);
                return true;
            }

            return false;
        };

        // Converts store, load, into a transfer
        // e.g.:
        //     STX foo
        //     LDA foo
        // becomes:
        //     STX foo
        //     TXA
        auto const peep_transfer2 = [&](op_name_t second, op_t replace)
        {
            if(op_name(b.op) == second 
               && (op_addr_mode(b.op) == MODE_ZERO_PAGE || op_addr_mode(b.op) == MODE_ABSOLUTE)
               && a.arg == b.arg
               && a.alt == b.alt
               && (!a.arg || is_var_like(a.arg.lclass())))
            {
                b.op = replace;
                b.arg = b.alt = {};
                changed = true;
                return true;
            }

            return false;

            /* TODO
            else if(c && op_name(c->op) == second 
               && (op_addr_mode(c->op) == MODE_ZERO_PAGE || op_addr_mode(c->op) == MODE_ABSOLUTE)
               && a.arg == c->arg
               && a.alt == c->alt
               && b.op != ASM_LABEL
               && !(op_output_regs(b.op) & op_input_regs(replace)))
            {
                c->op = replace;
                c->arg = c->alt = {};
                changed = true;
                return true;
            }

            return false;
            */
        };

    retry:

        // Prepare for ALR
        if(a.op == LDA_IMMEDIATE && op_name(b.op) == AND && !a.alt && !b.alt)
        {
            locator_t const imm = a.arg;

            a.op = get_op(LDA, op_addr_mode(b.op));
            a.arg = b.arg;
            b.op = AND_IMMEDIATE;
            b.arg = imm;
            changed = true;
        }

        switch(op_name(a.op))
        {
        default: break;
        case DEC: peep_rmw(CMP, DCP); break;
        case INC: peep_rmw(SBC, ISC); break;
        case ROL: peep_rmw(AND, RLA); break;
        case ROR: peep_rmw(ADC, RRA); break;
        case ASL: peep_rmw(ORA, SLO); break;
        case LSR: peep_rmw(EOR, SRE); break;
        case AND:
            if(a.op == AND_IMMEDIATE && b.op == LSR_IMPLIED)
                replace_op(ALR_IMMEDIATE);
            break;
        case LDX:
            if(peep_inxy(INX, STX, INC)) 
                goto retry;
            if(peep_inxy(DEX, STX, DEC)) 
                goto retry;
            if(peep_lax(LDA)) 
                goto retry;
            break;
        case LDY:
            if(peep_inxy(INY, STY, INC))
                goto retry;
            if(peep_inxy(DEY, STY, DEC))
                goto retry;
            if(peep_transfer(LDA, TYA_IMPLIED))
                goto retry;
            break;
        case LDA:
            if(peep_inxy(ASL, STA, ASL))
                goto retry;
            if(peep_inxy(LSR, STA, LSR))
                goto retry;
            if(peep_inxy(ROL, STA, ROL))
                goto retry;
            if(peep_inxy(ROR, STA, ROR))
                goto retry;
            if(peep_lax(LDX))
                goto retry;
            if(peep_transfer(LDY, TAY_IMPLIED))
                goto retry;
            break;
        case STA:
            if(peep_transfer2(LDX, TAX_IMPLIED))
                goto retry;
            if(peep_transfer2(LDY, TAY_IMPLIED))
                goto retry;
            break;
        case STX:
            if(peep_transfer2(LDA, TXA_IMPLIED))
                goto retry;
            break;
        case STY:
            if(peep_transfer2(LDA, TYA_IMPLIED))
                goto retry;
            break;

        case ALR:
            if(!a.alt && a.arg == locator_t::const_byte(1) && b.op == ROL_IMPLIED)
                replace_op(ANC_IMMEDIATE);
            break;

        case RTS:
        case JMP:
            // Code like:
            //   rts
            //   label:
            //   rts
            // Becomes:
            // label:
            //   rts

            if(c && c->op == a.op && b.op == ASM_LABEL && a.arg == c->arg && a.alt == c->alt)
            {
                a.op = ASM_PRUNED;
                a.arg = c->alt = {};
                changed = true;
            }
            break;
        }
    });

    return changed;
}


std::ostream& operator<<(std::ostream& o, asm_inst_t const& inst)
{
    o << "{ " << to_string(inst.op) << ", " << inst.arg;
    o << " hi: " << inst.alt;
    o << "   (" << inst.ssa_op << ") }";
#ifndef NDEBUG
    o << "   (" << (float(inst.cost) / 256.0f) << ") }";
#endif
    return o;
}

asm_proc_t::asm_proc_t(fn_ht fn, std::vector<asm_inst_t> code_, locator_t entry_label)
: fn(fn)
, entry_label(entry_label)
, code(std::move(code_))
{
    rebuild_label_map();
}

void asm_proc_t::rebuild_label_map()
{
    labels.clear();

    for(unsigned i = 0; i < code.size(); ++i)
    {
        if(code[i].op == ASM_LABEL)
        {
            auto result = labels.insert({ code[i].arg.mem_head(), { .index = i }});
            assert(result.second);
        }
    }

    assert(!entry_label || labels.count(entry_label));
}

void asm_proc_t::build_label_offsets()
{
    int offset = 0;
    for(asm_inst_t const& inst : code)
    {
        if(inst.op == ASM_LABEL)
            get_label(inst.arg).offset = offset;
        offset += op_size(inst.op);
    }
}

asm_inst_t* asm_proc_t::prev_inst(int i)
{
    return ::prev_inst(&*code.begin(), &*code.end(), i + code.data());
}

asm_inst_t* asm_proc_t::next_inst(int i) 
{
    return ::next_inst(&*code.begin(), &*code.end(), i + code.data());
}

int asm_proc_t::bytes_between(unsigned ai, unsigned bi) const
{
    if(bi < ai)
        return -bytes_between(bi, ai);

    int bytes = 0;
    for(unsigned i = ai; i < bi; ++i)
    {
        assert(i < code.size());
        bytes += op_size(code[i].op);
    }

    return bytes;
}

void asm_proc_t::push_inst(asm_inst_t inst)
{
    if(inst.op == ASM_LABEL)
    {
        auto result = labels.insert({ inst.arg.mem_head(), { .index = code.size() }});
        passert(result.second, inst);
    }

    code.push_back(inst);
}

void asm_proc_t::absolute_to_zp()
{
    for(asm_inst_t& inst : code)
    {
        // A hi-byte implies absolute.
        if(inst.alt && !inst.alt.eq_const(0))
            continue;

        if(inst.arg.is() != IS_DEREF && inst.arg.is() != IS_PTR)
            continue;

        if(inst.arg.lclass() == LOC_ADDR && inst.arg.data() >= 0x100)
            continue;

        // 'zp_only' *has* to go on the zero page:
        if(!inst.arg.mem_zp_only())
            continue;

        // OK! Replace with zp:

        switch(op_addr_mode(inst.op))
        {
        case MODE_ABSOLUTE:
            if(op_t new_op = get_op(op_name(inst.op), MODE_ZERO_PAGE))
                inst.op = new_op;
            break;

        // These are *generally* safe, but aren't if arrays can start in ZP but end outside of it.
        // TODO: Better specify this by possibly adding new pseudo asm ops.
        case MODE_ABSOLUTE_X:
            if(op_t new_op = get_op(op_name(inst.op), MODE_ZERO_PAGE_X))
                inst.op = new_op;
            break;
        case MODE_ABSOLUTE_Y: 
            if(op_t new_op = get_op(op_name(inst.op), MODE_ZERO_PAGE_Y))
                inst.op = new_op;
            break;

        default: 
            break;
        }
    }
}

void asm_proc_t::convert_long_branch_ops()
{
    // Loop until we can do no more work.
    bool progress; 
    do
    {
        progress = false;

        for(unsigned i = 0; i < code.size(); ++i)
        {
            asm_inst_t& inst = code[i];

            if(!is_branch(inst.op))
                continue;

            unsigned const label_i = get_label(inst.arg).index;
            int dist = bytes_between(i, label_i) - int(op_size(inst.op));

            if(is_relative_branch(inst.op))
            {
                // Change to long pseudo instruction when out of range
                if(dist > 127 || dist < -128)
                {
                    inst.op = get_op(op_name(inst.op), MODE_LONG);
                    progress = true;
                }
            }
            else if(is_long_branch(inst.op))
            {
                op_t const new_op = get_op(op_name(inst.op), MODE_RELATIVE);
                int const size_diff = int(op_size(inst.op)) - int(op_size(new_op));

                dist -= size_diff;

                // Change to short instruction when in range
                if(dist <= 127 && dist >= -128)
                {
                    inst.op = new_op;
                    progress = true;

                    passert(bytes_between(i, label_i) - int(op_size(inst.op)) <= 127, bytes_between(i, label_i) - int(op_size(inst.op)));
                    passert(bytes_between(i, label_i) - int(op_size(inst.op)) >= -128, bytes_between(i, label_i) - int(op_size(inst.op)));
                }
            }
        }
    }
    while(progress);
}

// Note: 'use_nops' can be dangerous if applied too early,
// as it hardcodes the relative jump distance.
void asm_proc_t::optimize_short_jumps(bool use_nops)
{
    for(unsigned i = 0; i < code.size(); ++i)
    {
        asm_inst_t& inst = code[i];

        asm_inst_t* next = next_inst(i);
        if(!next)
            continue;

        if(inst.op == JMP_ABSOLUTE)
        {
            unsigned const label_i = get_label(inst.arg).index;
            int const dist = bytes_between(next - code.data(), label_i);
            
            if(dist == 0)
            {
                // Prune unnecessary jumps
                inst.op = ASM_PRUNED;
                inst.arg = {};
            }
            else if(use_nops && dist == 1)
            {
                inst.op = SKB_IMPLIED;
                inst.arg = {};
            }
            else if(use_nops && dist == 2)
            {
                auto o = op_code(next->op);
                if(o < 0x20 || o >= 0x42) // Avoid reading PPU / APU registers, etc.
                {
                    inst.op = IGN_IMPLIED;
                    inst.arg = {};
                }
            }
        }
        else if(op_flags(inst.op) & ASMF_BRANCH)
        {
            // Prune unecessary branches

            unsigned const label_i = get_label(inst.arg).index;
            int const dist = bytes_between(next - code.data(), label_i);

            if(dist == 0)
            {
                inst.op = ASM_PRUNED;
                inst.arg = {};
            }
            else if(dist == 2 && next->op == invert_branch(inst.op))
            {
                // Handles code like:
                //  BEQ l1
                //  BNE l2
                //  L1:
                // (Removes the first BEQ)

                if(next->arg == code[i].arg)
                {
                    // Prune both
                    code[i].op = next->op = ASM_PRUNED;
                    code[i].arg = next->arg = {};
                }
                else
                {
                    // Prune the useless branch op
                    code[i] = *next;
                    next->op = ASM_PRUNED;
                    next->arg = {};
                }
            }
        }
    }
}

void asm_proc_t::optimize(bool initial)
{
    // Order matters here.
    o_peephole(&*code.begin(), &*code.end());
    absolute_to_zp();
    optimize_short_jumps(!initial);
    convert_long_branch_ops();
}

void asm_proc_t::initial_optimize()
{
    // Order matters here.
    optimize(true);
}

void asm_proc_t::link(romv_t romv, int bank)
{
#ifndef NDEBUG
    std::size_t const pre_size = size();
#endif

    for(asm_inst_t& inst : code)
    {
        inst.arg = inst.arg.link(romv, fn, bank);
        inst.alt = inst.alt.link(romv, fn, bank);
    }

    if(!fn || !fn->iasm)
        optimize(false);
    assert(pre_size >= size());
}

void asm_proc_t::write_assembly(std::ostream& os, romv_t romv) const
{
    if(fn)
        os << fn->global.name << ":\n";

    for(unsigned i = 0; i < code.size(); ++i)
    {
        asm_inst_t const& inst = code[i];

        for(auto const& pair : labels)
            if(pair.second.index == i)
                os << "LABEL " << pair.first << ":\n";

        if(inst.op == ASM_PRUNED || inst.op == ASM_LABEL)
            continue;

        os << "-   " << to_string(inst.op) << ' ';

        switch(inst.arg.lclass())
        {
        case LOC_CONST_BYTE:
            os << "#" << inst.arg.data() << "   " << inst.arg;
            break;
        case LOC_GMEMBER:
            os << "gmember " << inst.arg.gmember()->gvar.global.name << ' ' << inst.arg.gmember()->member() 
               << " " << inst.arg.gmember()->span(inst.arg.atom()) << "   " << inst.arg;
            break;
        default:
            os << inst.arg << ' ';
            os << inst.alt << ' ';

            fn_ht lfn = fn;
            if(has_fn(inst.arg.lclass()) && inst.arg.fn())
                lfn = inst.arg.fn();

            if(lfn)
            {
                int const index = lfn->lvars().index(inst.arg);
                if(index >= 0)
                    os << " lvar " << lfn->lvar_span(romv, index);
            }

            break;
        }

        os << '\n';
    }
}

static std::pair<locator_t, locator_t> absolute_locs(asm_inst_t const& inst)
{
    locator_t lo = inst.arg;
    locator_t hi = inst.alt;

    if(!hi)
    {
        if(lo.is() == IS_PTR || lo.is() == IS_DEREF)
            hi = lo.with_is(IS_PTR_HI);
        else
            hi = locator_t::const_byte(0);
    }

    return std::make_pair(lo, hi);
}

template<typename Fn>
void asm_proc_t::for_each_inst(Fn const& fn) const
{
    for(asm_inst_t const& inst : code)
    {
        if(op_size(inst.op) == 0)
            continue;

        switch(inst.op)
        {
        case STORE_C_ABSOLUTE:
            fn(asm_inst_t{ .op = PHP_IMPLIED });
            fn(asm_inst_t{ .op = PHA_IMPLIED });
            fn(asm_inst_t{ .op = LDA_IMMEDIATE, .arg = locator_t::const_byte(0) });
            fn(asm_inst_t{ .op = ROL_IMPLIED });
            fn(asm_inst_t{ .op = STA_ABSOLUTE, .arg = inst.arg });
            fn(asm_inst_t{ .op = PLA_IMPLIED });
            fn(asm_inst_t{ .op = PLP_IMPLIED });
            // total bytes: 1+1+2+1+3+1+1 = 10
            break;

        case STORE_Z_ABSOLUTE:
            fn(asm_inst_t{ .op = PHP_IMPLIED });
            fn(asm_inst_t{ .op = PHA_IMPLIED });
            fn(asm_inst_t{ .op = PHP_IMPLIED });
            fn(asm_inst_t{ .op = PLA_IMPLIED });
            fn(asm_inst_t{ .op = ALR_IMMEDIATE, .arg = locator_t::const_byte(0b10) });
            fn(asm_inst_t{ .op = STA_ABSOLUTE, .arg = inst.arg });
            fn(asm_inst_t{ .op = PLA_IMPLIED });
            fn(asm_inst_t{ .op = PLP_IMPLIED });
            // total bytes: 1+1+1+1+2+3+1+1 = 11
            break;

        case STORE_N_ABSOLUTE:
            fn(asm_inst_t{ .op = PHP_IMPLIED });
            fn(asm_inst_t{ .op = PHA_IMPLIED });
            fn(asm_inst_t{ .op = PHP_IMPLIED });
            fn(asm_inst_t{ .op = PLA_IMPLIED });
            fn(asm_inst_t{ .op = ANC_IMMEDIATE, .arg = locator_t::const_byte(0x80) });
            fn(asm_inst_t{ .op = ROL_IMPLIED });
            fn(asm_inst_t{ .op = STA_ABSOLUTE, .arg = inst.arg });
            fn(asm_inst_t{ .op = PLA_IMPLIED });
            fn(asm_inst_t{ .op = PLP_IMPLIED });
            // total bytes: 1+1+1+1+2+1+3+1+1 = 12
            break;

        case BANKED_Y_JSR:
        case BANKED_Y_JMP:
            {
                assert(!inst.alt);
                auto locs = absolute_locs(inst);

                fn(asm_inst_t{ .op = LDA_IMMEDIATE, .arg = locs.first });
                fn(asm_inst_t{ .op = LDX_IMMEDIATE, .arg = locs.second });
                if(inst.op == BANKED_Y_JSR)
                    fn(asm_inst_t{ .op = JSR_ABSOLUTE, .arg = locator_t::runtime_rom(RTROM_jsr_y_trampoline) });
                else 
                {
                    assert(inst.op == BANKED_Y_JMP);
                    fn(asm_inst_t{ .op = JMP_ABSOLUTE, .arg = locator_t::runtime_rom(RTROM_jmp_y_trampoline) });
                }
            }
            break;

        case ASM_X_SWITCH:
            fn(asm_inst_t{ .op = LDA_ABSOLUTE_X, .arg = inst.alt.with_is(IS_DEREF) });
            fn(asm_inst_t{ .op = PHA_IMPLIED });
            fn(asm_inst_t{ .op = LDA_ABSOLUTE_X, .arg = inst.arg.with_is(IS_DEREF) });
            fn(asm_inst_t{ .op = PHA_IMPLIED });
            fn(asm_inst_t{ .op = RTS_IMPLIED });
            break;

        case ASM_Y_SWITCH:
            fn(asm_inst_t{ .op = LDA_ABSOLUTE_Y, .arg = inst.alt.with_is(IS_DEREF) });
            fn(asm_inst_t{ .op = PHA_IMPLIED });
            fn(asm_inst_t{ .op = LDA_ABSOLUTE_Y, .arg = inst.arg.with_is(IS_DEREF) });
            fn(asm_inst_t{ .op = PHA_IMPLIED });
            fn(asm_inst_t{ .op = RTS_IMPLIED });
            break;

        default:
            fn(inst);
            break;
        }
    }
}

template<typename Fn>
void asm_proc_t::for_each_locator(Fn const& fn) const
{
    for_each_inst([&](asm_inst_t const& inst)
    {
        if(inst.op == ASM_DATA)
        {
            fn(inst.arg);
            return;
        }

        passert(!(op_flags(inst.op) & ASMF_FAKE), to_string(inst.op), inst.arg);
        locator_t const op = locator_t::const_byte(op_code(inst.op));

        switch(op_addr_mode(inst.op))
        {
        case MODE_IMPLIED:
            fn(op);
            break;

        case MODE_IMMEDIATE:
        case MODE_RELATIVE:
        case MODE_ZERO_PAGE:
        case MODE_ZERO_PAGE_X:
        case MODE_ZERO_PAGE_Y:
        case MODE_INDIRECT_X:
        case MODE_INDIRECT_Y:
            fn(op);
            fn(inst.arg);
            break;

        case MODE_LONG:
            {
                locator_t const inverted_op = locator_t::const_byte(
                    op_code(get_op(invert_branch(op_name(inst.op)), MODE_RELATIVE)));
                fn(inverted_op);
                fn(locator_t::const_byte(3)); // Branch over upcoming jmp
                fn(locator_t::const_byte(op_code(JMP_ABSOLUTE)));
            }
            goto absolute_addr;

        case MODE_ABSOLUTE:
        case MODE_ABSOLUTE_X:
        case MODE_ABSOLUTE_Y:
        case MODE_INDIRECT:
            {
                fn(op);
            absolute_addr:
                auto locs = absolute_locs(inst);
                passert(locs.first && locs.second, inst);
                fn(locs.first);
                fn(locs.second);
            }
            break;

        default:
            throw std::runtime_error("Invalid addressing mode.");
        }
    });
}

loc_vec_t asm_proc_t::loc_vec() const
{
    loc_vec_t ret;
    ret.reserve(size());
    for_each_locator([&](locator_t loc){ ret.push_back(loc); });
    return ret;
}

void asm_proc_t::write_bytes(std::uint8_t* const start, romv_t romv, int bank) const
{
    std::uint8_t* at = start;

    auto const from_locator = [&](locator_t loc) -> std::uint8_t
    {
        loc = loc.link(romv, fn, bank);
        if(!is_const(loc.lclass()))
            throw std::runtime_error(fmt("Unable to link %", loc));
        assert(loc.offset() == 0);

        if(loc.is() == IS_PTR_HI)
            return loc.data() >> 8;
        return loc.data();
    };

    for_each_locator([&](locator_t loc){ *at++ = from_locator(loc); });
}

void asm_proc_t::relocate(locator_t from)
{
    std::uint16_t addr = linked_to_rom(from, true);

    for(unsigned i = 0; i < code.size(); ++i)
    {
        asm_inst_t& inst = code[i];

        auto const relocate1 = [&](locator_t loc)
        {
            int dist;
            unsigned label_i;

            if(op_addr_mode(inst.op) == MODE_RELATIVE && loc.lclass() == LOC_ADDR)
            {
                if(!is_const(from.lclass()))
                   throw std::runtime_error(fmt("Unable to relocate %", from));
                dist = linked_to_rom(loc) - addr - int(op_size(inst.op));
                goto have_dist;
            }

            if(!is_label(loc.lclass()))
                return loc;

            if(loc.is() == IS_BANK)
                return loc;

            {
                auto* mapped = labels.mapped(loc.mem_head());
                if(!mapped)
                    return loc;

                label_i = mapped->index;
            }

            if(op_addr_mode(inst.op) == MODE_RELATIVE)
            {
                dist = bytes_between(i, label_i) - int(op_size(inst.op)) + static_cast<std::int16_t>(loc.offset());
            have_dist:

                if(dist > 127 || dist < -128)
                {
                    std::string what = fmt("Unable to relocate branch instruction %. Destination outside valid range. (%)", 
                                           op_name(inst.op), dist);
                    if(fn)
                    {
                        pstring_t pstring = {};

                        if(inst.iasm_child >= 0)
                        {
                            assert(std::size_t(inst.iasm_child) < pstrings.size());
                            pstring = pstrings[inst.iasm_child];
                        }

                        if(!pstring)
                            pstring = fn->global.pstring();

                        compiler_error(pstring, std::move(what));
                    }
                    throw relocate_error_t(std::move(what));
                }
                return locator_t::const_byte(dist);
            }
            else
                return from.with_advance_offset(loc.offset() + bytes_between(0, label_i)).with_is(loc.is());
        };

        inst.arg = relocate1(inst.arg);
        inst.alt = relocate1(inst.alt);
        addr += op_size(inst.op);
    }
}

unsigned asm_proc_t::add_pstring(pstring_t pstring)
{
    unsigned const ret = pstrings.size();
    pstrings.push_back(pstring);
    return ret;
}

void asm_proc_t::append(asm_proc_t const& proc)
{
    for(asm_inst_t inst : proc.code)
    {
        if(inst.iasm_child >= 0)
        {
            assert(std::size_t(inst.iasm_child) < proc.pstrings.size());
            inst.iasm_child = add_pstring(proc.pstrings[inst.iasm_child]);
        }

        push_inst(inst);
    }
}

unsigned asm_proc_t::next_label_id() const
{
    unsigned next_id = 0;

    for(auto const& inst : code)
        if(inst.op == ASM_LABEL && inst.arg.lclass() == LOC_MINOR_LABEL)
            next_id = std::max<unsigned>(next_id, inst.arg.data() + 1);

    return next_id;
}

