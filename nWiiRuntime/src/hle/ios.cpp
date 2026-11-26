#include "input/gip_wiimote.h"
#include "runtime/platform/platform.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include "runtime/hw/hw.h"
#include "runtime/event_scheduler.h"
#include "runtime/os_thread.h"
#include "runtime/virtual_disc.h"

using namespace nwii::runtime;

namespace nwii::runtime {
    std::string read_guest_string(CPUContext &ctx, uint32_t addr, int max_len = 256);
    bool valid_callback(uint32_t cb);
}

#include "runtime/ios_device.h"
#include "runtime/ios_kernel.h"
#include "runtime/devices.h"
#include <iostream>
#include <vector>

extern "C" void run_game(nwii::runtime::CPUContext &ctx);

static uint32_t phys_to_virt(uint32_t addr) {
  if (addr == 0)
    return 0;
  if (addr >= 0x80000000 && addr < 0x94000000)
    return addr;
  if (addr >= 0x00800000 && addr < 0x04000000)
    return addr | 0x90000000;
  if (addr < 0x00800000)
    return addr | 0x80000000;
  if (addr >= 0x10000000 && addr < 0x14000000)
    return addr | 0x90000000;
  return addr;
}

std::string nwii::runtime::read_guest_string(CPUContext &ctx, uint32_t addr,
                                     int max_len) {
  std::string path;
  if (addr == 0) {
    std::cout << "[HLE IOS] read_guest_string: null address! pc=0x" << std::hex
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
    return path;
  }
  uint32_t vaddr = phys_to_virt(addr);
  if (vaddr >= 0x80000000 && vaddr < 0x94000000) {
    for (int i = 0; i < max_len; ++i) {
      char c = ctx.mmu.read8(vaddr + i);
      if (c == '\0')
        break;
      if (c >= 32 && c <= 126)
        path += c;
    }
    if (path.empty()) {
      std::cout << "[HLE IOS] read_guest_string: string at 0x" << std::hex
                << addr << " (vaddr 0x" << vaddr
                << ") is empty (first char is 0x" << (int)ctx.mmu.read8(vaddr)
                << ") pc=0x" << ctx.pc << " lr=0x" << ctx.lr << std::dec
                << std::endl;
    }
  } else {
    std::cout << "[HLE IOS] read_guest_string: invalid translated address 0x"
              << std::hex << vaddr << " (orig: 0x" << addr << ") pc=0x"
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
  }
  return path;
}

static constexpr int32_t IPC_EIO = -8;


static constexpr uint32_t IPC_POOL_BASE = 0x93EE0000;
static constexpr uint32_t IPC_SLOT_SIZE = 128; 
static constexpr uint32_t IPC_POOL_SLOTS = 64;
static uint32_t ipc_pool_next = 0; 




static uint32_t ios_guest_heap_ptr = 0x93E40000;
static uint32_t ios_ipc_heap_ptr = 0x93E00000;

static uint32_t ios_guest_alloc(CPUContext &ctx, uint32_t size, uint32_t align,
                                int heap_id = 0) {
  if (align < 32)
    align = 32;
  
  uint32_t &heap =
      (heap_id == 0 && size <= 128) ? ios_ipc_heap_ptr : ios_guest_heap_ptr;
  heap = (heap + align - 1) & ~(align - 1);
  uint32_t ptr = heap;
  heap += (size + align - 1) & ~(align - 1);
  for (uint32_t i = 0; i < size; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}

static uint32_t ipc_pool_alloc(CPUContext &ctx) {
  uint32_t slot = ipc_pool_next;
  ipc_pool_next = (ipc_pool_next + 1) % IPC_POOL_SLOTS;
  uint32_t ptr = IPC_POOL_BASE + slot * IPC_SLOT_SIZE;
  
  for (uint32_t i = 0; i < IPC_SLOT_SIZE; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}


bool nwii::runtime::valid_callback(uint32_t cb) {
  return cb != 0 && cb != 0xFFFFFFFFu && cb >= 0x80000000u && cb < 0x82000000u;
}

static std::shared_ptr<std::ifstream> g_disc_file;
static bool g_disc_is_wbfs = false;
static uint32_t g_wbfs_block_size = 0;
static std::vector<uint16_t> g_wbfs_bat;

static bool init_wbfs(const std::filesystem::path &path) {
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!file->is_open())
    return false;

  char magic[4];
  file->read(magic, 4);
  if (magic[0] != 'W' || magic[1] != 'B' || magic[2] != 'F' ||
      magic[3] != 'S') {
    return false;
  }

  uint8_t shift;
  file->read(reinterpret_cast<char *>(&shift), 1);
  g_wbfs_block_size = 1u << (shift + 17);

  file->seekg(0, std::ios::end);
  size_t file_size = file->tellg();
  file->seekg(0x100, std::ios::beg);

  size_t num_entries = (file_size - 0x100) / 2;
  if (num_entries > 65536)
    num_entries = 65536;

  g_wbfs_bat.resize(num_entries);
  for (size_t i = 0; i < num_entries; ++i) {
    uint16_t entry;
    file->read(reinterpret_cast<char *>(&entry), 2);
    g_wbfs_bat[i] = ((entry >> 8) & 0xFF) | ((entry << 8) & 0xFF00);
  }

  g_disc_file = file;
  g_disc_is_wbfs = true;
  std::cout << "[HLE IOS] Opened WBFS: " << path
            << " block_size=" << g_wbfs_block_size << std::endl;
  return true;
}

static void init_iso(const std::filesystem::path &path) {
  g_disc_file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (g_disc_file->is_open()) {
    g_disc_is_wbfs = false;
    std::cout << "[HLE IOS] Opened ISO: " << path << std::endl;
  }
}

static std::shared_ptr<std::ifstream> get_disc_file() {
  if (!g_disc_file) {
    auto &cfg = Config::get();
    std::filesystem::path game_dir(cfg.game_dir);
    std::filesystem::path p;

    p = game_dir / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }

    if (std::filesystem::exists(game_dir)) {
      for (const auto &entry : std::filesystem::directory_iterator(game_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
    if (std::filesystem::exists(game_dir.parent_path())) {
      for (const auto &entry :
           std::filesystem::directory_iterator(game_dir.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
  }
  return g_disc_file;
}

static void read_disc_data(uint8_t *dst, uint64_t offset, uint32_t length) {
  
  auto &vd = VirtualDisc::get();
  if (!vd.valid())
    vd.init(Config::get().game_dir);
  if (vd.valid() && vd.read(offset, length, dst))
    return;

  auto file = get_disc_file();
  if (!file || !file->is_open()) {
    std::memset(dst, 0, length);
    return;
  }

  if (!g_disc_is_wbfs) {
    file->seekg(offset);
    file->read(reinterpret_cast<char *>(dst), length);
  } else {
    uint32_t done = 0;
    while (done < length) {
      uint64_t block_idx = (offset + done) / g_wbfs_block_size;
      uint32_t block_off = (offset + done) % g_wbfs_block_size;
      uint32_t chunk =
          std::min<uint32_t>(length - done, g_wbfs_block_size - block_off);

      if (block_idx < g_wbfs_bat.size() && g_wbfs_bat[block_idx] != 0) {
        uint64_t phys =
            (uint64_t)g_wbfs_bat[block_idx] * g_wbfs_block_size + block_off;
        file->seekg(phys);
        file->read(reinterpret_cast<char *>(dst + done), chunk);
      } else {
        std::memset(dst + done, 0, chunk);
      }
      done += chunk;
    }
  }
}

namespace nwii {
namespace runtime {

struct PendingCallback {
  uint32_t callback;
  uint32_t arg1;
  uint32_t arg2;
};

static std::vector<PendingCallback> g_pending_callbacks;



} 
} 

static void di_read_internal(nwii::runtime::MMU *mmu, uint32_t inbuf, uint32_t outbuf,
                             uint32_t callback, uint32_t userdata,
                             bool is_async, uint32_t out_size = 0) {
  
  uint32_t cmd = 0, transferSize = 0, addr = 0, length = 0;
  uint64_t offset = 0;

  uint32_t ipc_ptr = phys_to_virt(inbuf);

  if (ipc_ptr >= 0x80000000 && ipc_ptr < 0x94000000) {
    uint32_t raw_cmd = mmu->read32(ipc_ptr + 0x00);
    cmd = raw_cmd >> 24; 
    if (cmd == 1) {
      cmd = 0x12; 
    } else if (cmd == 0) {
      
      if (raw_cmd == 0x20)
        cmd = 0x20;
    }

    transferSize = mmu->read32(ipc_ptr + 0x04);
    addr = mmu->read32(ipc_ptr + 0x08);

    uint32_t val10 = mmu->read32(ipc_ptr + 0x10);
    uint32_t val14 = mmu->read32(ipc_ptr + 0x14);

    if (nwii::runtime::Config::get().platform ==
        nwii::runtime::Platform::GameCube) {
      offset = ((uint64_t)val14) << 2; 
    } else {
      
      offset = (((uint64_t)(val10 & 0xFFFF) << 32) | val14) << 2;
    }

    length = mmu->read32(ipc_ptr + 0x18);
  }
  
  if (addr == 0 && outbuf != 0) {
      addr = outbuf;
  }

  std::cout << "[HLE IOS] DI_Read" << (is_async ? "Async" : "") << ": inbuf=0x"
            << std::hex << inbuf << " cmd=0x" << cmd
            << " transferSize=" << std::dec << transferSize << " addr=0x"
            << std::hex << addr << " cb=0x" << callback << " offset=0x"
            << offset << " length=" << std::dec << length << " outbuf=0x" << std::hex << outbuf << std::endl;

  bool use_defaults =
      (cmd != 0x20 && cmd != 0x12 && cmd != 0x95 && cmd != 0x71 && cmd != 0x72 && cmd != 0x96 && cmd != 0xe3) || (offset > 0x1FFFFFFFFFFULL);

  if (use_defaults) {
    std::cout << "[HLE IOS] DI_Read: Using defaults (invalid structure)"
              << std::endl;
    cmd = 0x20;
    transferSize = 2;
    offset = 0;
    length = 0x440;
  }

  uint32_t buf_ptr = phys_to_virt(addr);
  uint32_t total_length = transferSize * 2048;
  if (length > 0 && length < total_length)
    total_length = length;
  if (total_length == 0 && length > 0)
    total_length = length;
  if (total_length == 0)
    total_length = 0x440;
  
  if (out_size > 0 && total_length > out_size)
    total_length = out_size;

  std::cout << "[HLE IOS] DI_Read final: offset=0x" << std::hex
            << (uint32_t)offset << " total_length=" << std::dec << total_length
            << " buf_ptr=0x" << std::hex << buf_ptr << std::dec << std::endl;

  auto file = get_disc_file();
  bool success = false;
  uint32_t bytes_read = 0;
  bool have_media = (file && file->is_open()) || VirtualDisc::get().valid();

  if (have_media && buf_ptr != 0 && total_length > 0) {
    std::vector<uint8_t> temp(total_length);
    read_disc_data(temp.data(), (uint32_t)offset, total_length);
    bytes_read = total_length;
    for (uint32_t i = 0; i < bytes_read; i++)
      mmu->write8(buf_ptr + i, temp[i]);
    std::cout << "[DI] Read " << bytes_read << " bytes" << std::endl;
    success = true;
  } else if (buf_ptr != 0) {
    if (offset == 0) {
      const std::string& gid = Config::get().game_id;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      for (int i = 0; i < (int)gid.size() && i < 4; i++)
        mmu->write8(buf_ptr + i, (uint8_t)gid[i]);
      mmu->write32(buf_ptr + 0x18, 0x5D1C9EA3);
      std::cout << "[DI] Faked disc header (" << gid << ")" << std::endl;
      success = true;
      bytes_read = total_length;
    } else {
      
      std::cout << "[DI] No disc file, zeroing out read buffer of size " << total_length << std::endl;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      success = true;
      bytes_read = total_length;
    }
  }
}

void di_read_internal_shared(nwii::runtime::MMU* mmu, uint32_t inbuf, uint32_t outbuf,
                              uint32_t callback, uint32_t userdata, bool is_async,
                              uint32_t out_size) {
    di_read_internal(mmu, inbuf, outbuf, callback, userdata, is_async, out_size);
}

extern "C" {
void IOS_Open(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open(ctx);
}

void IOS_OpenAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open_async(ctx);
}

void IOS_Close(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close(ctx);
}

void IOS_Read(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read(ctx);
}

void IOS_Write(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write(ctx);
}

void IOS_Ioctl(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl(ctx);
}

void IOS_Ioctlv(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv(ctx);
}

void iosAlloc(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t size = ctx.gpr[4];
  uint32_t align = ctx.gpr[5];
  uint32_t ptr = ios_guest_alloc(ctx, size, align, heap_id);
  std::cout << "[HLE IOS] iosAlloc: heap=" << heap_id << " size=" << size
            << " align=" << align << " -> ptr=0x" << std::hex << ptr << " LR=0x" << ctx.lr << std::dec
            << std::endl;
  ctx.gpr[3] = ptr;
  ctx.pc = ctx.lr;
}


void iosFree(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t ptr = ctx.gpr[4];
  std::cout << "[HLE IOS] iosFree: heap=" << heap_id << " ptr=0x" << std::hex
            << ptr << " LR=0x" << ctx.lr << std::dec << std::endl;
  ctx.gpr[3] = 0; 
  ctx.pc = ctx.lr;
}

void IOS_IoctlAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl_async(ctx);
}

void IOS_IoctlvAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv_async(ctx);
}

void IOS_Seek(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);
  ctx.gpr[3] = result;
  ctx.pc = ctx.lr;
}

void IOS_CloseAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close_async(ctx);
}

void IOS_ReadAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read_async(ctx);
}

void IOS_WriteAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write_async(ctx);
}

