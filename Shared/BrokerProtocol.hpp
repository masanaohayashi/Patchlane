#pragma once
#include <mach/mach.h>
#include <cstdint>
namespace lcshared {
constexpr char BrokerService[]="audio.patchlane.ring-broker";
constexpr mach_msg_id_t PublishRing=0x4c430101,AcquireRing=0x4c430102,BrokerReply=0x4c430103;
constexpr mach_msg_id_t PublishStereoRing=0x4c430201,AcquireStereoRing=0x4c430202;
// Publish includes one memory-entry send right; acquire has no descriptors.
struct BrokerRequest { mach_msg_header_t header;mach_msg_body_t body;mach_msg_port_descriptor_t memory;uint64_t bytes; };
struct BrokerResponse { mach_msg_header_t header;mach_msg_body_t body;mach_msg_port_descriptor_t memory;uint64_t bytes;kern_return_t status; };
}
