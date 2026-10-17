#pragma once

#include <util/types.h>

#include <cstddef>

union SceSelfAuthBits {
    SceUInt64 bits[4];
    SceUInt8 bytes[0x20];
};

struct SceSelfAuthSecret {
    SceUInt32 data[4];
};

struct SceSelfAuthInfo {
    SceUInt64 programAuthorityId;
    SceUInt64 spfsoInfo;
    SceSelfAuthBits capability;
    SceSelfAuthBits attribute;
    SceSelfAuthSecret sharedSecret0;
    SceUInt32 klicensee[4];
    SceUInt8 sharedSecret2[0x10];
    SceUInt32 sharedSecret3[4];
};

static_assert(sizeof(SceSelfAuthBits) == 0x20);
static_assert(sizeof(SceSelfAuthSecret) == 0x10);
static_assert(sizeof(SceSelfAuthInfo) == 0x90);
static_assert(offsetof(SceSelfAuthInfo, capability) == 0x10);
static_assert(offsetof(SceSelfAuthInfo, attribute) == 0x30);
