#include "AudioCore.h"
#include "../../Shared/BrokerClient.hpp"
#include <cstdio>
extern "C" int lc_shared_probe(char *error,int capacity) {
    lcshared::MappedAudioRing mapping;
    const auto status=lcshared::brokerExchange(mapping,false);
    if(error&&capacity>0) std::snprintf(error,size_t(capacity),"%s (%d)",status==KERN_SUCCESS?"共有音声リングに接続しました":"共有音声リングに接続できません",status);
    return status==KERN_SUCCESS?0:-1;
}
