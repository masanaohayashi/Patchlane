#include "../Shared/BrokerProtocol.hpp"
#include "../Shared/CodeIdentity.hpp"
#include "../Shared/MappedAudioRing.hpp"
#include <Security/Security.h>
#include <servers/bootstrap.h>
#include <bsm/libbsm.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {
using namespace lcshared;
const char *DriverHost="/System/Library/Frameworks/CoreAudio.framework/Versions/A/XPCServices/com.apple.audio.Core-Audio-Driver-Service.helper.xpc";
// Check the kernel-supplied audit token against the installed binary's designated
// requirement. Client-supplied PID, path or role strings are never identities.
bool matches(audit_token_t token,const char *path) { return matchesCode(token,path); }

struct Published {
    mach_port_t port=MACH_PORT_NULL;uint64_t bytes=0;audit_token_t owner{};
    ~Published() { clear(); }
    void clear() { if(port) mach_port_deallocate(mach_task_self(),port);port=MACH_PORT_NULL;bytes=0; }
    bool live() const { return port&&matches(owner,DriverHost); }
};
Published &publicationFor(Published (&slots)[2],mach_msg_id_t id) {
    return slots[(id==PublishStereoRing||id==AcquireStereoRing)?1:0];
}
void serve(mach_port_t service) {
    Published slots[2];
    while(true) {
        struct { BrokerRequest request;mach_msg_max_trailer_t trailer; } buffer{};
        auto &request=buffer.request;
        const auto options=MACH_RCV_MSG|MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
        auto result=mach_msg(&request.header,options,0,sizeof(buffer),service,MACH_MSG_TIMEOUT_NONE,MACH_PORT_NULL);
        if(result!=MACH_MSG_SUCCESS) { std::fprintf(stderr,"broker receive: %d\n",result);continue; }
        if(MACH_MSGH_BITS_REMOTE(request.header.msgh_bits)!=MACH_MSG_TYPE_PORT_SEND_ONCE) {
            mach_msg_destroy(&request.header);continue;
        }
        auto replyPort=request.header.msgh_remote_port;
        // Move the reply right into our response; destroy everything else below.
        request.header.msgh_remote_port=MACH_PORT_NULL;
        kern_return_t status=KERN_INVALID_ARGUMENT;mach_port_t memory=MACH_PORT_NULL;uint64_t bytes=0;
        const auto size=request.header.msgh_size;
        auto *trailer=reinterpret_cast<mach_msg_audit_trailer_t*>(reinterpret_cast<char*>(&buffer)+round_msg(size));
        const bool auditOK=size==sizeof(BrokerRequest)&&trailer->msgh_trailer_type==MACH_MSG_TRAILER_FORMAT_0&&trailer->msgh_trailer_size>=sizeof(mach_msg_audit_trailer_t);
        if(auditOK) {
            const auto token=trailer->msgh_audit;
            auto &published=publicationFor(slots,request.header.msgh_id);
            if((request.header.msgh_id==PublishRing||request.header.msgh_id==PublishStereoRing) && (request.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) &&
               request.body.msgh_descriptor_count==1&&request.memory.type==MACH_MSG_PORT_DESCRIPTOR&&
               request.memory.disposition==MACH_MSG_TYPE_PORT_SEND&&matches(token,DriverHost)) {
                MappedAudioRing validation;
                status=validation.mapReadOnly(request.memory.name,request.bytes);
                if(status==KERN_SUCCESS) {
                    // Only one HAL producer owns this dedicated device. A live
                    // producer cannot be replaced by a different process.
                    if(published.live()&&std::memcmp(&published.owner,&token,sizeof(token))!=0) status=KERN_NO_ACCESS;
                    else {
                        published.clear();published.port=request.memory.name;request.memory.name=MACH_PORT_NULL;
                        published.bytes=request.bytes;published.owner=token;
                    }
                }
            } else if((request.header.msgh_id==AcquireRing||request.header.msgh_id==AcquireStereoRing) && !(request.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) &&
                      matches(token,"/Applications/Patchlane.app")) {
                if(!published.live()) { published.clear();status=KERN_FAILURE; }
                else { memory=published.port;bytes=published.bytes;status=KERN_SUCCESS; }
            } else status=KERN_NO_ACCESS;
        }
        mach_msg_destroy(&request.header);
        if(!replyPort) continue;
        BrokerResponse reply{};
        reply.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0);
        reply.header.msgh_remote_port=replyPort;reply.header.msgh_size=sizeof(reply);reply.header.msgh_id=BrokerReply;
        reply.bytes=bytes;reply.status=status;
        if(memory) {
            reply.header.msgh_bits|=MACH_MSGH_BITS_COMPLEX;reply.body.msgh_descriptor_count=1;
            reply.memory.name=memory;reply.memory.disposition=MACH_MSG_TYPE_COPY_SEND;reply.memory.type=MACH_MSG_PORT_DESCRIPTOR;
        }
        result=mach_msg(&reply.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(reply),0,MACH_PORT_NULL,1000,MACH_PORT_NULL);
        // COPY_SEND borrows our persistent memory right; release only the
        // unsent reply right on failure, never the producer's stored right.
        if(result!=MACH_MSG_SUCCESS) mach_port_deallocate(mach_task_self(),replyPort);
    }
}
}
int main() {
    mach_port_t service=MACH_PORT_NULL;
    const auto status=bootstrap_check_in(bootstrap_port,lcshared::BrokerService,&service);
    if(status!=KERN_SUCCESS) { std::fprintf(stderr,"broker launchd check-in failed: %d\n",status);return 1; }
    serve(service);return 0;
}
