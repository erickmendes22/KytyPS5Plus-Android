#include "graphics/host_gpu/renderer/demonsP8CapturePolicy.h"
#include <cassert>
using namespace Libs::Graphics::DemonsP8CapturePolicy;
int main(){
    static_assert(ClusterShader(0x1ce1a858f62b5a11ull));
    static_assert(!ClusterShader(0x3dfe402653925fbfull));
    static_assert(ClusterExtent(351424,43928));
    static_assert(ClusterExtent(881280,110160));
    static_assert(!ClusterExtent(351424,43924));
    static_assert(!ClusterExtent(0,0));
    static_assert(!ClusterExtent(8ull*ClusterRangeLimit,ClusterRangeLimit));
    static_assert(ClusterRange(351424,351424,0));
    static_assert(!ClusterRange(351424,351424,4));
    static_assert(ClusterRange(351424,351428,4));
    static_assert(!ClusterRange(0,16,0));
    static_assert(!ClusterRange(3,16,0));
    static_assert(!ClusterRange(8,4,8));
    static_assert(ClusterCopyBudget<=CriticalCopyBudget);
    assert(ClusterDispatchLimit==8);
}
