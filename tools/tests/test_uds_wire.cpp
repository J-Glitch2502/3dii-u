#include <cassert>
#include <array>
#include "azl/uds_types.hpp"
#include "azl/room_proto.hpp"

int main() {
    azl::ConnectionStatus status{};
    status.status = 6;
    status.status_change_reason = 0x12345678;
    status.network_node_id = 0x1234;
    status.changed_nodes = 0xabcd;
    status.nodes[0] = 0x2345;
    status.nodes[15] = 0x6789;
    status.total_nodes = 3;
    status.max_nodes = 3;
    status.node_bitmask = 7;
    std::array<azl::u8, 48> bytes{};
    status.Store(bytes.data());
    assert(bytes[0] == 6 && bytes[1] == 0);
    assert(bytes[4] == 0x78 && bytes[7] == 0x12);
    assert(bytes[8] == 0x34 && bytes[9] == 0x12);
    assert(bytes[10] == 0xcd && bytes[11] == 0xab);
    assert(bytes[12] == 0x45 && bytes[13] == 0x23);
    assert(bytes[42] == 0x89 && bytes[43] == 0x67);
    assert(bytes[44] == 3 && bytes[45] == 3 && bytes[46] == 7 && bytes[47] == 0);
    // Room packets use big-endian lengths, in contrast to the guest IPC buffer.
    auto join = azl::room::EncodeJoinRequest("A", "", {}, "", "");
    const std::array<azl::u8, 28> expected{
        1, 0,0,0,1, 'A', 0,0,0,0, 0,0,0,0,0,0, 0,0,0,4, 0,0,0,0, 0,0,0,0};
    // Exact wire fields through network-version and empty password/token.
    assert(join.size() == 28);
    for (std::size_t i = 0; i < join.size(); ++i) assert(join[i] == expected[i]);
}
