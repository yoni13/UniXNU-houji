// SPDX-License-Identifier: BSD-2-Clause-Patent
// Policy test against the generated headers, not an SMMU hardware emulator.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "GiuliaSmmuTakeover.h"

int main()
{
    std::array<uint32_t, 0xd0000 / 4> registers{};
    registers[0x20 / 4] = 0x40000080;
    registers[0x24 / 4] = 0x60000046;
    registers[0x28 / 4] = 0x5111;
    // Firmware owns a translating context for exact UFS SID 0x60.
    registers[0x800 / 4] = 0x80000060;
    registers[0xc00 / 4] = 1;
    registers[(128 + 1) * 4096 / 4] = 1;
    auto read = [&](unsigned offset) { return registers.at(offset / 4); };
    auto plan = planGiuliaSmmu(0x60, 0x8cb5e9000ULL, 0x22000, read);
    assert(!plan.target.error && plan.retarget && plan.target.bank != 1);
    unsigned writes = 0;
    auto write = [&](unsigned offset, uint32_t value) {
        ++writes;
        assert(offset != (128 + 1) * 4096); // Never disable firmware's context.
        registers.at(offset / 4) = value;
    };
    assert(configureGiuliaSmmu(plan, 0x60, false, read, write));
    assert(writes == 0);
    registers[0xc00 / 4] = 2; // Concurrent ownership change must be refused.
    assert(configureGiuliaSmmu(plan, 0x60, true, read, write));
    assert(writes == 0);
    registers[0xc00 / 4] = 1;
    assert(!configureGiuliaSmmu(plan, 0x60, true, read, write));
    assert(writes > 0 && registers[(128 + 1) * 4096 / 4] == 1);
    assert((registers[0xc00 / 4] & 255) == plan.target.bank);
    puts("PASS: houji exact-SID retarget; old context preserved; busy queue and changed ownership refused");
}