void IOS_SeekAsync(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];
  uint32_t callback = ctx.gpr[6];
  uint32_t userdata = ctx.gpr[7];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);

  if (valid_callback(callback)) {
    ctx.queue_callback(callback, result, userdata);
  }

  ctx.gpr[3] = IPC_OK;
  ctx.pc = ctx.lr;
}

} 

namespace nwii {
namespace runtime {
extern MMU *g_mmu;
}
} 

extern "C" int32_t handle_ios_ipc(CPUContext& ctx, uint32_t request_addr) {
  if (!nwii::runtime::g_mmu)
    return 0;

  

  uint32_t virt_addr = request_addr;
  if (virt_addr < 0x01800000) {
    virt_addr |= 0x80000000;
  } else if (virt_addr >= 0x10000000 && virt_addr < 0x14000000) {
    virt_addr = (virt_addr & 0x03FFFFFF) | 0x90000000;
  }

  // IOS IPC request layout (Dolphin IOS::HLE):
  
  uint32_t cmd = nwii::runtime::g_mmu->read32(virt_addr);
  int32_t result = IPC_OK;

  if (cmd == 1) { 
    
    uint32_t path_addr = nwii::runtime::g_mmu->read32(virt_addr + 0x0C);
    uint32_t mode = nwii::runtime::g_mmu->read32(virt_addr + 0x10);
    
    if (path_addr < 0x01800000)
      path_addr |= 0x80000000;
    else if (path_addr >= 0x10000000 && path_addr < 0x14000000)
      path_addr = (path_addr & 0x03FFFFFF) | 0x90000000;

    std::string path;
    for (int i = 0; i < 64; ++i) {
      char c = (char)ctx.mmu.read8(path_addr + i);
      if (c == '\0') break;
      path += c;
    }
    result = IOSKernel::get().open(ctx, path, mode);
    std::cout << "[HW IPC] Open path='" << path << "' -> fd=" << result << "\n";
  } else if (cmd == 2) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    result = IOSKernel::get().close(ctx, fd);
  } else if (cmd == 3) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().read(ctx, fd, buf, len);
  } else if (cmd == 4) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().write(ctx, fd, buf, len);
  } else if (cmd == 5) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    int32_t offset = (int32_t)ctx.mmu.read32(virt_addr + 0x0C);
    int32_t whence = (int32_t)ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().seek(ctx, fd, offset, whence);
  } else if (cmd == 6) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t in_buf = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t in_size = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t out_buf = ctx.mmu.read32(virt_addr + 0x18);
    uint32_t out_size = ctx.mmu.read32(virt_addr + 0x1C);

    std::cout << "[HW IPC] Ioctl fd=" << fd << " cmd=0x" << std::hex << ioctl_cmd << std::dec << "\n";

    IpcRequest req{};
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.in_buf = in_buf;
    req.in_size = in_size;
    req.out_buf = out_buf;
    req.out_size = out_size;
    result = IOSKernel::get().ioctl(ctx, req);
  } else if (cmd == 7) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t arg_in = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t arg_out = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t arg_array = ctx.mmu.read32(virt_addr + 0x18);

    IpcRequest req{};
    req.cmd = cmd;
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.arg_cnt_in = arg_in;
    req.arg_cnt_out = arg_out;

    std::cout << "[HW IPC] Parsed IOCTLV fd=" << fd << " cmd=" << std::hex << ioctl_cmd << std::dec 
              << " in=" << arg_in << " out=" << arg_out << "\n";

    for (uint32_t i = 0; i < arg_in + arg_out; i++) {
        IoctlvVector vec;
        vec.addr = ctx.mmu.read32(arg_array + i * 8);
        vec.len = ctx.mmu.read32(arg_array + i * 8 + 4);
        req.ioctlv_vecs.push_back(vec);
    }

    result = IOSKernel::get().ioctlv(ctx, req);
  }

  std::cout << "[HW IPC] cmd=" << cmd << " virt=0x" << std::hex << virt_addr
            << " -> result=" << (int32_t)result << std::dec << std::endl;

  if (result != -0x70000001) { 
      nwii::runtime::g_mmu->write32(virt_addr + 4, (uint32_t)(int32_t)result);
  }
  return result;
}

namespace nwii {
namespace runtime {

void init_ipc_client(CPUContext &ctx) {
  IOSKernel::get().init();
  nwii::runtime::devices::register_all();
}

bool handle_syscall(CPUContext &ctx) {
  uint32_t syscall_id = ctx.gpr[0];
  uint32_t old_pc = ctx.pc; 

  switch (syscall_id) {
  case 0x61:
    IOS_Open(ctx);
    break;
  case 0x62:
    IOS_Close(ctx);
    break;
  case 0x63:
    IOS_Read(ctx);
    break;
  case 0x64:
    IOS_Write(ctx);
    break;
  case 0x65:
    IOS_Seek(ctx);
    break;
  case 0x66:
    IOS_Ioctl(ctx);
    break;
  case 0x67:
    IOS_Ioctlv(ctx);
    break;
  case 0x68:
    IOS_OpenAsync(ctx);
    break;
  case 0x69:
    IOS_CloseAsync(ctx);
    break;
  case 0x6A:
    IOS_ReadAsync(ctx);
    break;
  case 0x6B:
    IOS_IoctlAsync(ctx);
    break;
  case 0x6C:
    IOS_IoctlvAsync(ctx);
    break;
  default:

    
    if (process_pending_callbacks(ctx))
      return true;
    break;
  }

  ctx.pc = old_pc; 
  return false;
}

} 
} 

#include "runtime/cpu_context.h"

