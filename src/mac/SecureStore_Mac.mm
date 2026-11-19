#include "../SecureStore.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace {

NSString* kService = @"dev.ps5directview";

NSString* Utf8Name(const char* name) {
    return [NSString stringWithUTF8String:(name ? name : "")];
}

NSString* WideName(const wchar_t* name) {
    if (!name) return @"";
    // wchar_t on macOS is UCS-4.
    size_t length = 0;
    while (name[length]) ++length;
    return [[NSString alloc] initWithBytes:name
                                    length:length * sizeof(wchar_t)
                                  encoding:NSUTF32LittleEndianStringEncoding]
        ?: @"";
}

NSDictionary* Query(NSString* account) {
    return @{
        (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService: kService,
        (__bridge id)kSecAttrAccount: account,
    };
}

bool SaveData(NSString* account, const std::vector<std::uint8_t>& plain) {
    if (!account.length) return false;
    SecItemDelete((__bridge CFDictionaryRef)Query(account));
    if (plain.empty()) return true;
    NSData* data = [NSData dataWithBytes:plain.data() length:plain.size()];
    NSMutableDictionary* attrs = [Query(account) mutableCopy];
    attrs[(__bridge id)kSecValueData] = data;
    attrs[(__bridge id)kSecAttrAccessible] =
        (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    OSStatus status = SecItemAdd((__bridge CFDictionaryRef)attrs, NULL);
    return status == errSecSuccess;
}

std::optional<std::vector<std::uint8_t>> LoadData(NSString* account) {
    if (!account.length) return std::nullopt;
    NSMutableDictionary* q = [Query(account) mutableCopy];
    q[(__bridge id)kSecReturnData] = @YES;
    q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)q, &result);
    if (status != errSecSuccess || !result) return std::nullopt;
    NSData* data = (__bridge_transfer NSData*)result;
    std::vector<std::uint8_t> out(data.length);
    std::memcpy(out.data(), data.bytes, data.length);
    return out;
}

void EraseData(NSString* account) {
    if (!account.length) return;
    SecItemDelete((__bridge CFDictionaryRef)Query(account));
}

} // namespace

namespace secure_store {

bool Save(const wchar_t* name, const std::vector<std::uint8_t>& plain) {
    return SaveData(WideName(name), plain);
}

std::optional<std::vector<std::uint8_t>> Load(const wchar_t* name) {
    return LoadData(WideName(name));
}

void Erase(const wchar_t* name) {
    EraseData(WideName(name));
}

bool Save(const char* name, const std::vector<std::uint8_t>& plain) {
    return SaveData(Utf8Name(name), plain);
}

std::optional<std::vector<std::uint8_t>> Load(const char* name) {
    return LoadData(Utf8Name(name));
}

void Erase(const char* name) {
    EraseData(Utf8Name(name));
}

} // namespace secure_store
