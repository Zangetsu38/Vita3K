// Vita3K emulator project
// Copyright (C) 2018 Vita3K team
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

#include "SceIme.h"

struct SceImeRect {
    SceUInt32 x;
    SceUInt32 y;
    SceUInt32 width;
    SceUInt32 height;
};

struct SceImeEditText {
    SceUInt32 preeditIndex;
    SceUInt32 preeditLength;
    SceUInt32 caretIndex;
    Ptr<SceWChar16> str;
    SceUInt32 editIndex;
    SceInt32 editLengthChange;
};

union SceImeEventParam {
    SceImeRect rect;
    SceImeEditText text;
    SceUInt32 caretIndex;
    SceUChar8 reserved[40];
};

typedef struct SceImeEvent {
    SceUInt32 id;
    SceImeEventParam param;
};

typedef void (*SceImeEventHandler)(void *arg, const SceImeEvent *e);

typedef SceInt32 (*SceImeTextFilter)(
    SceWChar16 *outText,
    SceUInt32 *outTextLength,
    const SceWChar16 *srcText,
    SceUInt32 srcTextLength);

struct SceImeParam {
    SceUInt32 sdkVersion;
    SceUInt32 inputMethod;
    SceUInt64 supportedLanguages;
    SceBool languagesForced;
    SceUInt32 type;
    SceUInt32 option;
    void *work;
    void *arg;
    SceImeEventHandler handler;
    SceImeTextFilter filter;
    Ptr<SceWChar16> initialText;
    SceUInt32 maxTextLength;
    SceWChar16 *inputTextBuffer;
    SceUChar8 enterLabel;
    SceUChar8 reserved[7];
};

EXPORT(int, sceImeClose) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceImeOpen, SceImeParam *param) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceImeSetCaret) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceImeSetPreeditGeometry) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceImeSetText) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceImeUpdate) {
    STUBBED("Hack");
    return 0x80100701;
}

BRIDGE_IMPL(sceImeClose)
BRIDGE_IMPL(sceImeOpen)
BRIDGE_IMPL(sceImeSetCaret)
BRIDGE_IMPL(sceImeSetPreeditGeometry)
BRIDGE_IMPL(sceImeSetText)
BRIDGE_IMPL(sceImeUpdate)