namespace nwii::runtime {



bool g_ipc_reply_pending = false;

int g_ipc_interrupt_delay = 0;


static inline uint32_t get_callback_stack_top(const CPUContext& ctx) {
    uint32_t v = Config::get().game_profile.callback_stack_top;
    return v ? v : ctx.bootstrap_stack_top;
}

void hle_drive_thread_queue(CPUContext& ctx) {
    
}



static void hle_save_context_to_guest(CPUContext &ctx, uint32_t c) {
  if (std::getenv("NWII_SAMPLE")) {
      static int n = 0;
      if (n++ < 40)
          std::cout << "[CtxSave] #" << n << " ctx=0x" << std::hex << c
                    << " srr0<=0x" << ctx.pc << " msr=0x" << ctx.msr
                    << " lr=0x" << ctx.lr << std::dec << "\n";
  }
  for (int i = 0; i < 32; i++)
    ctx.mmu.write32(c + i * 4, ctx.gpr[i]);

  

  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) | 0x0002);
  uint32_t cr_val = 0;
  for (int i = 0; i < 8; i++) {
    cr_val |= (ctx.cr[i].lt ? 8u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].gt ? 4u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].eq ? 2u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].so ? 1u : 0) << (28 - i * 4);
  }
  ctx.mmu.write32(c + 0x80, cr_val);
  ctx.mmu.write32(c + 0x84, ctx.lr);
  ctx.mmu.write32(c + 0x88, ctx.ctr);
  ctx.mmu.write32(c + 0x8C, ctx.xer);
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.fpr[i], 8);
    ctx.mmu.write64(c + 0x90 + i * 8, d);
  }
  ctx.mmu.write32(c + 0x194, ctx.fpscr);
  ctx.mmu.write32(c + 0x198, ctx.pc);  
  ctx.mmu.write32(c + 0x19C, ctx.msr); 
  for (int i = 0; i < 8; i++)
    ctx.mmu.write32(c + 0x1A4 + i * 4, ctx.gqr[i]);
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.ps1[i], 8);
    ctx.mmu.write64(c + 0x1C8 + i * 8, d);
  }
}


static bool hle_load_context_from_guest(CPUContext &ctx) {
  uint32_t c = ctx.mmu.read32(0x800000D4);
  if (c == 0)
    return false;
  for (int i = 0; i < 32; i++)
    ctx.gpr[i] = ctx.mmu.read32(c + i * 4);
  uint32_t cr_val = ctx.mmu.read32(c + 0x80);
  for (int i = 0; i < 8; i++) {
    uint32_t nib = (cr_val >> (28 - i * 4)) & 0xF;
    ctx.cr[i].lt = (nib & 8) != 0;
    ctx.cr[i].gt = (nib & 4) != 0;
    ctx.cr[i].eq = (nib & 2) != 0;
    ctx.cr[i].so = (nib & 1) != 0;
  }
  ctx.lr = ctx.mmu.read32(c + 0x84);
  ctx.ctr = ctx.mmu.read32(c + 0x88);
  ctx.xer = ctx.mmu.read32(c + 0x8C);
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x90 + i * 8);
    std::memcpy(&ctx.fpr[i], &d, 8);
  }
  ctx.fpscr = ctx.mmu.read32(c + 0x194);
  for (int i = 0; i < 8; i++)
    ctx.gqr[i] = ctx.mmu.read32(c + 0x1A4 + i * 4);
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x1C8 + i * 8);
    std::memcpy(&ctx.ps1[i], &d, 8);
  }
  ctx.pc = ctx.mmu.read32(c + 0x198);  
  ctx.msr = ctx.mmu.read32(c + 0x19C); 

  

  
  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) & ~0x0002);
  return true;
}

int g_trace_after_dec = 0;

// SelectThread's idle spin (0x800E2F48) waits until the run-queue bitmap
// at r13-26624 is non-zero. A thread can be left READY with a null queue
// pointer, so the bit stays clear and the CPU never draws. Relink those
// threads the same way SetRun does, then let the spin fall into the pop.
static void repair_ready_runqueue(CPUContext &ctx) {
  if (ctx.pc != 0x800E2F48u && ctx.pc != 0x800E2F58u) return;
  if (!ctx.cr[0].eq) return;
  uint32_t r13 = ctx.gpr[13];
  if (r13 < 0x80000000u || r13 >= 0x81800000u) return;

  uint32_t bitmap_addr = r13 - 26624u;
  uint32_t bitmap = ctx.mmu.read32(bitmap_addr);
  constexpr uint32_t kRunQ = 0x8034D0F0u;
  uint32_t th = ctx.mmu.read32(0x800000DCu);
  uint32_t bits = 0;
  int logged = 0;
  for (int guard = 0; th >= 0x80000000u && th < 0x81800000u && guard < 16; ++guard) {
    uint16_t state = ctx.mmu.read16(th + 0x2C8);
    uint32_t queue = ctx.mmu.read32(th + 0x2DC);
    uint32_t prio = ctx.mmu.read32(th + 0x2D0);
    uint32_t next = ctx.mmu.read32(th + 0x2FC);
    bool on_runq = queue >= kRunQ && queue < kRunQ + 32u * 8u;
    // OSWakeupThread stores READY before it links the run queue, and it
    // skips that link when suspend is non-zero. A finished IPC sleep then
    // sits READY with the old sleep queue still in +0x2DC, and the idle
    // loop never sees a bit in the run-queue bitmap.
    if (state == 1 && prio < 32 && queue != 0 && !on_runq) {
      uint32_t req = queue - 0x2Cu;
      uint32_t cmd = (req >= 0x80000000u && req < 0x94000000u)
                         ? ctx.mmu.read32(req) : 0;
      if (cmd >= 1 && cmd <= 8) {
        uint32_t suspend = ctx.mmu.read32(th + 0x2CC);
        if (suspend != 0)
          ctx.mmu.write32(th + 0x2CC, 0);
        ctx.mmu.write32(th + 0x2DC, 0);
        ctx.mmu.write32(th + 0x2E0, 0);
        ctx.mmu.write32(th + 0x2E4, 0);
        queue = 0;
        static int n = 0;
        if (n++ < 6)
          std::cout << "[Sched] IPC wakeup stranded 0x" << std::hex << th
                    << " suspend=" << std::dec << suspend << "\n";
      }
    }
    if (state == 1 && prio < 32 && (queue == 0 || on_runq)) {
      if (queue == 0) {
        uint32_t q = kRunQ + prio * 8u;
        uint32_t tail = ctx.mmu.read32(q + 4);
        if (tail == 0)
          ctx.mmu.write32(q + 0, th);
        else if (tail >= 0x80000000u && tail < 0x81800000u)
          ctx.mmu.write32(tail + 736, th);
        ctx.mmu.write32(th + 740, tail);
        ctx.mmu.write32(th + 736, 0);
        ctx.mmu.write32(q + 4, th);
        ctx.mmu.write32(th + 732, q);
      }
      bits |= 1u << (31u - prio);
      if (logged++ < 4) {
        std::cout << "[Sched] queued READY thread 0x" << std::hex << th
                  << " prio=" << std::dec << prio << " srr0=0x" << std::hex
                  << ctx.mmu.read32(th + 0x198) << std::dec << "\n";
      }
    }
    th = next;
  }
  if (bits == 0 && bitmap == 0) return;
  uint32_t merged = bitmap | bits;
  if (merged == 0) return;
  ctx.mmu.write32(bitmap_addr, merged);
  ctx.mmu.write32(r13 - 26628u, 1);
  ctx.cr[0].eq = false;
  ctx.cr[0].lt = false;
  ctx.cr[0].gt = true;
}

// Four 32-byte DI request slots. Byte +8 is the in-use flag. A cover-clear
// ioctl (0x86) marks a slot and, if its completion never clears it, the next
// read spins forever in newContext ("freeDvdContext.inUse is true").
static void release_di_request_slots(CPUContext &ctx) {
  uint32_t r13 = ctx.gpr[13];
  if (r13 >= 0x80000000u && r13 < 0x81800000u)
    ctx.mmu.write8((uint32_t)(r13 - 26280), 0);
  for (int i = 0; i < 4; ++i)
    ctx.mmu.write8(0x80352B20u + (uint32_t)i * 32u + 8u, 0);
}

// The busy-slot spins do not retest the flag. Drop it and resume at the
// instruction after the spin so the issuer can reach IOS_IoctlAsync.
static void escape_di_slot_spin(CPUContext &ctx) {
  uint32_t resume = 0;
  if (ctx.pc == 0x800F0058u) resume = 0x800F005Cu;
  else if (ctx.pc == 0x800EF868u) resume = 0x800EF86Cu;
  else return;
  release_di_request_slots(ctx);
  static int n = 0;
  if (n++ < 6)
    std::cout << "[DVD] DI slot busy at 0x" << std::hex << ctx.pc
              << ", continuing at 0x" << resume << std::dec << std::endl;
  ctx.pc = resume;
  longjmp(ctx.exception_jmp_buf, 1);
}

