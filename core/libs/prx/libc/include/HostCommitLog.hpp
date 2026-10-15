#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTCOMMITLOG_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTCOMMITLOG_HPP

#include <cstddef>
#include <cstdint>

namespace HostCommitLog {

extern "C" {

bool HostCommitLogEnabled_nid_no_patch();
void HostCommitNote_nid_no_patch(const char* owner, const void* address, std::size_t bytes);
void HostCommitForget_nid_no_patch(const void* address, std::size_t bytes);
// Logical counter for memory the process cannot walk (Vulkan allocations the driver places): bytes and allocation count are added, negative on free.
void HostCommitCount_nid_no_patch(const char* owner, std::int64_t bytes, std::int64_t allocations);

}

}

#endif
