//===----------------------------------------------------------------------===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "kalmar_runtime.h"
#include "kalmar_serialize.h"

#include "../hc2/external/elfio/elfio.hpp"

#include <link.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <utility>

namespace Concurrency
{
    template<int, int, int> class tiled_extent;
    template<int, int, int> class tiled_index;
}

namespace hc
{
    template<int> class tiled_extent;
    template<int> class tiled_index;
}

/** \cond HIDDEN_SYMBOLS */
namespace Kalmar {

template <typename Kernel>
inline
void append_kernel(
  const std::shared_ptr<KalmarQueue>& pQueue, const Kernel& f, void* kernel)
{
  Kalmar::BufferArgumentsAppender vis(pQueue, kernel);
  Kalmar::Serialize s(&vis);
  //f.__cxxamp_serialize(s);
}

// template<typename Kernel>
// inline
// std::shared_ptr<KalmarQueue> get_available_que(const Kernel& f)
// {
//     Kalmar::QueueSearcher ser;
//     Kalmar::Serialize s(&ser);
//     f.__cxxamp_serialize(s);
//     if (ser.get_que())
//         return ser.get_que();
//     else
//         return getContext()->auto_select();
// }

struct Indexer {
    template<int n>
    operator index<n>() const [[hc]]
    {
        int tmp[n]{};
        for (auto i = 0; i != n; ++i) tmp[i] = amp_get_global_id(i);

        return index<n>{tmp};
    }

    template<int... dims>
    operator Concurrency::tiled_index<dims...>() const [[hc]]
    {
        return {};
    }

    template<int n>
    operator hc::tiled_index<n>() const [[hc]]
    {
        return {};
    }
};

template<typename Index, typename Kernel>
struct Kernel_emitter {
    static
    __attribute__((used, annotate("__HCC_KERNEL__")))
    void entry_point(Kernel f) restrict(cpu, amp)
    {
        #if __KALMAR_ACCELERATOR__ != 0
            Index tmp = Indexer{};
            f(tmp);
        #endif
    }
};

template<typename Kernel>
inline
const char* linker_name_for()
{
    static std::once_flag f{};
    static std::string r{};

    // TODO: this should be fused with the one used in mcwamp_hsa.cpp as a
    //       for_each_elf(...) function.
    std::call_once(f, [&]() {
        dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* pr) {
            const auto base = info->dlpi_addr;
            ELFIO::elfio elf;

            if (!elf.load(base ? info->dlpi_name : "/proc/self/exe")) return 0;

            struct Symbol {
                std::string name;
                ELFIO::Elf64_Addr value;
                ELFIO::Elf_Xword size;
                unsigned char bind;
                unsigned char type;
                ELFIO::Elf_Half section_index;
                unsigned char other;
            } tmp{};
            for (auto&& section : elf.sections) {
                if (section->get_type() != SHT_SYMTAB) continue;

                ELFIO::symbol_section_accessor fn{elf, section};

                auto n = fn.get_symbols_num();
                while (n--) {
                    fn.get_symbol(
                      n,
                      tmp.name,
                      tmp.value,
                      tmp.size,
                      tmp.bind,
                      tmp.type,
                      tmp.section_index,
                      tmp.other);

                    if (tmp.type != STT_FUNC) continue;

                    static const auto k_addr =
                        reinterpret_cast<std::uintptr_t>(&Kernel::entry_point);
                    if (tmp.value + base == k_addr) {
                        *static_cast<std::string*>(pr) = tmp.name;

                        return 1;
                    }
                }
            }

            return 0;
        }, &r);
    });

    if (r.empty()) {
        throw std::runtime_error{
            std::string{"Kernel: "} +
            typeid(&Kernel::entry_point).name() +
            " is not available."};
    }

    return r.c_str();
}

template<typename T>
struct Index_type;

template<int n>
struct Index_type<Concurrency::extent<n>> {
    using index_type = index<n>;
};

