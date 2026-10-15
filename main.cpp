#include "machinelog.hpp"
#include "safequeue.hpp"
#include "casepairs.hpp"
#include "ringbuffer.hpp"
#include "ringbuffer_spsc.hpp"
#include "ringbuffer_mpmc.hpp"


int main() {
  tst::testMachineLog();
  tst::testSafeQueue();
  tst::testCasePairs();
  tst::testRingBuffer();
  tst::testRingBufferSpsc();
  tst::testRingBufferMpmc();
  return 0;
}
