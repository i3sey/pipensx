#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "install/ticket_rights.hpp"

using pipensx::install::kTicketMinSize;
using pipensx::install::kTicketRightsIdOffset;
using pipensx::install::parseTicketRightsId;
using pipensx::install::rightsIdEqual;

int main() {
    std::vector<uint8_t> ticket(kTicketMinSize, 0);
    for (size_t i = 0; i < 16; ++i)
        ticket[kTicketRightsIdOffset + i] = static_cast<uint8_t>(0xA0 + i);

    uint8_t rightsId[16] {};
    assert(parseTicketRightsId(ticket.data(), ticket.size(), rightsId));
    for (size_t i = 0; i < 16; ++i)
        assert(rightsId[i] == static_cast<uint8_t>(0xA0 + i));

    uint8_t same[16] {};
    std::memcpy(same, rightsId, sizeof(same));
    assert(rightsIdEqual(rightsId, same));

    uint8_t different[16] {};
    different[0] = 0xFF;
    assert(!rightsIdEqual(rightsId, different));

    assert(!parseTicketRightsId(ticket.data(), kTicketMinSize - 1, rightsId));
    assert(!parseTicketRightsId(nullptr, ticket.size(), rightsId));
    assert(!parseTicketRightsId(ticket.data(), ticket.size(), nullptr));

    return 0;
}
