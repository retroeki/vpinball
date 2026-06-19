// license:GPLv3+
//
// Minimal stdafx shim for the standalone whole-patcher corpus harness.
//
// simplescriptpatcher.cpp begins with `#include "stdafx.h"`, which in the app
// is a heavy precompiled header that drags in the entire engine. The patcher
// itself uses NO app types -- only the C++ standard library, plog's logging
// macros (PLOGI / PLOGE), and the RE2 helpers from scriptpatcher_internal.h.
// This shim therefore provides just the standard headers + plog so the file
// compiles in isolation. The logger is never initialized, so plog's IF_PLOG_
// guard makes every PLOGI/PLOGE call a no-op at runtime (see plog/Log.h).
//
// This directory is placed FIRST on the include path (see CMakeLists.txt) so
// `"stdafx.h"` resolves here instead of src/core/stdafx.h.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <plog/Log.h>
