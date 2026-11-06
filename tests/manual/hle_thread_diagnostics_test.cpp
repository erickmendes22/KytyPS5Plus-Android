#include "cpu/hleThreadDiagnostics.h"
#include <cassert>
int main() {
    setenv("KYTY_DS_FRAME_TRACE", "1", 1);
    using namespace Cpu::HleDiagnostics;
    {
        Scope outer(17, "wait", 0x1234, 7, 8, 9);
        auto* slot = CurrentSlot();
        assert(slot && slot->state.active && slot->state.id == 17);
        {
            Scope inner(18, "nested callback", 0x5678, 1, 2, 3);
            assert(slot->state.active && slot->state.id == 18);
            Dump();
        }
        assert(slot->state.active && slot->state.id == 17 && slot->state.caller == 0x1234);
    }
    assert(!CurrentSlot()->state.active && CurrentSlot()->state.id == 17);
    std::thread worker([] { Scope scope(19, "worker", 0x9999, 0, 0, 0); });
    worker.join();
    assert(next_slot.load() == 2);
    Dump();
}
