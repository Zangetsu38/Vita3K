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

#include "private.h"

#include <cpu/state.h>

#include <kernel/state.h>
#include <kernel/thread/thread_state.h>

#include <fmt/format.h>

#include <algorithm>
#include <vector>

namespace gui {

void draw_thread_details_dialog(GuiState &gui, EmuEnvState &emuenv) {
    ThreadStatePtr thread = emuenv.kernel.get_thread(gui.thread_watch_index);
    if (!thread) {
        gui.debug_menu.thread_details_dialog = false;
        return;
    }
    CPUState &cpu = *thread->cpu;

    ImGui::Begin("Thread Viewer", &gui.debug_menu.thread_details_dialog);

    uint32_t pc = cpu.get_pc();
    uint32_t sp = cpu.get_sp();
    uint32_t lr = cpu.get_lr();
    uint32_t registers[13];
    for (size_t a = 0; a < 13; a++)
        registers[a] = cpu.get_reg(static_cast<uint8_t>(a));

    // TODO: Add THUMB/ARM mode viewer. What arch is the cpu currently using?

    ImGui::Text("Name: %s (%x)", thread->name, gui.thread_watch_index);

    ImGui::Text("PC: %08x", pc);
    ImGui::Text("SP: %08x", sp);
    ImGui::Text("LR: %08x", lr);
    ImGui::Text("Executing: %s", cpu.disassemble(pc).c_str());
    ImGui::Separator();
    for (int a = 0; a < 6; a++) {
        ImGui::Text("r%02i: %08x   r%02i: %08x", a, registers[a], a + 6, registers[a + 6]);
    }
    ImGui::Text("r12: %08x", registers[12]);
    ImGui::Separator();
    for (int i = 0; i < 15; i++) {
        ImGui::Text("Stack %d: %08x", i, *Ptr<uint32_t>(sp + i * 4).get(emuenv.mem));
    }

    ImGui::End();
}

void draw_threads_dialog(GuiState &gui, EmuEnvState &emuenv) {
    const auto &viewport = sdl::get_viewport();
    const ImVec2 RES_SCALE(viewport.scale_x, viewport.scale_y);
    const ImVec2 SCALE(RES_SCALE.x * viewport.manual_dpi_scale, RES_SCALE.y * viewport.manual_dpi_scale);
    const ImVec2 viewport_size(viewport.logical_width, viewport.logical_height);
    const ImVec2 window_size(520.f * SCALE.x, 500.f * SCALE.y);
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(
                                viewport.logical_x + (viewport_size.x - window_size.x) / 2.f,
                                viewport.logical_y + (viewport_size.y - window_size.y) / 2.f),
        ImGuiCond_Appearing);
    ImGui::Begin("Threads", &gui.debug_menu.threads_dialog);

    struct ProcessThreads {
        const MemState *memory;
        SceUID pid;
        std::string app_path;
        std::vector<ThreadStatePtr> threads;
    };

    std::vector<ThreadStatePtr> threads;
    {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        threads.reserve(emuenv.kernel.threads.size());
        for (const auto &[id, thread] : emuenv.kernel.threads)
            threads.push_back(thread);
    }

    std::vector<ProcessThreads> processes;
    for (const ThreadStatePtr &thread : threads) {
        auto process = std::find_if(processes.begin(), processes.end(), [&](const ProcessThreads &candidate) {
            return candidate.memory == &thread->cpu->mem;
        });
        if (process == processes.end()) {
            const auto identity = emuenv.kernel.get_process_identity(thread->cpu->mem);
            processes.push_back({ &thread->cpu->mem, identity ? identity->pid : 0,
                identity ? identity->app_path : std::string{}, {} });
            process = std::prev(processes.end());
        }
        process->threads.push_back(thread);
    }

    std::sort(processes.begin(), processes.end(), [](const ProcessThreads &left, const ProcessThreads &right) {
        if (left.pid == 0 || right.pid == 0)
            return right.pid == 0 && left.pid != 0;
        return left.pid < right.pid;
    });

    for (const ProcessThreads &process : processes) {
        const std::string process_name = process.app_path.empty()
            ? fmt::format("Unknown process ({} threads)", process.threads.size())
            : fmt::format("{} (PID {:X}, {} threads)", process.app_path, process.pid, process.threads.size());
        if (ImGui::CollapsingHeader(process_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID(process.memory);
            if (ImGui::BeginTable("##threads", 4,
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 70.f * SCALE.x);
                ImGui::TableSetupColumn("Thread Name", ImGuiTableColumnFlags_WidthStretch, 140.f * SCALE.x);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70.f * SCALE.x);
                ImGui::TableSetupColumn("Stack Pointer", ImGuiTableColumnFlags_WidthFixed, 100.f * SCALE.x);
                ImGui::TableHeadersRow();

                for (const ThreadStatePtr &th_state : process.threads) {
                    std::string run_state;
                    switch (th_state->get_status()) {
                    case ThreadStatus::running:
                        run_state = "Running";
                        break;
                    case ThreadStatus::waiting:
                        run_state = "Waiting";
                        break;
                    case ThreadStatus::dormant:
                        run_state = "Dormant";
                        break;
                    case ThreadStatus::suspended:
                        run_state = "Suspended";
                        break;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(fmt::format("{:08X}", th_state->id).c_str(), false,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        gui.thread_watch_index = th_state->id;
                        gui.debug_menu.thread_details_dialog = true;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(th_state->name);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(run_state.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%08X", th_state->stack.get());
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace gui
