#include "machinelog.hpp"
#include "safequeue.hpp"
#include "casepairs.hpp"
#include "ringbuffer.hpp"

int main() {
  tst::testMachineLog();
  tst::testSafeQueue();
  tst::testCasePairs();
  tst::testRingBuffer();
  return 0;
}