template<int... dims>
struct Index_type<Concurrency::tiled_extent<dims...>> {
    using index_type = Concurrency::tiled_index<dims...>;
};

template<int n>
struct Index_type<hc::extent<n>> {
    using index_type = index<n>;
};

template<int n>
struct Index_type<hc::tiled_extent<n>> {
    using index_type = hc::tiled_index<n>;
};

template<typename T>
using IndexType = typename Index_type<T>::index_type;

template<typename Domain, typename Kernel>
inline
void* make_registered_kernel(
    const std::shared_ptr<KalmarQueue>& q, const Kernel& f)
{
    using K = Kalmar::Kernel_emitter<IndexType<Domain>, Kernel>;

    void *kernel{CLAMP::CreateKernel(
      linker_name_for<K>(), q.get(), &f, sizeof(Kernel))};
    append_kernel(q, f, kernel);

    return kernel;
}

template<typename T>
constexpr
inline
std::array<std::size_t, T::rank> local_dimensions(const T&)
{
    return std::array<std::size_t, T::rank>{};
}

template<int... dims>
constexpr
inline
std::array<std::size_t, sizeof...(dims)> local_dimensions(
    const Concurrency::tiled_extent<dims...>&)
{
    return std::array<std::size_t, sizeof...(dims)>{dims...};
}

template<int n>
inline
std::array<std::size_t, n> local_dimensions(const hc::tiled_extent<n>& domain)
{
    std::array<std::size_t, n> r{};
    for (auto i = 0; i != n; ++i) r[i] = domain.tile_dim[i];

    return r;
}

template<typename Domain>
inline
std::pair<
    std::array<std::size_t, Domain::rank>,
    std::array<std::size_t, Domain::rank>> dimensions(const Domain& domain)
{
    using R = std::pair<
        std::array<std::size_t, Domain::rank>,
        std::array<std::size_t, Domain::rank>>;

    R r{};
    for (auto i = 0; i != domain.rank; ++i) r.first[i] = domain[i];
    r.second = local_dimensions(domain);

    return r;
}

template<typename Domain, typename Kernel>
inline
std::shared_ptr<KalmarAsyncOp> launch_kernel_async(
    const std::shared_ptr<KalmarQueue>& q,
    const Domain& domain,
    const Kernel& f)
{
  const auto dims{dimensions(domain)};

  return q->LaunchKernelAsync(
      make_registered_kernel<Domain>(q, f),
      Domain::rank,
        dims.first.data(),
        dims.second.data());
}

template<typename Domain, typename Kernel>
inline
void launch_kernel(
    const std::shared_ptr<KalmarQueue>& q,
    const Domain& domain,
    const Kernel& f)
{
    const auto dims{dimensions(domain)};

    q->LaunchKernel(
        make_registered_kernel<Domain>(q, f),
        Domain::rank,
        dims.first.data(),
        dims.second.data());
}

template<typename Domain, typename Kernel>
inline
void launch_kernel_with_dynamic_group_memory(
    const std::shared_ptr<KalmarQueue>& q,
    const Domain& domain,
    const Kernel& f,
    std::size_t dynamic_group_memory_size)
{
    const auto dims{dimensions(domain)};

    q->LaunchKernelWithDynamicGroupMemory(
        make_registered_kernel<Domain>(q, f),
        Domain::rank,
        dims.first.data(),
        dims.second.data(),
        domain.dynamic_group_segment_size());
}

template<typename Domain, typename Kernel>
inline
std::shared_ptr<KalmarAsyncOp> launch_kernel_with_dynamic_group_memory_async(
  const std::shared_ptr<KalmarQueue>& q,
  const Domain& domain,
  const Kernel& f)
{
    const auto dims{dimensions(domain)};

    return q->LaunchKernelWithDynamicGroupMemoryAsync(
        make_registered_kernel<Domain>(q, f),
        Domain::rank,
        dims.first.data(),
        dims.second.data(),
        domain.get_dynamic_group_segment_size());
}
} // namespace Kalmar
/** \endcond */
