#pragma once

#include <cstdint>
#include <vector>

namespace research_test
{

void clearGuestRam();
bool writeGuestRam(std::uint32_t address, const std::vector<std::uint8_t>& bytes);

} // namespace research_test
