#include "mapper.hpp"

#include <stdexcept>
#include <cstring>
#include <charconv>

#include "builtin.hpp"
#include "format.hpp"

mapper_mirroring_t mapper_params_t::mirroring_none(mapper_type_t mt) const
{
    if(mirroring == MIRROR_NONE)
        return MIRROR_NONE;
    throw std::runtime_error(fmt("Unsupported % mirroring.", mapper_name(mt)));
}

mapper_mirroring_t mapper_params_t::mirroring_HV(mapper_type_t mt) const
{
    if(mirroring == MIRROR_NONE)
        return MIRROR_V;
    else if(mirroring != MIRROR_H && mirroring != MIRROR_V)
        throw std::runtime_error(fmt("Unsupported % mirroring. Expecting H or V.", mapper_name(mt)));
    return mirroring;
}

mapper_mirroring_t mapper_params_t::mirroring_4(mapper_type_t mt) const
{
    if(mirroring == MIRROR_NONE || mirroring == MIRROR_4)
        return MIRROR_4;
    throw std::runtime_error(fmt("Unsupported % mirroring. Expecting 4.", mapper_name(mt)));
}

unsigned mapper_params_t::num_32k_banks(mapper_type_t mt, unsigned min, unsigned max, unsigned default_) const
{
    if(!prg_size)
        return default_;
    if(prg_size < min)
        throw std::runtime_error(fmt("Invalid % PRG size: %. Minimum accepted: %.", mapper_name(mt), prg_size, min));
    if(prg_size > max)
        throw std::runtime_error(fmt("Invalid % PRG size: %. Maximum accepted: %.", mapper_name(mt), prg_size, max));
    if((prg_size % 32) != 0)
        throw std::runtime_error(fmt("Invalid % PRG size: %. Expecting a multiple of 32.", mapper_name(mt), prg_size));
    return prg_size / 32;
}

unsigned mapper_params_t::num_8k_chr(mapper_type_t mt, unsigned min, unsigned max, unsigned default_) const
{
    if(!chr_size)
        return default_;
    if(chr_size < min)
        throw std::runtime_error(fmt("Invalid % CHR size: %. Minimum accepted: %.", mapper_name(mt), prg_size, min));
    if(chr_size > max)
        throw std::runtime_error(fmt("Invalid % CHR size: %. Maximum accepted: %.", mapper_name(mt), prg_size, max));
    if((chr_size % 8) != 0)
        throw std::runtime_error(fmt("Invalid % CHR size: %. Expecting a multiple of 8.", mapper_name(mt), chr_size));
    return chr_size / 8;
}

mapper_t mapper_t::nrom(mapper_params_t const& params)
{
    constexpr mapper_type_t mt = MAPPER_NROM;
    return 
    {
        .type = mt,
        .mirroring = params.mirroring_HV(mt),
        .num_32k_banks = params.num_32k_banks(mt, 32, 32, 1),
        .num_8k_chr_rom = params.num_8k_chr(mt, 8, 8, 1),
    };
}

mapper_t mapper_t::cnrom(mapper_params_t const& params)
{
    constexpr mapper_type_t mt = MAPPER_CNROM;
    return 
    {
        .type = mt,
        .mirroring = params.mirroring_HV(mt),
        .num_32k_banks = params.num_32k_banks(mt, 32, 32, 1),
        .num_8k_chr_rom = params.num_8k_chr(mt, 8, 2048, 4),
    };
}

mapper_t mapper_t::anrom(mapper_params_t const& params)
{
    constexpr mapper_type_t mt = MAPPER_ANROM;
    return 
    {
        .type = mt,
        .mirroring = params.mirroring_none(mt),
        .num_32k_banks = params.num_32k_banks(mt, 32, 512, 8),
        .num_8k_chr_ram = params.num_8k_chr(mt, 8, 8, 1),
    };
}