// FUN_801d3d94 waits until its file-flag byte is cleared by the DVD callback.
// The recompiled issuer can return with the command left BUSY and no DI ioctl
// in flight, so the callback never runs and the screen stays black. Finish
// that read from the extracted disc. The command offset is a 4-byte word
// address (FST startAddr + fileOffset>>2); VirtualDisc wants bytes.
static void service_stuck_dvd_read(CPUContext &ctx) {
  if (ctx.pc != 0x801D3E7Cu || ctx.in_callback) return;
  uint32_t r13 = ctx.gpr[13];
  if (r13 < 0x80000000u || r13 >= 0x81800000u) return;

  uint32_t exec = ctx.mmu.read32((uint32_t)(r13 - 26320));
  if (exec < 0x80000000u || exec >= 0x94000000u) return;
  if (ctx.mmu.read32(exec + 8) != 1) return;
  if ((int32_t)ctx.mmu.read32(exec + 12) != 1) return;

  uint32_t word_off = ctx.mmu.read32(exec + 16);
  uint32_t length = ctx.mmu.read32(exec + 20);
  uint32_t transferred = ctx.mmu.read32(exec + 32);
  uint32_t dest = ctx.mmu.read32(exec + 24);
  uint32_t cb = ctx.mmu.read32(exec + 40);
  if (length == 0 || length > 0x2000000u || transferred > length) return;
  uint32_t todo = length - transferred;
  uint32_t dst = dest + transferred;
  if (todo == 0 || dst < 0x80000000u || dst + todo < dst || dst + todo > 0x94000000u)
    return;

  uint64_t byte_off = ((uint64_t)word_off << 2) + transferred;
  auto &vd = VirtualDisc::get();
  if (!vd.valid())
    vd.init(Config::get().game_dir);
  if (!vd.valid()) return;

  size_t got = 0;
  {
    std::vector<uint8_t> tmp(todo);
    got = vd.read(byte_off, todo, tmp.data());
    if (got == 0) {
      static int misses = 0;
      if (misses++ < 4)
        std::cout << "[DVD] read miss off=0x" << std::hex << byte_off
                  << " len=0x" << todo << std::dec << std::endl;
      return;
    }
    for (uint32_t i = 0; i < todo; ++i)
      ctx.mmu.write8(dst + i, tmp[i]);
  }

  ctx.mmu.write32(exec + 32, length);
  ctx.mmu.write32(exec + 28, todo);
  ctx.mmu.write32(exec + 12, 0);
  ctx.mmu.write32((uint32_t)(r13 - 26320), 0);
  release_di_request_slots(ctx);

  uint32_t obj = ctx.gpr[31];
  if (obj >= 0x80000000u && obj < 0x94000000u) {
    uint32_t count = ctx.mmu.read32(obj + 8);
    uint32_t ents = ctx.mmu.read32(obj + 12);
    uint32_t flags = ctx.mmu.read32(obj + 20);
    if (count > 0 && count < 4096 &&
        ents >= 0x80000000u && ents < 0x94000000u &&
        flags >= 0x80000000u && flags < 0x94000000u) {
      for (uint32_t i = 0; i < count; ++i) {
        if (ents + i * 60u == exec) {
          ctx.mmu.write8(flags + i, 0);
          break;
        }
      }
    }
  }

  std::cout << "[DVD] completed read off=0x" << std::hex << byte_off
            << " len=0x" << todo << " dst=0x" << dst
            << " covered=0x" << got << " cb=0x" << cb << std::dec << std::endl;

  if (!valid_callback(cb)) return;
  uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
  if (current_ctx == 0) return;

  hle_save_context_to_guest(ctx, current_ctx);
  ctx.dispatch_saved_ctx = current_ctx;
  ctx.srr0 = ctx.pc;
  ctx.srr1 = ctx.msr;
  ctx.msr &= ~0x8000u;
  ctx.gpr[3] = 0;
  ctx.gpr[4] = exec;
  ctx.lr = 0xFFFFFFFC;
  ctx.pc = cb;
  longjmp(ctx.exception_jmp_buf, 1);
}

// DVDReadPrio sleeps on the queue at r13-26488 until its command block
// state drops to 0. The recompiled DI path never finishes that command, so
// the worker that must set the home-button byte stays WAITING. Copy the
// read from the disc, mark the block done, and put the sleepers back on
// the run queue. yield_to_ready_thread then runs them.
static void service_sdk_dvd_sync(CPUContext &ctx) {
  if (ctx.in_callback) return;
  uint32_t r13 = ctx.gpr[13];
  if (r13 < 0x80000000u || r13 >= 0x81800000u) return;

  const uint32_t syncq = r13 - 26488u;
  uint32_t sleeper = 0;
  uint32_t th = ctx.mmu.read32(0x800000DCu);
  for (int guard = 0; th >= 0x80000000u && th < 0x94000000u && guard < 16; ++guard) {
    uint32_t next = ctx.mmu.read32(th + 0x2FC);
    if (ctx.mmu.read16(th + 0x2C8) == 4 && ctx.mmu.read32(th + 0x2DC) == syncq) {
      sleeper = th;
      break;
    }
    th = next;
  }
  if (sleeper == 0) return;

  auto is_read = [&](uint32_t block) -> bool {
    if (block < 0x80000000u || block >= 0x94000000u) return false;
    if (ctx.mmu.read32(block + 8) != 1) return false;
    int32_t state = (int32_t)ctx.mmu.read32(block + 12);
    return state == 1 || state == 2;
  };

  uint32_t exec_addr = r13 - 26320u;
  uint32_t block = ctx.mmu.read32(exec_addr);
  if (!is_read(block)) {
    block = 0;
    constexpr uint32_t kWait = 0x80352870u;
    for (uint32_t p = 0; p < 4 && block == 0; ++p) {
      uint32_t q = kWait + p * 8u;
      uint32_t node = ctx.mmu.read32(q);
      for (int g = 0; node != q && node >= 0x80000000u && node < 0x94000000u && g < 8; ++g) {
        if (is_read(node)) {
          block = node;
          break;
        }
        uint32_t n2 = ctx.mmu.read32(node);
        if (n2 == node) break;
        node = n2;
      }
    }
  }
  if (!is_read(block)) return;

  uint32_t word_off = ctx.mmu.read32(block + 16);
  uint32_t length = ctx.mmu.read32(block + 20);
  uint32_t transferred = ctx.mmu.read32(block + 32);
  uint32_t dest = ctx.mmu.read32(block + 24);
  if (length == 0 || length > 0x2000000u || transferred > length) return;
  uint32_t todo = length - transferred;
  uint32_t dst = dest + transferred;
  if (todo == 0 || dst < 0x80000000u || dst + todo < dst || dst + todo > 0x94000000u)
    return;

  uint64_t byte_off = ((uint64_t)word_off << 2) + transferred;
  auto &vd = VirtualDisc::get();
  if (!vd.valid())
    vd.init(Config::get().game_dir);
  if (!vd.valid()) return;

  size_t got = 0;
  {
    std::vector<uint8_t> tmp(todo);
    got = vd.read(byte_off, todo, tmp.data());
    if (got == 0) {
      static int misses = 0;
      if (misses++ < 4)
        std::cout << "[DVD] sync read miss off=0x" << std::hex << byte_off
                  << " len=0x" << todo << std::dec << std::endl;
      return;
    }
    uint32_t nwrite = (uint32_t)got < todo ? (uint32_t)got : todo;
    for (uint32_t i = 0; i < nwrite; ++i)
      ctx.mmu.write8(dst + i, tmp[i]);
    got = nwrite;
  }

  constexpr uint32_t kWait = 0x80352870u;
  uint32_t nxt = ctx.mmu.read32(block + 0);
  uint32_t prev = ctx.mmu.read32(block + 4);
  auto queued = [](uint32_t a) { return a >= kWait && a < kWait + 4u * 8u; };
  if (queued(nxt) || queued(prev)) {
    if (prev >= 0x80000000u && prev < 0x94000000u)
      ctx.mmu.write32(prev + 0, nxt);
    if (nxt >= 0x80000000u && nxt < 0x94000000u)
      ctx.mmu.write32(nxt + 4, prev);
    ctx.mmu.write32(block + 0, 0);
    ctx.mmu.write32(block + 4, 0);
  }

  ctx.mmu.write32(block + 28, (uint32_t)got);
  ctx.mmu.write32(block + 32, transferred + (uint32_t)got);
  ctx.mmu.write32(block + 12, 0);
  if (ctx.mmu.read32(exec_addr) == block)
    ctx.mmu.write32(exec_addr, 0);
  release_di_request_slots(ctx);

  constexpr uint32_t kRunQ = 0x8034D0F0u;
  uint32_t head = ctx.mmu.read32(syncq);
  for (int guard = 0; head >= 0x80000000u && head < 0x94000000u && head != syncq && guard < 8;
       ++guard) {
    uint32_t n = ctx.mmu.read32(head + 0x2E0);
    ctx.mmu.write32(syncq, n);
    if (n == 0)
      ctx.mmu.write32(syncq + 4, 0);
    else if (n >= 0x80000000u && n < 0x94000000u)
      ctx.mmu.write32(n + 0x2E4, 0);
    ctx.mmu.write32(head + 0x2E0, 0);
    ctx.mmu.write32(head + 0x2E4, 0);
    ctx.mmu.write16(head + 0x2C8, 1);
    uint32_t suspend = ctx.mmu.read32(head + 0x2CC);
    uint32_t prio = ctx.mmu.read32(head + 0x2D0);
    if (suspend == 0 && prio < 32) {
      uint32_t q = kRunQ + prio * 8u;
      uint32_t tail = ctx.mmu.read32(q + 4);
      if (tail == 0)
        ctx.mmu.write32(q, head);
      else if (tail >= 0x80000000u && tail < 0x94000000u)
        ctx.mmu.write32(tail + 0x2E0, head);
      ctx.mmu.write32(head + 0x2E4, tail);
      ctx.mmu.write32(head + 0x2E0, 0);
      ctx.mmu.write32(q + 4, head);
      ctx.mmu.write32(head + 0x2DC, q);
      uint32_t bitmap_addr = r13 - 26624u;
      ctx.mmu.write32(bitmap_addr,
                      ctx.mmu.read32(bitmap_addr) | (1u << (31u - prio)));
      ctx.mmu.write32(r13 - 26628u, 1);
    } else {
      ctx.mmu.write32(head + 0x2DC, 0);
    }
    head = n;
  }

  static int logged = 0;
  if (logged++ < 12)
    std::cout << "[DVD] sync read off=0x" << std::hex << byte_off
              << " len=0x" << todo << " dst=0x" << dst
              << " covered=0x" << got << " woke=0x" << sleeper
              << std::dec << std::endl;
}

// FUN_801d4544 compares extsh(index) with the word at object+24. A count
// above 32767 can never satisfy that compare, so the scan never returns
// to the waiter. The array length written beside it (object+8) is the
// value the same init call stored for the sibling table.
static void clamp_extsh_content_scan(CPUContext &ctx) {
  if (ctx.in_callback || ctx.pc != 0x801D4604u) return;
  uint32_t obj = ctx.gpr[24];
  if (obj < 0x80000000u || obj >= 0x94000000u) return;
  uint32_t count = ctx.mmu.read32(obj + 24);
  static int seen = 0;
  if (seen++ < 2)
    std::cout << "[Scan] obj=0x" << std::hex << obj
              << " count=0x" << count << " cap=0x" << ctx.mmu.read32(obj + 8)
              << " idx=" << std::dec << (int)(int16_t)ctx.gpr[27] << "\n";
  if ((int32_t)count <= 32767) return;
  uint32_t cap = ctx.mmu.read32(obj + 8);
  uint32_t clamped = ((int32_t)cap > 0 && (int32_t)cap <= 32767) ? cap : 0;
  ctx.mmu.write32(obj + 24, clamped);
  ctx.gpr[0] = clamped;
  int32_t idx = (int32_t)(int16_t)ctx.gpr[27];
  int32_t lim = (int32_t)clamped;
  ctx.cr[0].lt = idx < lim;
  ctx.cr[0].gt = idx > lim;
  ctx.cr[0].eq = idx == lim;
  static int n = 0;
  if (n++ < 4)
    std::cout << "[Scan] count 0x" << std::hex << count
              << " clamped to " << std::dec << clamped
              << " idx=" << idx << "\n";
}

