#ifndef AFIS_PARSER_H
#define AFIS_PARSER_H

#include <istream>
#include <string>

#include "ir.h"

namespace afis {

ParseResult ParseIR(std::istream& input);
ParseResult ParseIRFile(const std::string& path);

}  // namespace afis

#endif