#define main broker_daemon_main
#include "../../Broker/RingBroker.cpp"
#undef main
int main(int argc,char **argv) {
    if(argc!=1) return 2;
    audit_token_t token{};mach_msg_type_number_t count=TASK_AUDIT_TOKEN_COUNT;
    if(task_info(mach_task_self(),TASK_AUDIT_TOKEN,reinterpret_cast<task_info_t>(&token),&count)!=KERN_SUCCESS) return 3;
    if(!matches(token,argv[0])) { std::puts("FAIL own signature not matched");return 1; }
    if(matches(token,"/Applications/Patchlane.app")||matches(token,DriverHost)) { std::puts("FAIL unauthorized identity accepted");return 1; }
    auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(argv[0]),std::strlen(argv[0]),false);
    SecStaticCodeRef code=nullptr;SecRequirementRef requirement=nullptr;CFStringRef text=nullptr;
    if(SecStaticCodeCreateWithPath(url,kSecCSDefaultFlags,&code)!=errSecSuccess) return 4;
    if(SecCodeCopyDesignatedRequirement(code,kSecCSDefaultFlags,&requirement)!=errSecSuccess) return 5;
    if(SecRequirementCopyString(requirement,kSecCSDefaultFlags,&text)!=errSecSuccess) return 6;
    const bool pinned=lcshared::matchesRequirement(token,text);
    CFRelease(text);CFRelease(requirement);CFRelease(code);CFRelease(url);
    if(!pinned||lcshared::matchesRequirement(token,CFSTR("identifier \"not.this.executable\""))) return 7;
    std::puts("PASS identity: own audit-token signature accepted; app/HAL impersonation rejected");return 0;
}
