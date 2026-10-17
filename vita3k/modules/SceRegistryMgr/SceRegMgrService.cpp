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

#include <mem/ptr.h>
#include <util/tracy.h>
TRACY_MODULE_NAME(SceRegMgrService);

EXPORT(int, sceRegMgrSrvCnvRegionInt, Ptr<const char> name) {
    TRACY_FUNC(sceRegMgrSrvCnvRegionInt, name);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrSrvCnvRegionPsCode, SceInt32 region) {
    TRACY_FUNC(sceRegMgrSrvCnvRegionPsCode, region);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrSrvCnvRegionStr, SceInt32 region, Ptr<char> buf, SceSize size) {
    TRACY_FUNC(sceRegMgrSrvCnvRegionStr, region, buf.address(), size);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrSrvGetRegion) {
    TRACY_FUNC(sceRegMgrSrvGetRegion);
    return UNIMPLEMENTED();
}

EXPORT(int, sceRegMgrSrvGetRegionStr, Ptr<char> buf, SceSize size) {
    TRACY_FUNC(sceRegMgrSrvGetRegionStr, buf.address(), size);
    return UNIMPLEMENTED();
}