mapper_t mapper_t::bnrom(mapper_params_t const& params)
{
    constexpr mapper_type_t mt = MAPPER_BNROM;
    return 
    {
        .type = mt,
        .mirroring = params.mirroring_HV(mt),
        .num_32k_banks = params.num_32k_banks(mt, 32, 2048, 4),
        .num_8k_chr_ram = params.num_8k_chr(mt, 8, 8, 1),
    };
}

mapper_t mapper_t::gnrom(mapper_params_t const& params)
{
    constexpr mapper_type_t mt = MAPPER_GNROM;
    return 
    {
        .type = mt,
        .mirroring = params.mirroring_HV(mt),
        .num_32k_banks = params.num_32k_banks(mt, 32, 512, 4),
        .num_8k_chr_rom = params.num_8k_chr(mt, 8, 128, 4),
    };
}

mapper_t mapper_t::gtrom(mapper_params_t const& params)
{
    constexpr mapper_type_t mt = MAPPER_GTROM;
    return 
    {
        .type = mt,
        .mirroring = params.mirroring_4(mt),
        .num_32k_banks = params.num_32k_banks(mt, 32, 512, 16),
        .num_8k_chr_ram = params.num_8k_chr(mt, 8, 16, 2),
    };
}

void write_ines_header(std::uint8_t* at, mapper_t const& mapper)
{
    // https://www.nesdev.org/wiki/NES_2.0

    // 0-3
    char const magic_header[4] = { 0x4E, 0x45, 0x53, 0x1A };
    std::memcpy(at, magic_header, 4);

    // 4
    at[4] = std::uint8_t(mapper.num_16k_banks()); // Banks in 16k units, low byte.

    // 5
    at[5] = std::uint8_t(mapper.num_8k_chr_rom); // Banks in 16k units.

    // 6
    std::uint8_t flags6 = 0;
    flags6 |= unsigned(mapper.type) << 4;
    switch(mapper.mirroring)
    {
    default: break;
    case MIRROR_V: flags6 |= 1 << 0; break;
    case MIRROR_4: flags6 |= 1 << 3; break;
    }
    at[6] = flags6;

    // 7
    std::uint8_t flags7 = 0b00001000; // NES 2.0 format
    flags7 |= unsigned(mapper.type) & 0b11110000;
    at[7] = flags7;

    // 8
    std::uint8_t flags8 = 0;
    flags8 |= (unsigned(mapper.type) >> 8) & 0b1111;
    at[8] = flags8;

    // 9
    if(((mapper.num_16k_banks()) >> 8) > 0b1111)
        throw std::runtime_error("Too many ROM banks.");

    if(((mapper.num_8k_chr_rom) >> 8) > 0b1111)
        throw std::runtime_error("Too many CHR RAM banks.");

    std::uint8_t hi = 0;
    hi |= ((mapper.num_16k_banks()) >> 8) & 0b1111;
    hi |= ((mapper.num_8k_chr_rom) >> 4) & 0b11110000;
    at[9] = hi;

    // 10
    at[10] = 0;

    // 11
    unsigned const chr_ram_chunks = mapper.num_8k_chr_ram * 0x2000 / 64;
    if(chr_ram_chunks && builtin::popcount(chr_ram_chunks) != 1)
        throw std::runtime_error("Invalid CHR RAM size.");
    unsigned const chr_shift = chr_ram_chunks ? builtin::rclz(chr_ram_chunks)-1 : 0;
    assert(!chr_ram_chunks || 64 << chr_shift == mapper.num_8k_chr_ram * 0x2000);

    if(chr_shift > 0b1111)
        throw std::runtime_error("CHR RAM is too large.");

    at[11] = chr_shift & 0b1111;

    // 12
    at[12] = 0;

    // 13
    at[13] = 0;

    // 14
    at[14] = 0;

    // 15
    at[15] = 0;
}

std::string_view mapper_name(mapper_type_t mt)
{
    using namespace std::literals;

    switch(mt)
    {
    default: return "unknown mapper"sv;
#define MAPPER(name, value) case MAPPER_##name: return #name ""sv;
    MAPPER_XENUM
#undef MAPPER
    }
}
