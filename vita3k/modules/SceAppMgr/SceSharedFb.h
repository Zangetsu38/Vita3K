// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

struct EmuEnvState;
struct MemState;

void release_shared_fb_process_state(EmuEnvState &emuenv, const MemState &mem);
