#include <protocols/updserver/protocol.hpp>

namespace updclient::updserver {

NandInfo swapNandInfo(const NandInfo &in) noexcept {
  NandInfo out = in;
  out.structVer = swapBe(in.structVer);
  out.kernelVer = swapBe(in.kernelVer);
  out.optFlag = swapBe(in.optFlag);
  out.useFlags = swapBe(in.useFlags);
  out.hwFlags = swapBe(in.hwFlags);
  out.dumpSize = swapBe(in.dumpSize);
  out.blockSize = swapBe(in.blockSize);
  out.pairing = swapBe(in.pairing);
  return out;
}

} // namespace updclient::updserver
