#pragma once

#include <cstdint>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>

namespace eot::kernel::memory {

inline void* GuestAddressToHostMutable(uint32_t guest_addr) {
    if (!guest_addr) return nullptr;
    uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
    return rex::memory::GuestPtr(base, guest_addr);
}

inline const void* ToHost(uint8_t* base, uint32_t guest_addr) {
    if (!guest_addr) return nullptr;
    return rex::memory::GuestPtr(base, guest_addr);
}

inline void* ToHostMutable(uint8_t* base, uint32_t guest_addr) {
    if (!guest_addr) return nullptr;
    return rex::memory::GuestPtr(base, guest_addr);
}

inline uint32_t ReadU32(uint8_t* base, uint32_t guest_addr) {
    if (!guest_addr) return 0;
    return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, guest_addr));
}

inline void WriteU32(uint8_t* base, uint32_t guest_addr, uint32_t value) {
    if (!guest_addr) return;
    rex::memory::store_and_swap(rex::memory::GuestPtr(base, guest_addr), value);
}

inline uint8_t ReadU8(uint8_t* base, uint32_t guest_addr) {
    if (!guest_addr) return 0;
    return *rex::memory::GuestPtr(base, guest_addr);
}

inline void WriteU8(uint8_t* base, uint32_t guest_addr, uint8_t value) {
    if (!guest_addr) return;
    *rex::memory::GuestPtr(base, guest_addr) = value;
}

inline int32_t ReadS32(uint8_t* base, uint32_t guest_addr) {
    return static_cast<int32_t>(ReadU32(base, guest_addr));
}

inline void WriteS32(uint8_t* base, uint32_t guest_addr, int32_t value) {
    WriteU32(base, guest_addr, static_cast<uint32_t>(value));
}

}
