#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTCOMMITLOG_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTCOMMITLOG_HPP

#include <cstddef>

namespace HostCommitLog {

extern "C" {

bool HostCommitLogEnabled_nid_no_patch();
void HostCommitNote_nid_no_patch(const char* owner, const void* address, std::size_t bytes);
void HostCommitForget_nid_no_patch(const void* address, std::size_t bytes);

}

}

#endif
