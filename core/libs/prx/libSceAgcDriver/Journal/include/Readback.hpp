#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_READBACK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_JOURNAL_INCLUDE_READBACK_HPP

#include "prx/libSceAgcDriver/Journal/include/Format.hpp"
#include <span>

namespace AgcDriver::Journal {

void Settle(std::span<const Range> ranges);
Frame ReadFrame(const DisplayBuffer& buffer);

}

#endif
