#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// Small secrets (Remote Play pairing keys) encrypted with Windows DPAPI for
// the current user and stored in HKCU\Software\PS5DirectView. Only this
// Windows account on this PC can decrypt them.
namespace secure_store {
bool Save(const wchar_t* name, const std::vector<std::uint8_t>& plain);
std::optional<std::vector<std::uint8_t>> Load(const wchar_t* name);
void Erase(const wchar_t* name);
}
