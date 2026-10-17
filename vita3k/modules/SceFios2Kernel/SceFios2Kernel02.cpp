// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <module/module.h>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceFios2Kernel);

DECL_EXPORT(int, _sceFiosKernelOverlayGetList, SceUID pid, SceUInt8 min_order, SceUInt8 max_order, Ptr<const void> args);
DECL_EXPORT(int, _sceFiosKernelOverlayResolveWithRangeSync, SceUID pid, int resolveFlag, Ptr<const char> pInPath, Ptr<const void> opt);

EXPORT(int, sceFiosKernelOverlayAddForProcess02) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosKernelOverlayGetInfoForProcess02) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosKernelOverlayGetList02, SceUID pid, SceUInt8 min_order, SceUInt8 max_order, Ptr<const void> args) {
    TRACY_FUNC(sceFiosKernelOverlayGetList02, pid, min_order, max_order, args);
    return CALL_EXPORT(_sceFiosKernelOverlayGetList, pid, min_order, max_order, args);
}

EXPORT(int, sceFiosKernelOverlayGetRecommendedScheduler02) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosKernelOverlayModifyForProcess02) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosKernelOverlayRemoveForProcess02) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosKernelOverlayResolveSync02) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosKernelOverlayResolveWithRangeSync02, SceUID pid, int resolveFlag, Ptr<const char> pInPath, Ptr<const void> opt) {
    TRACY_FUNC(sceFiosKernelOverlayResolveWithRangeSync02, pid, resolveFlag, pInPath, opt);
    return CALL_EXPORT(_sceFiosKernelOverlayResolveWithRangeSync, pid, resolveFlag, pInPath, opt);
}

EXPORT(int, sceFiosKernelOverlayThreadIsDisabled02) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosKernelOverlayThreadSetDisabled02) {
    return UNIMPLEMENTED();
}
