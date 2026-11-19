#include "SecureStore.h"

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>

#include <cstring>

namespace {
constexpr wchar_t kKey[] = L"Software\\PS5DirectView";
// Fixed secondary entropy: binds blobs to this application's purpose.
constexpr char kEntropy[] = "PS5DirectView/RemotePlay/v1";

struct LocalBlob {
    DATA_BLOB blob{};
    ~LocalBlob() {
        if (blob.pbData) {
            SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
        }
    }
};
} // namespace

namespace secure_store {

bool Save(const wchar_t* name, const std::vector<std::uint8_t>& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    LocalBlob out;
    if (!CryptProtectData(&in, L"PS5 DirectView pairing", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out.blob))
        return false;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_BINARY, out.blob.pbData,
                           out.blob.cbData) == ERROR_SUCCESS;
}

std::optional<std::vector<std::uint8_t>> Load(const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_BINARY, nullptr, nullptr,
                     &size) != ERROR_SUCCESS || size == 0 || size > 64 * 1024)
        return std::nullopt;
    std::vector<std::uint8_t> encrypted(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_BINARY, nullptr,
                     encrypted.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    DATA_BLOB in{size, encrypted.data()};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    LocalBlob out;
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out.blob))
        return std::nullopt;
    return std::vector<std::uint8_t>(out.blob.pbData, out.blob.pbData + out.blob.cbData);
}

void Erase(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

} // namespace secure_store

#endif // _WIN32