// FUN_801e4890 clears byte +266, starts a priority-31 thread to set it,
// then spins while the byte stays 0. OSSleepTicks is supposed to suspend
// the caller, but the caller stays RUNNING, so SelectThread never picks
// the new thread and the byte never changes. The content wait at
// 0x801D4424 / 0x801D452C is the same hole: the loader thread created at
// object+48 stays queued at 0x801D47B8 and the entry state never reaches 5.
// Park the waiter and run the READY thread. The waiter is queued again so
// a later SelectThread resumes it.
static void yield_to_ready_thread(CPUContext &ctx) {
  if (ctx.in_callback) return;
  // 0x801E4930 is the first home-button worker; 0x801E72D0 is the same
  // wait for the worker created at 0x801E72E8. Both retry while eq.
  // 0x801D4424 retries while the entry state is not 5 (!eq).
  // 0x801D452C retries while the scan still found unfinished entries (eq).
  bool byte_wait = ctx.cr[0].eq &&
                   (ctx.pc == 0x801E4930u || ctx.pc == 0x801E72D0u);
  bool content_wait = (ctx.pc == 0x801D4424u && !ctx.cr[0].eq) ||
                      (ctx.pc == 0x801D452Cu && ctx.cr[0].eq);
  if (!byte_wait && !content_wait) return;

  uint32_t cur = ctx.mmu.read32(0x800000E4);
  if (cur < 0x80000000u || cur >= 0x94000000u) return;

  constexpr uint32_t kRunQ = 0x8034D0F0u;
  uint32_t best = 0;
  uint32_t best_prio = 32;
  uint32_t th = ctx.mmu.read32(0x800000DCu);
  for (int guard = 0; th >= 0x80000000u && th < 0x94000000u && guard < 16; ++guard) {
    uint16_t state = ctx.mmu.read16(th + 0x2C8);
    uint32_t prio = ctx.mmu.read32(th + 0x2D0);
    uint32_t next = ctx.mmu.read32(th + 0x2FC);
    if (th != cur && state == 1 && prio < best_prio) {
      best = th;
      best_prio = prio;
    }
    th = next;
  }
  if (best == 0) return;

  uint32_t ctxp = ctx.mmu.read32(0x800000D4);
  if (ctxp == 0) ctxp = cur;
  hle_save_context_to_guest(ctx, ctxp);

  uint32_t r13 = ctx.gpr[13];
  uint32_t prio = ctx.mmu.read32(cur + 0x2D0);
  if (prio < 32 && r13 >= 0x80000000u && r13 < 0x81800000u) {
    uint32_t q = kRunQ + prio * 8u;
    uint32_t queue = ctx.mmu.read32(cur + 0x2DC);
    bool on_runq = queue >= kRunQ && queue < kRunQ + 32u * 8u;
    if (!on_runq) {
      uint32_t tail = ctx.mmu.read32(q + 4);
      if (tail == 0)
        ctx.mmu.write32(q, cur);
      else if (tail >= 0x80000000u && tail < 0x94000000u)
        ctx.mmu.write32(tail + 0x2E0, cur);
      ctx.mmu.write32(cur + 0x2E4, tail);
      ctx.mmu.write32(cur + 0x2E0, 0);
      ctx.mmu.write32(q + 4, cur);
      ctx.mmu.write32(cur + 0x2DC, q);
    }
    ctx.mmu.write16(cur + 0x2C8, 1);
    uint32_t bitmap_addr = r13 - 26624u;
    ctx.mmu.write32(bitmap_addr, ctx.mmu.read32(bitmap_addr) | (1u << (31u - prio)));
  }

  uint32_t q = ctx.mmu.read32(best + 0x2DC);
  uint32_t nxt = ctx.mmu.read32(best + 0x2E0);
  uint32_t prev = ctx.mmu.read32(best + 0x2E4);
  if (q >= kRunQ && q < kRunQ + 32u * 8u) {
    if (prev == 0)
      ctx.mmu.write32(q, nxt);
    else if (prev >= 0x80000000u && prev < 0x94000000u)
      ctx.mmu.write32(prev + 0x2E0, nxt);
    if (nxt == 0)
      ctx.mmu.write32(q + 4, prev);
    else if (nxt >= 0x80000000u && nxt < 0x94000000u)
      ctx.mmu.write32(nxt + 0x2E4, prev);
    if (nxt == 0 && prev == 0 && best_prio < 32 &&
        r13 >= 0x80000000u && r13 < 0x81800000u) {
      uint32_t bitmap_addr = r13 - 26624u;
      ctx.mmu.write32(bitmap_addr,
                      ctx.mmu.read32(bitmap_addr) & ~(1u << (31u - best_prio)));
    }
  }
  ctx.mmu.write32(best + 0x2DC, 0);
  ctx.mmu.write32(best + 0x2E0, 0);
  ctx.mmu.write32(best + 0x2E4, 0);
  ctx.mmu.write16(best + 0x2C8, 2);
  ctx.mmu.write32(0x800000E4, best);
  ctx.mmu.write32(0x800000D4, best);

  static int n = 0;
  if (n++ < 8 || (content_wait && n < 16))
    std::cout << "[Sched] yield waiter 0x" << std::hex << cur
              << " to 0x" << best << " srr0=0x"
              << ctx.mmu.read32(best + 0x198)
              << (content_wait ? " content" : "")
              << std::dec << "\n";

  if (!hle_load_context_from_guest(ctx)) return;
  longjmp(ctx.exception_jmp_buf, 1);
}

// [LDR] NWII_LOADER_DEBUG=1: log every write to a content-loader entry state
// word (entry array base + i*48 + 44) with the writing PC/LR.
extern CPUContext* g_ctx_ptr;
void loader_state_watch(uint32_t addr, uint32_t value, int width) {
  static const bool enabled = std::getenv("NWII_LOADER_DEBUG") != nullptr;
  if (!enabled || width != 32) return;
  if (addr < 0x90000000u || addr >= 0x94000000u) return;
  CPUContext* c = g_ctx_ptr;
  if (!c) return;
  uint32_t ents = c->mmu.read32(0x90000888u + 28);
  if (ents < 0x80000000u || ents >= 0x94000000u) return;
  if (addr < ents || addr >= ents + 256 * 48) return;
  uint32_t off = addr - ents;
  if (off % 48 != 44) return;
  std::cout << "[LDR-ST] entry" << std::dec << (off / 48) << " state<=" << value
            << " pc=0x" << std::hex << c->pc << " lr=0x" << c->lr
            << " r1=0x" << c->gpr[1] << " r3=0x" << c->gpr[3]
            << " r4=0x" << c->gpr[4] << " r5=0x" << c->gpr[5] << std::dec
            << "\n";
}

