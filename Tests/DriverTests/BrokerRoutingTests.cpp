#define main broker_daemon_main
#include "../../Broker/RingBroker.cpp"
#undef main
#include <cassert>
int main() {
    using namespace lcshared;
    Published slots[2];
    publicationFor(slots,PublishRing).bytes=800;
    publicationFor(slots,PublishStereoRing).bytes=200;
    assert(publicationFor(slots,AcquireRing).bytes==800);
    assert(publicationFor(slots,AcquireStereoRing).bytes==200);
    publicationFor(slots,PublishStereoRing).clear();
    assert(publicationFor(slots,AcquireRing).bytes==800);
    assert(publicationFor(slots,AcquireStereoRing).bytes==0);
    publicationFor(slots,PublishStereoRing).bytes=201;
    publicationFor(slots,PublishRing).clear();
    assert(publicationFor(slots,AcquireStereoRing).bytes==201);
    puts("PASS broker: stereo/eight-channel publication, acquire, disconnect and replacement remain isolated");
}
