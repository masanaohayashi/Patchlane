#pragma once
#include <Security/Security.h>
#include <mach/mach.h>
#include <cstring>
namespace lcshared {
inline bool matchesRequirement(audit_token_t token,CFStringRef text) {
    SecRequirementRef requirement=nullptr;
    if(SecRequirementCreateWithString(text,kSecCSDefaultFlags,&requirement)!=errSecSuccess) return false;
    auto data=CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(&token),sizeof(token));
    const void *key=kSecGuestAttributeAudit,*value=data;
    auto attributes=CFDictionaryCreate(nullptr,&key,&value,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    SecCodeRef guest=nullptr;
    auto status=SecCodeCopyGuestWithAttributes(nullptr,attributes,kSecCSDefaultFlags,&guest);
    if(status==errSecSuccess) status=SecCodeCheckValidity(guest,kSecCSStrictValidate,requirement);
    if(guest) CFRelease(guest);
    CFRelease(attributes);CFRelease(data);CFRelease(requirement);
    return status==errSecSuccess;
}
inline bool matchesCode(audit_token_t token,const char *path) {
    auto data=CFDataCreate(nullptr,reinterpret_cast<const UInt8*>(&token),sizeof(token));
    const void *key=kSecGuestAttributeAudit,*value=data;
    auto attrs=CFDictionaryCreate(nullptr,&key,&value,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    SecCodeRef guest=nullptr;auto result=SecCodeCopyGuestWithAttributes(nullptr,attrs,kSecCSDefaultFlags,&guest);
    CFRelease(attrs);CFRelease(data);
    if(result!=errSecSuccess) return false;
    auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(path),std::strlen(path),false);
    SecStaticCodeRef reference=nullptr;SecRequirementRef requirement=nullptr;
    result=SecStaticCodeCreateWithPath(url,kSecCSDefaultFlags,&reference);CFRelease(url);
    if(result==errSecSuccess) result=SecCodeCopyDesignatedRequirement(reference,kSecCSDefaultFlags,&requirement);
    if(result==errSecSuccess) result=SecCodeCheckValidity(guest,kSecCSStrictValidate,requirement);
    if(requirement) CFRelease(requirement);if(reference) CFRelease(reference);CFRelease(guest);
    return result==errSecSuccess;
}
}
