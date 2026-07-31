#pragma once

#include <filesystem>

namespace research
{

// Revalidates a published capture-transaction-v1 package at its declared
// accepted path. This is exposed for capture-package-v2 composition; it does
// not issue or alter the v1 validation receipt.
void validateCaptureV1PackageReadOnly(const std::filesystem::path& package);

} // namespace research
