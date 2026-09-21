// Build the recursive-spinlock stubs without platform dependencies.
// Layout: owner at C, depth at C+4, waiters at C+8, code at C+16.
// went/rent are writer/reader entries; stubOff holds offsets of the two acquire and two release stubs.
// Acquire counts waiters before attempting the lock. Release checks thread ownership.
// The 35-byte release prefix is used to recognize retained exit stubs.
#ifndef CHATSERIAL_CAVE_HPP_INCLUDED
#define CHATSERIAL_CAVE_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

inline void BuildChatLockCave(uintptr_t C, uintptr_t went, uintptr_t rent,
                              std::vector<uint8_t>& code, size_t stubOff[4])
{
    const uintptr_t OWNER = C + 0, COUNT = C + 4, WAITERS = C + 8;
    const uintptr_t CODE = C + 16;
    auto put32 = [&](uint32_t v)
    { code.push_back(v & 0xFF); code.push_back((v>>8)&0xFF); code.push_back((v>>16)&0xFF); code.push_back((v>>24)&0xFF); };

    // Acquire recursively by thread ID, replay the stolen prologue, then return to the function.
    auto emit_acquire = [&](std::initializer_list<uint8_t> repl, uintptr_t back)
    {
        code.push_back(0xf0); code.push_back(0xff); code.push_back(0x05); put32((uint32_t)WAITERS); // lock inc [waiters] -- FIRST
        for (uint8_t x : {0x64,0x8b,0x15,0x24,0x00,0x00,0x00}) code.push_back(x);           // mov edx, fs:[0x24]
        const size_t spin = code.size();
        code.push_back(0x3b); code.push_back(0x15); put32((uint32_t)OWNER);                  // cmp [owner],edx
        const size_t je_at = code.size(); code.push_back(0x74); code.push_back(0x00);        // je .have
        code.push_back(0x33); code.push_back(0xc0);                                          // xor eax,eax
        code.push_back(0xf0); code.push_back(0x0f); code.push_back(0xb1); code.push_back(0x15); put32((uint32_t)OWNER); // lock cmpxchg [owner],edx
        const uint8_t jnz_rel = (uint8_t)((spin - (code.size()+2)) & 0xFF);                  // (rel BEFORE pushing the jnz)
        code.push_back(0x75); code.push_back(jnz_rel);                                       // jnz .spin
        const size_t have = code.size();
        code.push_back(0xff); code.push_back(0x05); put32((uint32_t)COUNT);                  // inc [count]
        code.push_back(0xf0); code.push_back(0xff); code.push_back(0x0d); put32((uint32_t)WAITERS); // lock dec [waiters]
        code[je_at+1] = (uint8_t)((have - (je_at+2)) & 0xFF);                                // fix je .have
        for (uint8_t x : repl) code.push_back(x);                                            // replicated prologue
        code.push_back(0xe9);
        const uintptr_t jp = CODE + code.size() + 4;                                         // addr after the jmp (code at C+16)
        put32((uint32_t)(back - jp));                                                        // jmp back
    };
    // Only the owning thread releases; clear the owner at zero depth, then replay the exit.
    auto emit_release = [&](std::initializer_list<uint8_t> tail)
    {
        code.push_back(0x52);                                                                // push edx
        for (uint8_t x : {0x64,0x8b,0x15,0x24,0x00,0x00,0x00}) code.push_back(x);           // mov edx, fs:[0x24]
        code.push_back(0x3b); code.push_back(0x15); put32((uint32_t)OWNER);                  // cmp edx,[owner]
        code.push_back(0x5a);                                                                // pop edx
        const size_t jne_at = code.size(); code.push_back(0x75); code.push_back(0x00);       // jne .done (not mine)
        code.push_back(0xff); code.push_back(0x0d); put32((uint32_t)COUNT);                  // dec [count]
        const size_t jnz_at = code.size(); code.push_back(0x75); code.push_back(0x00);       // jnz .done (still held)
        code.push_back(0xc7); code.push_back(0x05); put32((uint32_t)OWNER);                  // mov [owner],0
        code.push_back(0); code.push_back(0); code.push_back(0); code.push_back(0);
        const size_t done = code.size();
        code[jne_at+1] = (uint8_t)((done - (jne_at+2)) & 0xFF);                              // fix jne .done
        code[jnz_at+1] = (uint8_t)((done - (jnz_at+2)) & 0xFF);                              // fix jnz .done
        for (uint8_t x : tail) code.push_back(x);
    };

    stubOff[0] = code.size(); emit_acquire({0x81,0xec,0x6c,0x06,0x00,0x00}, went+6);         // sub esp,0x66C ; ->went+6
    stubOff[1] = code.size(); emit_acquire({0x83,0xec,0x20,0x53,0x56}, rent+5);              // sub esp,0x20;push ebx;push esi ; ->rent+5
    stubOff[2] = code.size(); emit_release({0x81,0xc4,0x6c,0x06,0x00,0x00, 0xc2,0x14,0x00}); // add esp,0x66C ; ret 0x14
    stubOff[3] = code.size(); emit_release({0x83,0xc4,0x20, 0xc2,0x08,0x00});                // add esp,0x20 ; ret 8
}

#endif // CHATSERIAL_CAVE_HPP_INCLUDED
