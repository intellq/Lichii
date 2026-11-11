// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Ported from Hookshot (ir_opt/oot_visual_workaround.h, GuardFragmentOutputs).

#pragma once

#include <utility>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {

/// Replaces NaN with 0 and clamps to +-1e30 in every float colour output of a fragment program.
/// One NaN pixel in a scene target spreads through the bloom's chain of averages to the whole
/// picture (the one-frame white flash in Ocarina of Time, with the HUD over it).
/// float_targets: bit i is set when colour target i is a float format.
/// Returns the number of writes changed.
inline size_t GuardFragmentOutputs(IR::Program& program, u32 float_targets) {
    if (program.stage != Stage::Fragment) {
        return 0;
    }
    std::vector<std::pair<IR::Block*, IR::Inst*>> writes;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(0).IsImmediate() &&
                inst.Arg(0).U32() < 8 && ((float_targets >> inst.Arg(0).U32()) & 1) != 0 &&
                !inst.Arg(2).IsImmediate()) {
                writes.emplace_back(block, &inst);
            }
        }
    }
    for (const auto& [block, inst] : writes) {
        IR::IREmitter ir{*block, block->Instructions().iterator_to(*inst)};
        const IR::F32 value{inst->Arg(2)};
        const IR::F32 limited{ir.FPClamp(value, ir.Imm32(-1e30f), ir.Imm32(1e30f))};
        const IR::F32 safe{ir.Select(ir.FPIsNan(value), ir.Imm32(0.0f), limited)};
        inst->SetArg(2, safe);
    }
    return writes.size();
}

} // namespace Shader::Optimization
