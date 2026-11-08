/* Compatibility with the existing 32-byte loader and v2 corruption rejection. */
#include <stdint.h>
#include "../Ums9620Pkg/Include/LoaderHandoff.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
int main(void) {
  W202_LOADER_HANDOFF h={.magic=W202_HANDOFF_MAGIC,.version=1,.size=32,
    .flags=3,.voltage_gain=694,.ntc_high_mv=0xffffffff,.ntc_low_raw=0xffffffff};
  h.checksum=0-W202HandoffSum(&h);
  CHECK(W202HandoffValid(&h));
  h.flags|=4;h.checksum=0;h.checksum=0-W202HandoffSum(&h);
  CHECK(!W202HandoffValid(&h));
  h.version=2;h.size=sizeof(h);h.ntc_high_mv=1400;h.ntc_high_raw=3484;
  h.ntc_low_mv=200;h.ntc_low_raw=470;h.checksum=0;h.checksum=0-W202HandoffSum(&h);
  CHECK(W202HandoffValid(&h));
  ++h.ntc_low_raw;CHECK(!W202HandoffValid(&h));--h.ntc_low_raw;
  ++h.size;CHECK(!W202HandoffValid(&h));--h.size;
  ++h.reserved;CHECK(!W202HandoffValid(&h));--h.reserved;
  h.flags|=8;h.checksum=0;h.checksum=0-W202HandoffSum(&h);
  CHECK(W202HandoffValid(&h));
  h.flags|=16;h.checksum=0;h.checksum=0-W202HandoffSum(&h);
  CHECK(!W202HandoffValid(&h));
  return 0;
}
