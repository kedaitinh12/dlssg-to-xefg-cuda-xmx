#pragma once

// Ported from MFG-Unlocker (source/native/adapter_discovery.h, overlay_native.h and overlay_slots.h), MIT licensed, see
// NOTICE.md. Only what the CUDA route uses, the function bodies are MFG-Unlocker's:
//   adapter_discovery: the checks that an NVAPI entry point is driver owned code (System32\nvapi64.dll stub or the
//                      driver store's nvapi64_impl.dll), never a detour
//   device_unwrap:     overlay_native.h's Unwrap with the two slot checks of overlay_slots.h it depends on
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <initializer_list>
#include <iterator>

namespace adapter_discovery
{
inline bool OwnedCode(HMODULE module, const void* code) noexcept
{
    MEMORY_BASIC_INFORMATION memory{};
    if (!code || VirtualQuery(code, &memory, sizeof(memory)) != sizeof(memory)) return false;
    const DWORD protection = memory.Protect & 0xff;
    return memory.AllocationBase == module && memory.Type == MEM_IMAGE && memory.State == MEM_COMMIT
        && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && (protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE);
}

struct ModuleReference
{
    HMODULE module{};
    ModuleReference() noexcept = default;
    ModuleReference(const ModuleReference&) = delete;
    ModuleReference& operator=(const ModuleReference&) = delete;
    ~ModuleReference() { if (module) FreeLibrary(module); }
    bool Acquire(const void* address) noexcept
    {
        return !module && address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(address), &module);
    }
    void Reset() noexcept
    {
        if (module) FreeLibrary(module);
        module = nullptr;
    }
};

// An unusually long or truncated module path is unavailable, never trusted.
constexpr size_t kPathCapacity = 1024;
inline bool CanonicalPath(const wchar_t* path, std::array<wchar_t, kPathCapacity>& canonical) noexcept
{
    const size_t length = path ? wcsnlen_s(path, kPathCapacity) : 0;
    if (length < 3 || length == kPathCapacity || path[1] != L':'
        || (path[2] != L'\\' && path[2] != L'/')) return false;
    // Loaded Windows driver paths need no relative, device, or UNC syntax.
    // Reject component aliases before normalization rather than admitting an
    // unexpected loader spelling beneath a trusted directory by coincidence.
    for (size_t start = 3, end = start; start < length; start = end + 1)
    {
        end = start;
        while (end < length && path[end] != L'\\' && path[end] != L'/') ++end;
        if (end == start || (end - start == 1 && path[start] == L'.')
            || (end - start == 2 && path[start] == L'.' && path[start + 1] == L'.')) return false;
    }
    const DWORD result = GetFullPathNameW(path, static_cast<DWORD>(canonical.size()), canonical.data(), nullptr);
    return result && result < canonical.size();
}

inline bool NvapiOwnerPath(const wchar_t* systemDirectory, const wchar_t* candidate,
    bool systemStubOnly = false) noexcept
{
    std::array<wchar_t, kPathCapacity> system{}, path{};
    if (!CanonicalPath(systemDirectory, system) || !CanonicalPath(candidate, path)) return false;
    const size_t baseLength = wcslen(system.data()), pathLength = wcslen(path.data());
    if (baseLength + 1 >= pathLength || path[baseLength] != L'\\'
        || CompareStringOrdinal(system.data(), static_cast<int>(baseLength), path.data(),
            static_cast<int>(baseLength), TRUE) != CSTR_EQUAL) return false;
    const wchar_t* relative = path.data() + baseLength + 1;
    if (_wcsicmp(relative, L"nvapi64.dll") == 0) return true;
    if (systemStubOnly) return false;
    constexpr wchar_t repository[] = L"DriverStore\\FileRepository\\";
    constexpr size_t prefixLength = std::size(repository) - 1;
    if (wcslen(relative) <= prefixLength
        || _wcsnicmp(relative, repository, prefixLength) != 0) return false;
    const wchar_t* leaf = wcsrchr(relative + prefixLength, L'\\');
    return leaf && leaf > relative + prefixLength && _wcsicmp(leaf + 1, L"nvapi64_impl.dll") == 0;
}

