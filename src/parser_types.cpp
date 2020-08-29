#include "parser_types.hpp"

#include "format.hpp"

std::string token_t::to_string(char const* source) const
{
    return fmt("{ %, %, % }", token_name(type), value, pstring.view(source));
}

