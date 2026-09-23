#include "AudioRing.hpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <new>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL shared ring line %d: %s\n",__LINE__,#x);std::exit(1); } } while(0)
int main() {
    using namespace lcshared;
    const auto bytes=sizeof(AudioRing);
    auto memory=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_ANON|MAP_SHARED,-1,0);
    CHECK(memory!=MAP_FAILED);auto *ring=new(memory) AudioRing;
    CHECK(ring->compatible(bytes));CHECK(!ring->compatible(bytes-1));
    float left=99,right=99;
    CHECK(!ring->readStereo(0,1,0,left,right));CHECK(left==0&&right==0);
    CHECK(!ring->readStereo(0,1,4,left,right));
    Snapshot result;CHECK(!ring->read(0,1,result));
    Snapshot sample;sample.generation=1;sample.sampleTime=-1;sample.hostTime=100;
    for(unsigned c=0;c<Channels;++c) sample.samples[c]=float(c)+0.25f;
    ring->write(sample);CHECK(ring->read(-1,1,result));CHECK(result.hostTime==100);
    for(unsigned bus=0;bus<4;++bus) {
        CHECK(ring->readStereo(-1,1,bus,left,right));
        CHECK(left==float(bus*2)+0.25f&&right==left+1);
    }
    CHECK(!ring->readStereo(-1,2,0,left,right));CHECK(left==0&&right==0);
    CHECK(!ring->read(-1,2,result)); // No stale data after a restart.
    sample.sampleTime=Capacity-1;ring->write(sample);CHECK(!ring->read(-1,1,result));
    CHECK(!ring->readStereo(-1,1,0,left,right));
    auto &slot=ring->frames[Capacity-1];slot.sequence.fetch_add(1);
    CHECK(!ring->read(Capacity-1,1,result)); // Writer interrupted mid-frame: no spin.
    CHECK(!ring->readStereo(Capacity-1,1,0,left,right));CHECK(left==0&&right==0);
    slot.sequence.fetch_add(1);
    int ready[2],ack[2];CHECK(pipe(ready)==0);CHECK(pipe(ack)==0);
    const auto child=fork();CHECK(child>=0);
    if(child==0) {
        close(ready[1]);close(ack[0]);
        CHECK(mprotect(memory,bytes,PROT_READ)==0); // Reader must never write atomics.
        for(unsigned block=0;block<4096;++block) {
            char token;CHECK(read(ready[0],&token,1)==1);
            for(unsigned f=0;f<32;++f) {
                const int64_t t=int64_t(block)*32+f;
                CHECK(ring->read(t,7,result));CHECK(result.sampleTime==t);
                CHECK(result.hostTime==100000+uint64_t(t)*500);
                for(unsigned c=0;c<Channels;++c) CHECK(result.samples[c]==float(t*8+c));
                for(unsigned bus=0;bus<4;++bus) {
                    CHECK(ring->readStereo(t,7,bus,left,right));
                    CHECK(left==result.samples[bus*2]&&right==result.samples[bus*2+1]);
                }
            }
            CHECK(write(ack[1],"x",1)==1);
        }
        _exit(0);
    }
    close(ready[0]);close(ack[1]);
    sample.generation=7;
    for(unsigned block=0;block<4096;++block) {
        for(unsigned f=0;f<32;++f) {
            sample.sampleTime=int64_t(block)*32+f;
            sample.hostTime=100000+uint64_t(sample.sampleTime)*500;
            for(unsigned c=0;c<Channels;++c) sample.samples[c]=float(sample.sampleTime*8+c);
            ring->write(sample);
        }
        CHECK(write(ready[1],"x",1)==1);char token;CHECK(read(ack[0],&token,1)==1);
    }
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    close(ready[1]);close(ack[0]);ring->~AudioRing();CHECK(munmap(memory,bytes)==0);
    std::puts("PASS shared ring: read-only child process, exact samples/host times, wrap, generation, incomplete writes");
}
