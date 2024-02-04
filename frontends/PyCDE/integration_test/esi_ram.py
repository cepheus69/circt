# REQUIRES: esi-runtime, esi-cosim, rtl-sim
# RUN: rm -rf %t
# RUN: mkdir %t && cd %t
# RUN: %PYTHON% %s %t 2>&1
# RUN: esi-cosim.py -- %PYTHON% %S/test_software/esi_ram.py cosim env

import pycde
from pycde import (AppID, Clock, Input, Module, generator)
from pycde.esi import DeclareRandomAccessMemory, ServiceDecl
from pycde.bsp import cosim, xrt
from pycde.module import Metadata
from pycde.types import Bits

import sys

RamI64x8 = DeclareRandomAccessMemory(Bits(64), 256)
WriteType = RamI64x8.write.type.req


@ServiceDecl
class MemComms:
  write = ServiceDecl.From(RamI64x8.write.type)
  read = ServiceDecl.From(RamI64x8.read.type)


class Dummy(Module):
  """To test completely automated metadata collection."""

  @generator
  def construct(ports):
    pass


class MemWriter(Module):
  """Write to address 3 the contents of address 2."""

  metadata = Metadata(version="0.1", misc={"numWriters": 1, "style": "stupid"})

  clk = Clock()
  rst = Input(Bits(1))

  @generator
  def construct(ports):
    read_bundle_type = RamI64x8.read.type
    address = 2
    (address_chan, ready) = read_bundle_type.address.wrap(address, True)
    read_bundle, [(_, data_chan)] = read_bundle_type.pack(address=address_chan)
    read_data, read_valid = data_chan.unwrap(True)
    RamI64x8.read(read_bundle, AppID("int_reader"))

    write_bundle_type = RamI64x8.write.type
    write_data, _ = write_bundle_type.req.wrap({
        'data': read_data,
        'address': 3
    }, read_valid)
    write_bundle, [ack] = write_bundle_type.pack(req=write_data)
    RamI64x8.write(write_bundle, appid=AppID("int_writer"))


def Top(xrt: bool):

  class Top(Module):
    clk = Clock()
    rst = Input(Bits(1))

    @generator
    def construct(ports):
      Dummy(appid=AppID("dummy"))
      MemWriter(clk=ports.clk, rst=ports.rst, appid=AppID("mem_writer"))

      # We don't have support for host--device channel communication on XRT yet.
      if not xrt:
        # Pass through reads and writes from the host.
        ram_write = MemComms.write(AppID("write"))
        RamI64x8.write(ram_write, AppID("ram_write"))
        ram_read = MemComms.read(AppID("read"))
        RamI64x8.read(ram_read, AppID("ram_read"))

      # Instantiate the RAM.
      RamI64x8.instantiate_builtin(appid=AppID("mem"),
                                   builtin="sv_mem",
                                   result_types=[],
                                   inputs=[ports.clk, ports.rst])

  return Top


if __name__ == "__main__":
  is_xrt = len(sys.argv) > 2 and sys.argv[2] == "xrt"
  if is_xrt:
    bsp = xrt.XrtBSP
  else:
    bsp = cosim.CosimBSP
  s = pycde.System(bsp(Top(is_xrt)),
                   name="ESIMem",
                   output_directory=sys.argv[1])
  s.generate()
  s.compile()
  s.package()