// [LDR] NWII_LOADER_DEBUG=1: snapshot the content loader, its worker thread
// and the full thread list whenever the entry states or worker status change.
static void debug_loader_snapshot(CPUContext &ctx) {
  static const bool enabled = std::getenv("NWII_LOADER_DEBUG") != nullptr;
  if (!enabled) return;
  if (ctx.pc >= 0x801D412Cu && ctx.pc < 0x801D41C0u) {
    std::cout << "[LDR-CB] pc=0x" << std::hex << ctx.pc
              << " obj=0x" << ctx.gpr[3] << " len=0x" << ctx.gpr[4]
              << " blk=0x" << ctx.gpr[5] << " off=0x" << ctx.gpr[6]
              << " slot=" << std::dec << (int32_t)ctx.gpr[7]
              << " idx=" << ctx.gpr[8] << " lr=0x" << std::hex << ctx.lr
              << " r1=0x" << ctx.gpr[1] << std::dec << "\n";
  }
  static uint64_t last_bucket = 0;
  uint64_t bucket = ctx.inst_count / 1000000;
  if (bucket == last_bucket) return;
  last_bucket = bucket;

  const uint32_t obj = 0x90000888u;
  uint32_t count = ctx.mmu.read32(obj + 24);
  uint32_t ents = ctx.mmu.read32(obj + 28);
  if (count == 0 || count > 256 || ents < 0x80000000u || ents >= 0x94000000u)
    return;
  const uint32_t worker = obj + 48;
  uint16_t wstate = ctx.mmu.read16(worker + 0x2C8);
  uint32_t wsrr0 = ctx.mmu.read32(worker + 0x198);
  uint32_t wqueue = ctx.mmu.read32(worker + 0x2DC);
  uint32_t wr13 = ctx.mmu.read32(worker + 13 * 4);
  bool in_list = false;
  int list_len = 0;
  uint32_t th = ctx.mmu.read32(0x800000DCu);
  for (int g = 0; th >= 0x80000000u && th < 0x94000000u && g < 64; ++g) {
    ++list_len;
    if (th == worker) in_list = true;
    th = ctx.mmu.read32(th + 0x2FC);
  }
  static const char hexd[] = "0123456789abcdef";
  std::string states;
  states.reserve(count);
  for (uint32_t i = 0; i < count; ++i)
    states += hexd[ctx.mmu.read32(ents + i * 48 + 44) & 15];
  uint32_t arg0 = ctx.mmu.read32(0x803D5C88u);
  uint32_t arg4 = ctx.mmu.read32(0x803D5C8Cu);
  uint32_t arg8 = ctx.mmu.read32(0x803D5C90u);
  uint32_t arg12 = ctx.mmu.read32(0x803D5C94u);
  uint32_t arg16 = ctx.mmu.read32(0x803D5C98u);

  static std::string last_key;
  char keybuf[128];
  std::snprintf(keybuf, sizeof(keybuf), "%u|%u|%u|%u|%u|%u|%u",
                (unsigned)wstate, (unsigned)in_list, arg0, arg4, arg8, arg12,
                arg16);
  std::string key = std::string(keybuf) + "|" + states;
  bool changed = key != last_key;
  static uint64_t last_print_bucket = 0;
  if (!changed && bucket - last_print_bucket < 5000) return;
  last_key = key;
  last_print_bucket = bucket;

  uint32_t cur = ctx.mmu.read32(0x800000E4u);
  uint32_t waited = 0xFFFFFFFFu;
  if (ctx.lr == 0x801D43F4u && ctx.gpr[1] >= 0x80000000u)
    waited = ctx.mmu.read32(ctx.gpr[1] + 20) / 48;
  std::cout << "[LDR] inst=" << std::dec << ctx.inst_count
            << " pc=0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
            << " cur=0x" << cur << " r13=0x" << ctx.gpr[13]
            << " waited=" << std::dec << waited
            << " | worker st=" << wstate << " inlist=" << in_list
            << " listlen=" << list_len << " srr0=0x" << std::hex << wsrr0
            << " q=0x" << wqueue << " savr13=0x" << wr13
            << " | arg h=0x" << arg0 << " buf=0x" << arg4 << " sz=0x" << arg8
            << " al=0x" << arg12 << " idxflag=0x" << arg16
            << " | states=" << states << std::dec << "\n";
  if (waited < count) {
    uint32_t e = ents + waited * 48;
    std::cout << "[LDR]   entry" << std::dec << waited << " @0x" << std::hex << e
              << ":";
    for (int i = 0; i < 12; ++i) std::cout << " " << ctx.mmu.read32(e + i * 4);
    std::cout << std::dec << "\n";
    uint32_t cb = ctx.mmu.read32(e);
    if (cb >= 0x80000000u && cb < 0x94000000u) {
      std::cout << "[LDR]   cmdblk@0x" << std::hex << cb << ":";
      for (int i = 0; i < 15; ++i) std::cout << " " << ctx.mmu.read32(cb + i * 4);
      std::cout << std::dec << "\n";
    }
    std::cout << "[LDR]   active(idx:cmd:st):";
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t st = ctx.mmu.read32(ents + i * 48 + 44);
      if (st >= 1 && st <= 4)
        std::cout << " " << std::dec << i << ":0x" << std::hex
                  << ctx.mmu.read32(ents + i * 48) << ":" << st;
    }
    std::cout << std::dec << "\n";
    {
      uint32_t nslot = ctx.mmu.read32(obj + 8);
      uint32_t cmdbase = ctx.mmu.read32(obj + 12);
      uint32_t slotarr = ctx.mmu.read32(obj + 20);
      std::cout << "[LDR]   pool nslot=" << std::dec << nslot
                << " cmdbase=0x" << std::hex << cmdbase
                << " slotarr=0x" << slotarr << " usage:";
      if (slotarr >= 0x80000000u && slotarr < 0x94000000u && nslot < 64)
        for (uint32_t i = 0; i < nslot; ++i)
          std::cout << " " << (int)ctx.mmu.read8(slotarr + i);
      std::cout << std::dec << "\n";
    }
    uint32_t r13 = ctx.gpr[13];
    if (r13 >= 0x80000000u && r13 < 0x81800000u) {
      uint32_t exec = ctx.mmu.read32(r13 - 26320u);
      std::cout << "[LDR]   exec=0x" << std::hex << exec;
      if (exec >= 0x80000000u && exec < 0x94000000u) {
        std::cout << " inUse=" << std::dec << ctx.mmu.read32(exec + 8)
                  << " state=" << (int32_t)ctx.mmu.read32(exec + 12)
                  << " wordOff=0x" << std::hex << ctx.mmu.read32(exec + 16)
                  << " len=0x" << ctx.mmu.read32(exec + 20)
                  << " dst=0x" << ctx.mmu.read32(exec + 24)
                  << " xfer=0x" << ctx.mmu.read32(exec + 32)
                  << " cb=0x" << ctx.mmu.read32(exec + 40);
      }
      std::cout << " slots:";
      for (int i = 0; i < 4; ++i)
        std::cout << " " << (int)ctx.mmu.read8(0x80352B20u + i * 32u + 8u);
      std::cout << std::dec << "\n";
    }
  }
}

