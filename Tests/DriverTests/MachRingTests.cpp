#include "MappedAudioRing.hpp"
#include <servers/bootstrap.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL Mach ring line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)
struct Request { mach_msg_header_t header; };
struct Reply { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t memory; uint64_t bytes; };
static mach_port_t receivePort() {
    mach_port_t p;CHECK(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&p)==KERN_SUCCESS);return p;
}
int main(int argc,char **argv) {
    using namespace lcshared;
    if(argc==3&&std::strcmp(argv[1],"--reader")==0) {
        mach_port_t server;CHECK(bootstrap_look_up(bootstrap_port,argv[2],&server)==KERN_SUCCESS);
        const auto replyPort=receivePort();
        Request request{};request.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE);
        request.header.msgh_size=sizeof(request);request.header.msgh_remote_port=server;request.header.msgh_local_port=replyPort;request.header.msgh_id=1;
        CHECK(mach_msg(&request.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(request),0,MACH_PORT_NULL,5000,MACH_PORT_NULL)==MACH_MSG_SUCCESS);
        struct { Reply reply; mach_msg_max_trailer_t trailer; } received{};
        CHECK(mach_msg(&received.reply.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(received),replyPort,5000,MACH_PORT_NULL)==MACH_MSG_SUCCESS);
        auto &reply=received.reply;
        CHECK(reply.header.msgh_id==2&&reply.header.msgh_size==sizeof(Reply));
        CHECK(reply.header.msgh_bits&MACH_MSGH_BITS_COMPLEX);CHECK(reply.body.msgh_descriptor_count==1);
        CHECK(reply.memory.type==MACH_MSG_PORT_DESCRIPTOR);
        MappedAudioRing mapping;
        CHECK(mapping.mapReadOnly(reply.memory.name,reply.bytes+4096)!=KERN_SUCCESS);
        mach_vm_address_t writable=0;
        CHECK(mach_vm_map(mach_task_self(),&writable,reply.bytes,0,VM_FLAGS_ANYWHERE,reply.memory.name,0,false,
                          VM_PROT_READ|VM_PROT_WRITE,VM_PROT_READ|VM_PROT_WRITE,VM_INHERIT_NONE)!=KERN_SUCCESS);
        CHECK(mapping.mapReadOnly(reply.memory.name,reply.bytes)==KERN_SUCCESS);
        CHECK(mapping.producer()==nullptr);
        MappedAudioRing repeated;
        CHECK(repeated.mapReadOnly(reply.memory.name,reply.bytes)==KERN_SUCCESS);
        CHECK(mapping.sameEntry(repeated));
        mach_port_deallocate(mach_task_self(),reply.memory.name);
        Snapshot result;CHECK(mapping.view()->read(1234,9,result));
        CHECK(result.hostTime==987654321);CHECK(result.samples[0]==0.25f&&result.samples[7]==-0.5f);
        // Kernel maximum protection, not merely an application convention.
        CHECK(mach_vm_protect(mach_task_self(),reinterpret_cast<mach_vm_address_t>(mapping.view()),mapping.mappedSize(),false,VM_PROT_READ|VM_PROT_WRITE)!=KERN_SUCCESS);
        mach_port_deallocate(mach_task_self(),server);mach_port_mod_refs(mach_task_self(),replyPort,MACH_PORT_RIGHT_RECEIVE,-1);
        std::puts("PASS Mach child: transferred memory port, exact audio/time, write access denied");return 0;
    }
    CHECK(argc==1);MappedAudioRing producer;CHECK(producer.create()==KERN_SUCCESS);
    mach_port_urefs_t before=0,after=0;
    CHECK(mach_port_get_refs(mach_task_self(),producer.readOnlyPort(),MACH_PORT_RIGHT_SEND,&before)==KERN_SUCCESS);
    {
        MappedAudioRing first,repeated,replacement;
        CHECK(first.mapReadOnly(producer.readOnlyPort(),producer.mappedSize())==KERN_SUCCESS);
        CHECK(repeated.mapReadOnly(producer.readOnlyPort(),producer.mappedSize())==KERN_SUCCESS);
        CHECK(first.sameEntry(repeated));
        CHECK(replacement.create()==KERN_SUCCESS);CHECK(!first.sameEntry(replacement));
        CHECK(mach_port_get_refs(mach_task_self(),producer.readOnlyPort(),MACH_PORT_RIGHT_SEND,&after)==KERN_SUCCESS);
        CHECK(after==before+2);
    }
    CHECK(mach_port_get_refs(mach_task_self(),producer.readOnlyPort(),MACH_PORT_RIGHT_SEND,&after)==KERN_SUCCESS);
    CHECK(after==before);
    Snapshot sample;sample.generation=9;sample.sampleTime=1234;sample.hostTime=987654321;sample.samples[0]=0.25f;sample.samples[7]=-0.5f;
    producer.producer()->write(sample);
    const auto server=receivePort();CHECK(mach_port_insert_right(mach_task_self(),server,server,MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS);
    char name[128];std::snprintf(name,sizeof(name),"audio.patchlane.test.%d",getpid());
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CHECK(bootstrap_register(bootstrap_port,name,server)==KERN_SUCCESS);
#pragma clang diagnostic pop
    const auto child=fork();CHECK(child>=0);
    if(child==0) { execl(argv[0],argv[0],"--reader",name,nullptr);_exit(127); }
    struct { Request request; mach_msg_max_trailer_t trailer; } received{};
    CHECK(mach_msg(&received.request.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(received),server,5000,MACH_PORT_NULL)==MACH_MSG_SUCCESS);
    Reply reply{};reply.header.msgh_bits=MACH_MSGH_BITS_COMPLEX|MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0);
    reply.header.msgh_size=sizeof(reply);reply.header.msgh_remote_port=received.request.header.msgh_remote_port;reply.header.msgh_id=2;
    reply.body.msgh_descriptor_count=1;reply.memory.name=producer.readOnlyPort();reply.memory.disposition=MACH_MSG_TYPE_COPY_SEND;reply.memory.type=MACH_MSG_PORT_DESCRIPTOR;reply.bytes=producer.mappedSize();
    CHECK(mach_msg(&reply.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(reply),0,MACH_PORT_NULL,5000,MACH_PORT_NULL)==MACH_MSG_SUCCESS);
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    // launchd removes this temporary registration when its receive right dies.
    CHECK(mach_port_deallocate(mach_task_self(),server)==KERN_SUCCESS);
    CHECK(mach_port_mod_refs(mach_task_self(),server,MACH_PORT_RIGHT_RECEIVE,-1)==KERN_SUCCESS);
    std::puts("PASS Mach ring: separate exec process, read-only shared mapping, port cleanup");
}
