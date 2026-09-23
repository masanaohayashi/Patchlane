#pragma once
#include "BrokerProtocol.hpp"
#include "MappedAudioRing.hpp"
#include "CodeIdentity.hpp"
#include <servers/bootstrap.h>
#include <bsm/libbsm.h>
#include <algorithm>
namespace lcshared {
constexpr char BrokerExecutable[]="/Library/PrivilegedHelperTools/audio.patchlane.ring-broker";
// Control-thread only. No connection or message passing in audio callbacks.
inline kern_return_t brokerExchange(MappedAudioRing &mapping,bool publish,
    const char *trustedExecutable=BrokerExecutable,uid_t trustedUID=0,CFStringRef pinnedRequirement=nullptr,mach_msg_timeout_t timeoutMS=3000,bool stereo=false) {
    mach_port_t service=MACH_PORT_NULL,receiver=MACH_PORT_NULL;
    auto status=bootstrap_look_up(bootstrap_port,BrokerService,&service);
    if(status!=KERN_SUCCESS) return status;
    status=mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&receiver);
    if(status!=KERN_SUCCESS) { mach_port_deallocate(mach_task_self(),service);return status; }
    BrokerRequest request{};
    request.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE);
    request.header.msgh_size=sizeof(request);request.header.msgh_remote_port=service;request.header.msgh_local_port=receiver;
    request.header.msgh_id=stereo?(publish?PublishStereoRing:AcquireStereoRing):(publish?PublishRing:AcquireRing);
    if(publish) {
        request.header.msgh_bits|=MACH_MSGH_BITS_COMPLEX;request.body.msgh_descriptor_count=1;
        request.memory.name=mapping.readOnlyPort();request.memory.disposition=MACH_MSG_TYPE_COPY_SEND;request.memory.type=MACH_MSG_PORT_DESCRIPTOR;
        request.bytes=mapping.mappedSize();
    }
    status=mach_msg(&request.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(request),0,MACH_PORT_NULL,std::min<mach_msg_timeout_t>(1000,timeoutMS),MACH_PORT_NULL);
    if(status==MACH_MSG_SUCCESS) {
        struct { BrokerResponse response;mach_msg_max_trailer_t trailer; } buffer{};
        auto &response=buffer.response;
        const auto options=MACH_RCV_MSG|MACH_RCV_TIMEOUT|MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)|MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
        status=mach_msg(&response.header,options,0,sizeof(buffer),receiver,timeoutMS,MACH_PORT_NULL);
        if(status==MACH_MSG_SUCCESS) {
            const auto *trailer=reinterpret_cast<const mach_msg_audit_trailer_t*>(reinterpret_cast<const char*>(&buffer)+round_msg(response.header.msgh_size));
            if(response.header.msgh_id!=BrokerReply||response.header.msgh_size!=sizeof(response)||
               trailer->msgh_trailer_type!=MACH_MSG_TRAILER_FORMAT_0||trailer->msgh_trailer_size<sizeof(*trailer)) status=KERN_INVALID_ARGUMENT;
            else if(audit_token_to_euid(trailer->msgh_audit)!=trustedUID||!(pinnedRequirement?matchesRequirement(trailer->msgh_audit,pinnedRequirement):matchesCode(trailer->msgh_audit,trustedExecutable))) status=KERN_NO_ACCESS;
            else {
                status=response.status;
                if(status==KERN_SUCCESS&&!publish) {
                    if(!(response.header.msgh_bits&MACH_MSGH_BITS_COMPLEX)||response.body.msgh_descriptor_count!=1||
                       response.memory.type!=MACH_MSG_PORT_DESCRIPTOR||response.memory.disposition!=MACH_MSG_TYPE_PORT_SEND) status=KERN_INVALID_ARGUMENT;
                    else status=mapping.mapReadOnly(response.memory.name,response.bytes);
                }
            }
            mach_msg_destroy(&response.header);
        }
    }
    mach_port_deallocate(mach_task_self(),service);
    mach_port_mod_refs(mach_task_self(),receiver,MACH_PORT_RIGHT_RECEIVE,-1);
    return status;
}
}
