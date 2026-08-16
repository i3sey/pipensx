#pragma once

#include <cstddef>
#include <string>

namespace pipensx {

// Read the UTF-8 name/author from a raw 0x4000 control.nacp.
// titles_data_format != 0: raw DEFLATE; the leading u16 is compressed size.
// preferredIndex is the libnx language slot (0 = AmericanEnglish).
bool nacpReadLanguage(const void* nacp, size_t size, int preferredIndex,
                      std::string& name, std::string& author);

} // namespace pipensx
