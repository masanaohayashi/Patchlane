#include "BrokerProtocol.hpp"
#include "MappedAudioRing.hpp"
#include <servers/bootstrap.h>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL broker line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)
int main() {
    using namespace lcshared;
    mach_port_t server;CHECK(bootstrap_look_up(bootstrap_port,BrokerService,&server)==KERN_SUCCESS);
    MappedAudioRing fake;CHECK(fake.create()==KERN_SUCCESS);
    for(unsigned attempt=0;attempt<3;++attempt) {
        mach_port_t receiver;CHECK(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&receiver)==KERN_SUCCESS);
        BrokerRequest request{};
        request.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE);
        request.header.msgh_remote_port=server;request.header.msgh_local_port=receiver;request.header.msgh_size=sizeof(request);
        request.header.msgh_id=attempt==1?PublishRing:AcquireRing;
        if(attempt==1) {
            request.header.msgh_bits|=MACH_MSGH_BITS_COMPLEX;request.body.msgh_descriptor_count=1;
            request.memory.name=fake.readOnlyPort();request.memory.disposition=MACH_MSG_TYPE_COPY_SEND;request.memory.type=MACH_MSG_PORT_DESCRIPTOR;
            request.bytes=fake.mappedSize();
        }
        if(attempt==2) request.header.msgh_size-=sizeof(uint64_t);
        CHECK(mach_msg(&request.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,request.header.msgh_size,0,MACH_PORT_NULL,2000,MACH_PORT_NULL)==MACH_MSG_SUCCESS);
        struct { BrokerResponse response;mach_msg_max_trailer_t trailer; } reply{};
        const auto receiveStatus=mach_msg(&reply.response.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(reply),receiver,10000,MACH_PORT_NULL);
        if(receiveStatus!=MACH_MSG_SUCCESS) std::fprintf(stderr,"broker attempt=%u receive status=%d\n",attempt,receiveStatus);
        CHECK(receiveStatus==MACH_MSG_SUCCESS);
        CHECK(reply.response.header.msgh_id==BrokerReply);
        CHECK(reply.response.status==(attempt==2?KERN_INVALID_ARGUMENT:KERN_NO_ACCESS));
        CHECK(!(reply.response.header.msgh_bits&MACH_MSGH_BITS_COMPLEX));
        mach_msg_destroy(&reply.response.header);mach_port_mod_refs(mach_task_self(),receiver,MACH_PORT_RIGHT_RECEIVE,-1);
    }
    mach_port_deallocate(mach_task_self(),server);
    std::puts("PASS broker rejects unauthorized reader, forged producer and malformed request");
}
