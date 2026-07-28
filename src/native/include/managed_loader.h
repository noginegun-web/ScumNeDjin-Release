#pragma once

#include <string>

namespace warden {

void RunManagedLoaderProbe(const std::wstring& baseDir);
void RunManagedLoaderProbeAsync(const std::wstring& baseDir);

}