using QueryInterface = void*(__cdecl*)(uint32_t);
inline bool OwnerFactoryMatches(HMODULE owner, uint32_t id, const void* target) noexcept
{
    if (!OwnedCode(owner, target)) return false;
    const auto query = reinterpret_cast<QueryInterface>(GetProcAddress(owner, "nvapi_QueryInterface"));
    return OwnedCode(owner, reinterpret_cast<void*>(query)) && query(id) == target;
}
} // namespace adapter_discovery

namespace device_unwrap
{
// overlay_slots.h
inline bool ImageSlot(HMODULE image, void** slot) noexcept
{
    MEMORY_BASIC_INFORMATION m{};
    const auto address = reinterpret_cast<uintptr_t>(slot);
    return image && slot && !(address % alignof(void*))
        && VirtualQuery(slot, &m, sizeof(m)) == sizeof(m)
        && m.State == MEM_COMMIT && m.Type == MEM_IMAGE && m.AllocationBase == image
        && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_EXECUTE
            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        && address >= reinterpret_cast<uintptr_t>(m.BaseAddress)
        && m.RegionSize >= sizeof(void*)
        && address - reinterpret_cast<uintptr_t>(m.BaseAddress) <= m.RegionSize - sizeof(void*);
}

inline bool ImageEntry(HMODULE image, void* entry) noexcept
{
    MEMORY_BASIC_INFORMATION m{};
    return image && entry && VirtualQuery(entry, &m, sizeof(m)) == sizeof(m)
        && m.Type == MEM_IMAGE && m.State == MEM_COMMIT && m.AllocationBase == image
        && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        && (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// overlay_native.h
inline bool PinInterface(IUnknown* object, size_t last) noexcept
{
    if (!object || last > 64) return false;
    auto** table = *reinterpret_cast<void***>(object);
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(table), &owner)
        || !ImageSlot(owner, table) || !ImageSlot(owner, table + last)) return false;
    for (size_t i = 0; i <= last; ++i)
    {
        HMODULE callable = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(table[i]), &callable) || !ImageEntry(callable, table[i])) return false;
    }
    return true;
}

// NVIDIA, ReShade and Special K's public COM unwrapping contracts. No private
// object offsets, no replacement of an application's interface, and bounded
// cycle detection.
template<class T> bool Unwrap(IUnknown* input, Microsoft::WRL::ComPtr<T>& output) noexcept
{
    using Microsoft::WRL::ComPtr;
    constexpr GUID baseGuid = {0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
    // ReShade 6.8.0 source/com_utils.hpp: IID_UnwrappedObject. The returned
    // original owns an AddRef, as with any successful QueryInterface call.
    constexpr GUID reshadeGuid = {0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
    // Special K 26.9.25 include/SpecialK/render/dxgi/dxgi_swapchain.h:
    // IID_IUnwrappedDXGISwapChain. Only its swapchain wrapper answers, with its
    // AddRef'd real swapchain; it refuses Streamline's base IID by design and
    // passes other IIDs to that real swapchain.
    constexpr GUID specialKGuid = {0xe8a33b4a,0x1405,0x424c,{0xae,0x88,0x0d,0x3e,0x9d,0x46,0xc9,0x14}};
    output.Reset();
    ComPtr<IUnknown> current = input;
    std::array<ComPtr<IUnknown>, 4> visited;
    for (size_t i = 0; current && i < visited.size(); ++i)
    {
        if (!PinInterface(current.Get(), 2) || FAILED(current.As(&visited[i]))) return false;
        for (size_t j = 0; j < i; ++j) if (visited[j].Get() == visited[i].Get()) return false;
        ComPtr<IUnknown> base;
        HRESULT hr = current->QueryInterface(baseGuid, reinterpret_cast<void**>(base.GetAddressOf()));
        for (const GUID& next : {reshadeGuid, specialKGuid})
        {
            if (hr != E_NOINTERFACE) break;
            if (base) return false;
            hr = current->QueryInterface(next, reinterpret_cast<void**>(base.GetAddressOf()));
        }
        if (hr == E_NOINTERFACE && base) return false;
        if (hr == E_NOINTERFACE) return SUCCEEDED(current.As(&output)) && output;
        if (hr != S_OK || !base) return false;
        current = base;
    }
    return false;
}
} // namespace device_unwrap
