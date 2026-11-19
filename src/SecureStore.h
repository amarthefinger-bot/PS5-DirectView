#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// Small secrets (Remote Play pairing keys) encrypted per-user on each platform.
// Windows: DPAPI-NG + HKCU\Software\PS5DirectView.
// macOS:   Keychain generic password items under service "dev.ps5directview".
namespace secure_store {
bool Save(const wchar_t* name, const std::vector<std::uint8_t>& plain);
std::optional<std::vector<std::uint8_t>> Load(const wchar_t* name);
void Erase(const wchar_t* name);

#ifndef _WIN32
// UTF-8 variants used by the macOS code paths (wchar_t on Darwin is 32-bit).
bool Save(const char* name, const std::vector<std::uint8_t>& plain);
std::optional<std::vector<std::uint8_t>> Load(const char* name);
void Erase(const char* name);
#endif
}
