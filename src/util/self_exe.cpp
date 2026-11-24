// util/self_exe.cpp — see the header.

#include <maya/core/target.hpp>
#include "agentty/util/self_exe.hpp"

#if MAYA_OS_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif MAYA_OS_APPLE
#  include <mach-o/dyld.h>
#  include <cstdint>
#  include <vector>
#else
#  include <unistd.h>
#endif

namespace agentty::util {

std::filesystem::path self_exe_path() {
#if MAYA_OS_WINDOWS
    wchar_t buf[32768];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::filesystem::path{std::wstring{buf, n}};
#elif MAYA_OS_APPLE
    std::uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (::_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(buf.data(), ec);
    return ec ? std::filesystem::path{buf.data()} : p;
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::filesystem::path{std::string{buf, static_cast<std::size_t>(n)}};
#endif
}

}  // namespace agentty::util
