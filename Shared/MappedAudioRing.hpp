#pragma once
#include "AudioRing.hpp"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <new>

namespace lcshared {
// Control-thread resource ownership. A read-only memory entry is the only
// capability exported to consumers. Its maximum protection cannot be elevated.
class MappedAudioRing {
    mach_vm_address_t address=0;
    mach_vm_size_t bytes=0;
    mach_port_t entry=MACH_PORT_NULL;
    bool owner=false;
public:
    MappedAudioRing()=default;
    MappedAudioRing(const MappedAudioRing&)=delete;
    MappedAudioRing& operator=(const MappedAudioRing&)=delete;
    ~MappedAudioRing() { close(); }
    void close() {
        if(entry) mach_port_deallocate(mach_task_self(),entry);
        if(address) mach_vm_deallocate(mach_task_self(),address,bytes);
        entry=MACH_PORT_NULL;address=0;bytes=0;owner=false;
    }
    kern_return_t create() {
        close();bytes=round_page(sizeof(AudioRing));
        auto status=mach_vm_allocate(mach_task_self(),&address,bytes,VM_FLAGS_ANYWHERE);
        if(status!=KERN_SUCCESS) { close();return status; }
        auto *ring=new(reinterpret_cast<void*>(address)) AudioRing;
        (void)ring; // Initializes and touches the entire region before RT use.
        memory_object_size_t size=bytes;
        status=mach_make_memory_entry_64(mach_task_self(),&size,address,VM_PROT_READ,&entry,MACH_PORT_NULL);
        if(status!=KERN_SUCCESS||size<sizeof(AudioRing)) { close();return status==KERN_SUCCESS?KERN_INVALID_ARGUMENT:status; }
        owner=true;return KERN_SUCCESS;
    }
    // Retains its own send-right reference after validation. Callers still
    // release their message reference. The retained name identifies the entry
    // across broker acquisitions in this task and cannot be reused while held.
    kern_return_t mapReadOnly(mach_port_t memory,uint64_t size) {
        close();
        if(size!=round_page(sizeof(AudioRing))) return KERN_INVALID_ARGUMENT;
        bytes=size;
        auto status=mach_vm_map(mach_task_self(),&address,bytes,0,VM_FLAGS_ANYWHERE,memory,0,false,
                               VM_PROT_READ,VM_PROT_READ,VM_INHERIT_NONE);
        if(status!=KERN_SUCCESS) { close();return status; }
        if(!view()->compatible(bytes)) { close();return KERN_INVALID_ARGUMENT; }
        status=mach_port_mod_refs(mach_task_self(),memory,MACH_PORT_RIGHT_SEND,1);
        if(status!=KERN_SUCCESS) { close();return status; }
        entry=memory;
        return KERN_SUCCESS;
    }
    bool sameEntry(const MappedAudioRing &other) const { return entry!=MACH_PORT_NULL&&entry==other.entry; }
    AudioRing *producer() { return owner?reinterpret_cast<AudioRing*>(address):nullptr; }
    const AudioRing *view() const { return reinterpret_cast<const AudioRing*>(address); }
    mach_port_t readOnlyPort() const { return entry; }
    uint64_t mappedSize() const { return bytes; }
};
}