bool process_pending_callbacks(CPUContext &ctx) {
  debug_loader_snapshot(ctx);
  input::gip_satisfy_bta_device_up(ctx);
  repair_ready_runqueue(ctx);
  clamp_extsh_content_scan(ctx);
  yield_to_ready_thread(ctx);

  if (std::getenv("NWII_SAMPLE")) {
      static uint32_t last_ee = 0, edges = 0;
      uint32_t ee = ctx.msr & 0x8000;
      if (ee && !last_ee && edges < 8)
          std::cout << "[EEup] #" << ++edges << " pc=0x" << std::hex << ctx.pc
                    << " lr=0x" << ctx.lr << std::dec << "\n";
      last_ee = ee;
  }

  
  if (g_trace_after_dec > 0) {
      static int skip = 20000; 
      if (skip > 0) { skip--; }
      else {
          g_trace_after_dec--;
          std::cout << "[T] 0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                    << " r1=0x" << ctx.gpr[1] << " r3=0x" << ctx.gpr[3]
                    << " r29=0x" << ctx.gpr[29] << " sr0=0x" << ctx.srr0
                    << std::dec << "\n";
      }
  }

  {
    static uint32_t trace_pc = []() -> uint32_t {
      const char *e = std::getenv("NWII_LOOPTRACE");
      return e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0;
    }();
    static uint64_t hits = 0;
    if (trace_pc && ctx.pc == trace_pc) {
      if ((hits++ % 200000) == 0)
        std::cout << "[Loop 0x" << std::hex << trace_pc << "] #" << std::dec
                  << hits << " r3=0x" << std::hex << ctx.gpr[3] << " r4=0x"
                  << ctx.gpr[4] << " r5=0x" << ctx.gpr[5] << " r30=0x"
                  << ctx.gpr[30] << " r31=0x" << ctx.gpr[31] << " lr=0x"
                  << ctx.lr << std::dec << "\n";
    }
  }

  {
    static bool sample = std::getenv("NWII_SAMPLE") != nullptr;
    static uint64_t n = 0;
    if (sample && (++n % 400000) == 0)
      std::cout << "[Sample] pc=0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                << " r3=0x" << ctx.gpr[3] << std::dec << "\n";
  }

  

  
  if (ctx.pc == 0xFFFFFFFC && !ctx.in_callback) {

    

    
    
    static uint32_t g_resched_addr = []() -> uint32_t {
      const char *e = std::getenv("NWII_RESCHED");
      return e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0;
    }();
    if (ctx.ext_resched_pending && g_resched_addr) {
      ctx.ext_resched_pending = false;
      if (std::getenv("NWII_SAMPLE")) {
        static int n = 0;
        if (n++ < 12) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          std::cout << "[Resched] cur=0x" << std::hex << cur
                    << " prio=" << std::dec << ctx.mmu.read32(cur + 0x2D0)
                    << " r1=0x" << std::hex << ctx.gpr[1] << std::dec << "\n";
        }
      }
      ctx.gpr[3] = 0;
      ctx.lr = 0xFFFFFFFC;
      ctx.pc = g_resched_addr;
      return true;
    }
    ctx.ext_resched_pending = false;
    if (ctx.dispatch_saved_ctx) {
      uint32_t current_thread = ctx.mmu.read32(0x800000E4);
      if (current_thread != 0 && current_thread != ctx.dispatch_saved_ctx) {
        ctx.mmu.write32(0x800000D4, current_thread);
      } else {
        ctx.mmu.write32(0x800000D4, ctx.dispatch_saved_ctx);
      }
      ctx.dispatch_saved_ctx = 0;
    }
    if (hle_load_context_from_guest(ctx)) {

      

      
      ctx.dec_written_tb = ctx.read_timebase();
      static uint32_t restore_count = 0;
      restore_count++;
      if (ctx.pc == 0 || (restore_count % 64) == 1) {
          std::cout << "[HLE PI] Context restore #" << std::dec << restore_count
                    << " pc=0x" << std::hex << ctx.pc << " r1=0x" << ctx.gpr[1]
                    << " msr=0x" << ctx.msr << std::dec << "\n";
      }
      return true;
    }
  }

  if ((ctx.inst_count % 20000000) == 0) {
      std::cout << "[Heartbeat] PC: 0x" << std::hex << ctx.pc << " LR: 0x" << ctx.lr
                << " intmr=0x" << nwii::runtime::hw::pi_intmr.load()
                << " intsr=0x" << nwii::runtime::hw::pi_intsr.load()
                << " msr=0x" << ctx.msr
                << " dec_h=0x" << ctx.mmu.read32(0x80003020) << "\n";
      {
          std::cout << "  [ExcTable]";
          for (int i = 0; i < 16; i++) {
              uint32_t h = ctx.mmu.read32(0x80003000 + i * 4);
              if (h) std::cout << " " << i << ":0x" << std::hex << h << std::dec;
          }
          std::cout << "\n";
      }

      
      
      auto dump_thread = [&](uint32_t th, const char *tag) {
          uint16_t state = ctx.mmu.read16(th + 0x2C8);
          uint32_t prio = ctx.mmu.read32(th + 0x2D0);
          uint32_t wqueue = ctx.mmu.read32(th + 0x2DC);
          uint32_t srr0 = ctx.mmu.read32(th + 0x198);
          uint32_t slr = ctx.mmu.read32(th + 0x84);
          std::cout << "  [" << tag << "] 0x" << std::hex << th << " state=" << state
                    << " prio=" << std::dec << prio << " waitq=0x" << std::hex
                    << wqueue << " srr0=0x" << srr0 << " lr=0x" << slr;
          
          uint32_t sp = ctx.mmu.read32(th + 4);
          for (int f = 0; f < 6 && sp != 0 && sp < 0x94000000; ++f) {
              uint32_t ret = ctx.mmu.read32(sp + 4);
              if (ret) std::cout << " <-0x" << std::hex << ret;
              sp = ctx.mmu.read32(sp);
          }
          std::cout << std::dec << "\n";
      };
      
      {
          static uint32_t peek_addr = 0, peek_words = 8;
          static bool peek_parsed = false;
          if (!peek_parsed) {
              peek_parsed = true;
              if (const char *env = std::getenv("NWII_PEEK")) {
                  peek_addr = (uint32_t)std::strtoul(env, nullptr, 16);
                  if (const char *c = std::strchr(env, ','))
                      peek_words = (uint32_t)std::strtoul(c + 1, nullptr, 10);
              }
          }
          if (peek_addr) {
              std::cout << "  [Peek 0x" << std::hex << peek_addr << "]";
              for (uint32_t i = 0; i < peek_words; i++)
                  std::cout << " " << ctx.mmu.read32(peek_addr + i * 4);
              std::cout << std::dec << "\n";
          }
      }
      uint32_t cur = ctx.mmu.read32(0x800000E4);
      if (cur) dump_thread(cur, "CurThread");
      uint32_t th = ctx.mmu.read32(0x800000DC);
      int guard = 0;
      while (th != 0 && guard++ < 16) {
          if (th != cur) dump_thread(th, "Thread");
          th = ctx.mmu.read32(th + 0x2FC);
      }
  }

  
  
  if (ctx.pc == 0) {
      static uint64_t idle_hits = 0;
      if (++idle_hits == 200000) {
          std::cout << "[IDLE] no runnable thread. intmr=0x" << std::hex
                    << nwii::runtime::hw::pi_intmr << " intsr=0x"
                    << nwii::runtime::hw::pi_intsr << " dec_armed="
                    << std::dec << ctx.dec_irq_pending << "\n";
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  [IdleThread] 0x" << std::hex << th
                        << (th == cur ? "*" : " ") << " state=" << std::dec
                        << state << " prio=" << prio << " waitq=0x" << std::hex
                        << wq << " srr0=0x" << srr0 << std::dec << "\n";
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
  }

  if (g_ipc_interrupt_delay > 0) {
      g_ipc_interrupt_delay--;
      if (g_ipc_interrupt_delay == 0) {
          nwii::runtime::hw::trigger_pi_interrupt(0x00004000);
          if (std::getenv("NWII_SAMPLE"))
              std::cout << "[IPC] irq raised intmr=0x" << std::hex
                        << nwii::runtime::hw::pi_intmr << " msr=0x" << ctx.msr
                        << " pc=0x" << ctx.pc << std::dec << "\n";
      }
  }

  
  nwii::runtime::hw::di_tick();

  if (ctx.vblank_pending) {
      if (!ctx.in_callback && (ctx.msr & 0x8000)) {
          ctx.vblank_pending = false;
          nwii::runtime::hw::vi_trigger_interrupt();

          
          
          hle_drive_thread_queue(ctx);
      }
  }

  if (ctx.in_callback && ctx.pc == 0xFFFFFFFC) {
    
    ctx.in_callback = false;
  }

  if (ctx.in_callback) {
    if (std::getenv("NWII_SAMPLE") && (nwii::runtime::hw::pi_intsr & 0x4000)) {
        static uint64_t n = 0;
        if ((n++ % 100000) == 0)
            std::cout << "[IPC] irq pending but in_callback pc=0x" << std::hex
                      << ctx.pc << " lr=0x" << ctx.lr << " depth="
                      << std::dec << ctx.callback_depth << "\n";
    }
    return false;
  }

  // Periodically raise VI (PI_INTSR bit 8 = 0x100, Dolphin INT_CAUSE_VI)

  

  

  

  

  
  
  {
      static bool vi_registered = false;
      if (!vi_registered) {
          vi_registered = true;

          
          uint64_t vi_period = ctx.tb_freq / 60;
          if (vi_period == 0) vi_period = 500000;
          nwii::runtime::EventScheduler::get().schedule_recurring(
              vi_period, [](CPUContext& c, uint64_t) {

                  
                  
                  if ((nwii::runtime::hw::pi_intmr & 0x100) && !c.in_callback &&
                      !(nwii::runtime::hw::pi_intsr & 0x100) &&
                      (c.dispatch_saved_ctx == 0 || c.mmu.read32(0x800000E4) == 0)) {
                      nwii::runtime::hw::vi_trigger_interrupt();
                  }
              });
      }
      nwii::runtime::EventScheduler::get().advance(ctx, ctx.read_timebase());
  }

  
  if (ctx.dec_expired() && (ctx.msr & 0x8000) && !ctx.in_callback) {
      ctx.dec_irq_pending = false; 
      if (std::getenv("NWII_SAMPLE")) {
          static uint32_t df = 0;
          if (df++ < 12)
              std::cout << "[DECfire] #" << df << " pc=0x" << std::hex << ctx.pc
                        << " lr=0x" << ctx.lr << " inst=" << std::dec
                        << ctx.inst_count << "\n";
          extern int g_trace_after_dec;
          if (df == 1) g_trace_after_dec = 300;
      }

      

      
      {
          static uint32_t dec_same_pc_count = 0;
          static uint32_t dec_last_pc = 0;
          if (ctx.pc == dec_last_pc) {
              dec_same_pc_count++;
          } else {
              dec_same_pc_count = 0;
              dec_last_pc = ctx.pc;
          }

          if (dec_same_pc_count == 64) {
              dec_same_pc_count = 0;
              // OSAlarm: handler is +0, fire time is +8. +0x18 is the period,
              // and treating it as a handler matched 0x80000000 and then
              // returned before the real decrementer, so a runnable thread
              // never left the idle loop.
              if (ctx.gpr[13] != 0) {
                  uint64_t now_tb = ctx.read_timebase();
                  uint32_t now_hi = (uint32_t)(now_tb >> 32);
                  uint32_t now_lo = (uint32_t)(now_tb & 0xFFFFFFFF);
                  uint32_t base = ctx.gpr[13];
                  for (int i = 0; i < 512; i++) {
                      uint32_t addr = base - (uint32_t)(i * 4);
                      if (addr < 0x80000000u || addr > 0x81FFFFFFu) continue;
                      uint32_t handler = ctx.mmu.read32(addr + 0x00);
                      if (handler < 0x80004000u || handler >= 0x81800000u) continue;
                      uint32_t link = ctx.mmu.read32(addr + 0x10);
                      uint32_t next = ctx.mmu.read32(addr + 0x14);
                      auto ptr_ok = [](uint32_t p) {
                          return p == 0 || (p >= 0x80000000u && p < 0x81800000u);
                      };
                      if (!ptr_ok(link) || !ptr_ok(next)) continue;
                      uint32_t fire_hi = ctx.mmu.read32(addr + 0x08);
                      uint32_t fire_lo = ctx.mmu.read32(addr + 0x0C);
                      if (fire_hi == 0 && fire_lo == 0) continue;
                      if (fire_hi < now_hi || (fire_hi == now_hi && fire_lo <= now_lo))
                          continue;
                      ctx.mmu.write32(addr + 0x08, now_hi);
                      ctx.mmu.write32(addr + 0x0C, now_lo);
                      static uint32_t forced_count = 0;
                      if ((forced_count++ % 32) == 0)
                          std::cout << "[HLE DEC] force-expired alarm 0x"
                                    << std::hex << addr << " handler=0x"
                                    << handler << std::dec << "\n";
                  }
              }
          }
      }

      
      static uint32_t dcount = 0;
      if ((dcount++ % 500) == 0) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          std::cout << "[Sched] cur=0x" << std::hex << cur << std::dec << "\n";
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  th=0x" << std::hex << th << (th==cur?"*":" ")
                        << " state=" << std::dec << state << " prio=" << prio
                        << " waitq=0x" << std::hex << wq << " srr0=0x" << srr0
                        << std::dec << "\n";

              

              if (wq >= 0x80000000u && wq < 0x81800000u &&
                  std::getenv("NWII_SAMPLE")) {
                  std::cout << "    wq[-2..5]:" << std::hex;
                  for (int i = -2; i < 6; ++i)
                      std::cout << " " << ctx.mmu.read32(wq + i * 4);
                  std::cout << std::dec << "\n";
              }
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
      uint32_t dec_handler = ctx.mmu.read32(0x80003000 + 8 * 4);
      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
      if (dec_handler != 0 && dec_handler != 0xFFFFFFFF && current_ctx != 0) {
          hle_save_context_to_guest(ctx, current_ctx);
          ctx.dispatch_saved_ctx = current_ctx;
          ctx.srr0 = ctx.pc;
          ctx.srr1 = ctx.msr;
          ctx.msr &= ~0x8000;
          ctx.gpr[3] = 8;           
          ctx.gpr[4] = current_ctx; 

          
          
          ctx.lr = 0xFFFFFFFC;
          ctx.pc = dec_handler;
          longjmp(ctx.exception_jmp_buf, 1);
      }
  }

  uint32_t active_ints = nwii::runtime::hw::pi_intsr & nwii::runtime::hw::pi_intmr;

  
  if (active_ints != 0 && (ctx.msr & 0x8000)) {

      

      
      
      bool sched_disabled = false;
      uint32_t r13 = ctx.gpr[13];
      uint32_t lock_off = nwii::runtime::Config::get().game_profile.sched_lock_offset;
      if (lock_off != 0 && r13 >= 0x80200000u && r13 < 0x81800000u) {
          uint32_t lock_addr = r13 - lock_off;
          if (lock_addr >= 0x80000000u && lock_addr < 0x82000000u) {
              uint32_t lock_val = ctx.mmu.read32(lock_addr);

              sched_disabled = (lock_val > 0 && lock_val < 256);
          }
      }
      bool handler_in_flight = (ctx.dispatch_saved_ctx != 0);

      

      
      bool use_new_path =
          (nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch != 0);

      

      
      bool idle = (ctx.mmu.read32(0x800000E4) == 0);

      

      

      if (use_new_path && (handler_in_flight || (!idle && sched_disabled))) {
          
          if (std::getenv("NWII_SAMPLE")) {
              static uint64_t defer_count = 0;
              if ((++defer_count % 500000) == 0)
                  std::cout << "[HLE PI] int deferred: sched_disabled="
                            << sched_disabled << " in_flight=" << handler_in_flight
                            << " pc=0x" << std::hex << ctx.pc << std::dec << "\n";
          }
      } else {

          
          static uint32_t s_dispatch_addr = 0xFFFFFFFF; 
          if (s_dispatch_addr == 0xFFFFFFFF) {
              
              uint32_t from_cfg = nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch;
              if (from_cfg != 0) {
                  s_dispatch_addr = from_cfg;
                  std::cout << "[HLE PI] __OSDispatchInterrupt from config: 0x"
                            << std::hex << s_dispatch_addr << std::dec << "\n";
              } else {

                  
                  s_dispatch_addr = 0;
              }
          }

          if (s_dispatch_addr != 0) {

              

              

              

              

              ctx.srr0 = ctx.pc;
              ctx.srr1 = ctx.msr;

              
              
              uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
              if (current_ctx == 0) {
                  current_ctx = ctx.mmu.read32(0x800000D0);
              }
              if (current_ctx == 0) {
                  std::cout << "[HLE PI] FATAL: current_ctx is 0! pc=0x" << std::hex << ctx.pc << " intmr=0x" << nwii::runtime::hw::pi_intmr << " intsr=0x" << nwii::runtime::hw::pi_intsr << std::dec << "\n";
              }

              
              
              if (current_ctx != 0) {
                  hle_save_context_to_guest(ctx, current_ctx);
              }

              
              ctx.gpr[3] = 0x04;
              
              ctx.gpr[4] = current_ctx;

              
              ctx.msr &= ~0x8000u; 

              
              
              ctx.lr = 0xFFFFFFFC;
              ctx.pc = s_dispatch_addr;

              if (std::getenv("NWII_SAMPLE")) {
                  static uint32_t dc = 0;
                  if ((dc++ % 1000) == 0)
                      std::cout << "[HLE PI] -> __OSDispatchInterrupt #" << std::dec
                                << dc << " pc=0x" << std::hex << ctx.pc
                                << " cur=0x" << ctx.mmu.read32(0x800000E4)
                                << std::dec << "\n";
              }

              longjmp(ctx.exception_jmp_buf, 1);

          } else {

              
              int os_intr = -1;
              uint32_t bit_to_clear = 0;
              
              if      (active_ints & 0x00004000) { os_intr = 27; bit_to_clear = 0x00004000; } 
              else if (active_ints & 0x00000100) { os_intr = 24; bit_to_clear = 0x00000100; } 
              else if (active_ints & 0x00000004) { os_intr = 21; bit_to_clear = 0x00000004; } 
              else if (active_ints & 0x00000008) { os_intr = 20; bit_to_clear = 0x00000008; } 
              else if (active_ints & 0x00000010) { os_intr =  9; bit_to_clear = 0x00000010; } 

              else if (active_ints & 0x00000040) { os_intr = nwii::runtime::hw::dsp_pending_os_interrupt(); bit_to_clear = 0x00000040; } 
              else if (active_ints & 0x00000020) { os_intr =  8; bit_to_clear = 0x00000020; } 
              else if (active_ints & 0x00000400) { os_intr = 19; bit_to_clear = 0x00000400; } 
              else if (active_ints & 0x00000200) { os_intr = 18; bit_to_clear = 0x00000200; } 
              else if (active_ints & 0x00000800) { os_intr = 17; bit_to_clear = 0x00000800; } 

              if (os_intr != -1) {
                  uint32_t handler = ctx.mmu.read32(0x80003040 + os_intr * 4);

                  if (handler != 0 && handler != 0xFFFFFFFF) {
                      if (std::getenv("NWII_SAMPLE") && os_intr == 24) {
                          static int vn = 0;
                          if (vn++ < 5)
                              std::cout << "[VIdisp] #" << vn << " handler=0x"
                                        << std::hex << handler << " pc=0x"
                                        << ctx.pc << std::dec << "\n";
                      }
                      
                      static uint32_t vi_dispatch_count = 0;
                      if (os_intr != 24 && (vi_dispatch_count++ % 256) == 0) {
                          std::cout << "[HLE PI] Dispatching interrupt "
                                    << std::dec << os_intr << " to handler 0x"
                                    << std::hex << handler << " r1=0x"
                                    << ctx.gpr[1] << " pc=0x" << ctx.pc
                                    << std::dec << std::endl;
                      }
                      if (std::getenv("NWII_SAMPLE")) {
                          static uint64_t census[32] = {0};
                          static uint64_t total = 0;
                          if (os_intr >= 0 && os_intr < 32) census[os_intr]++;
                          if ((++total % 2000) == 0) {
                              std::cout << "[IntCensus]";
                              for (int i = 0; i < 32; ++i)
                                  if (census[i])
                                      std::cout << " " << std::dec << i
                                                << ":" << census[i];
                              std::cout << "\n";
                          }
                      }
                      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
                      if (current_ctx == 0) {
                          
                          if (std::getenv("NWII_SAMPLE")) {
                              static int n = 0;
                              if (n++ < 4)
                                  std::cout << "[HLE PI] int " << std::dec
                                            << os_intr << " deferred: no current context\n";
                          }
                      } else {
                          
                          nwii::runtime::hw::pi_intsr &= ~bit_to_clear;
                          hle_save_context_to_guest(ctx, current_ctx);
                          ctx.dispatch_saved_ctx = current_ctx;
                          ctx.srr0 = ctx.pc;
                          ctx.srr1 = ctx.msr;
                          ctx.msr &= ~0x8000u; 
                          ctx.gpr[3] = os_intr;
                          ctx.gpr[4] = current_ctx;
                          ctx.lr = 0xFFFFFFFC;
                          ctx.pc = handler;
                          ctx.ext_resched_pending = true;
                          longjmp(ctx.exception_jmp_buf, 1);
                      }

                      ctx.msr &= ~0x8000u;
                      CPUContext::BackupState bk;
                      bk.gpr = ctx.gpr;
                      bk.fpr = ctx.fpr;
                      bk.ps1 = ctx.ps1;
                      bk.cr  = ctx.cr;
                      bk.lr  = ctx.lr;
                      bk.ctr = ctx.ctr;
                      bk.xer = ctx.xer;
                      bk.pc  = ctx.pc;
                      bk.srr0 = ctx.srr0;
                      bk.srr1 = ctx.srr1;
                      bk.msr = ctx.msr | 0x8000;
                      bk.fpscr = ctx.fpscr;
                      bk.gqr = ctx.gqr;
                      bk.sprg = ctx.sprg;
                      ctx.backup_stack.push(bk);

                      ctx.in_callback = true;
                      ctx.callback_depth++;
                      ctx.srr0 = ctx.pc;
                      ctx.srr1 = ctx.msr | 0x8000;
                      ctx.gpr[3] = os_intr;
                      ctx.gpr[4] = 0;
                      ctx.lr = 0xFFFFFFFC;
                      ctx.pc = handler;
                      longjmp(ctx.exception_jmp_buf, 1);
                  } else {
                      
                      static uint32_t last_null_pc = 0;
                      static int null_warn_count = 0;
                      if (ctx.pc != last_null_pc) {
                          null_warn_count = 0;
                          last_null_pc = ctx.pc;
                      }
                      if (null_warn_count++ < 3)
                          std::cout << "[HLE PI] Handler for interrupt "
                                    << std::dec << os_intr
                                    << " is NULL (OS reinit in progress, deferring)."
                                    << std::endl;
                  }
              }
          } 
      } 
  } else if (nwii::runtime::hw::pi_intsr != 0) {
      
  }

  if ((ctx.msr & 0x8000) == 0)
    return false;

  if (ctx.pending_cb_count.load(std::memory_order_relaxed) == 0)
    return false;

  CallbackInfo cb;
  {
    std::lock_guard<std::mutex> lock(ctx.cb_mutex);
    if (ctx.pending_callbacks.empty())
      return false;
    cb = ctx.pending_callbacks.front();
    ctx.pending_callbacks.pop();
    ctx.pending_cb_count.fetch_sub(1, std::memory_order_relaxed);
  }

  std::cout << "[HLE Callback] Dispatching cb=0x" << std::hex << cb.cb_addr
            << " arg1=0x" << cb.arg1 << " arg2=0x" << cb.arg2 << " from PC=0x"
            << ctx.pc << " LR=0x" << ctx.lr << " r1=0x" << ctx.gpr[1]
            << std::dec << std::endl;

  uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
  if (current_ctx != 0) {

    hle_save_context_to_guest(ctx, current_ctx);
    ctx.dispatch_saved_ctx = current_ctx;

    ctx.srr0 = ctx.pc;
    ctx.srr1 = ctx.msr;
    ctx.msr &= ~0x8000; 

    ctx.gpr[3] = cb.arg1;
    ctx.gpr[4] = cb.arg2;

    ctx.lr = 0xFFFFFFFC;
    ctx.pc = cb.cb_addr;
    longjmp(ctx.exception_jmp_buf, 1);
  }

  CPUContext::BackupState bk;
  bk.gpr = ctx.gpr;
  bk.fpr = ctx.fpr;
  bk.ps1 = ctx.ps1;
  bk.cr  = ctx.cr;
  bk.lr  = ctx.lr;
  bk.ctr = ctx.ctr;
  bk.xer = ctx.xer;
  bk.pc  = ctx.pc;
  bk.srr0 = ctx.srr0;
  bk.srr1 = ctx.srr1;
  bk.msr = ctx.msr;
  bk.fpscr = ctx.fpscr;
  bk.gqr = ctx.gqr;
  bk.sprg = ctx.sprg;
  ctx.backup_stack.push(bk);

  ctx.in_callback = true;
  ctx.callback_depth++;

  ctx.gpr[3] = cb.arg1;
  ctx.gpr[4] = cb.arg2;

  uint32_t cb_stack_top = get_callback_stack_top(ctx) - (ctx.callback_depth - 1) * 4096;
  ctx.gpr[1] = cb_stack_top - 16;
  ctx.mmu.write32(ctx.gpr[1], 0);
  ctx.mmu.write32(ctx.gpr[1] + 4, 0xFFFFFFFC);

  ctx.lr = 0xFFFFFFFC;
  ctx.pc = cb.cb_addr;
  longjmp(ctx.exception_jmp_buf, 1);
}

} 
