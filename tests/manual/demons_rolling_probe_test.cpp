#include "graphics/host_gpu/renderer/demonsRollingProbe.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <vector>

using Libs::Graphics::DemonsRollingProbe::FrameGate;
int main() {
    FrameGate short_session;
    assert(short_session.Due(0, 32));
    assert(!short_session.Due(10, 32));
    assert(short_session.Due(32, 32));
    // Session may stop before, exactly at, or well after any older milestone.
    FrameGate late_session;
    assert(late_session.Due(735, 32));
    assert(!late_session.Due(736, 32));
    assert(late_session.Due(767, 32));
    assert(late_session.Due(1201, 32));
    assert(late_session.Due(4096, 32));
    assert(late_session.Due(2, 32)); // restarted frame counter
    assert(!late_session.Due(1, 32)); // a repeated reordered sample is ignored
    assert(late_session.Due(34, 32));
    FrameGate threaded;
    std::atomic<int> won{0};
    std::vector<std::thread> jobs;
    for (int i=0;i<8;i++) jobs.emplace_back([&] { if(threaded.Due(88, 30)) won++; });
    for (auto& t:jobs) t.join();
    assert(won == 1);
    return 0;
}
