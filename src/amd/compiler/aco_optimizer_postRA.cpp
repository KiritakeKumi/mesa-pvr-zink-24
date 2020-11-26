/*
 * Copyright © 2020 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Timur Kristóf <timur.kristof@gmail.com
 *
 */

#include "aco_ir.h"

#include <vector>
#include <bitset>
#include <algorithm>

namespace aco {

namespace {

enum pr_opt_label
{
   label_vcc_to_scc,
   num_labels,
};

struct pr_opt_ssa_info
{
   std::bitset<num_labels> labels;
   Instruction *instr = nullptr;
   Block *block = nullptr;
};

struct pr_opt_ctx
{
   Program *program;
   Block *current_block;
   std::vector<uint16_t> uses;
   std::vector<pr_opt_ssa_info> info;
};

void set_label(pr_opt_ctx &ctx, uint32_t tempId, pr_opt_label label, aco_ptr<Instruction> &instr, Block *block)
{
   auto &info = ctx.info[tempId];

   if ((info.instr && info.instr != instr.get()) ||
       (info.block && info.block != block)) {
      info.labels.reset();
   }

   info.labels.set(label);
   info.instr = instr.get();
   info.block = block;
}

void process_instruction(pr_opt_ctx &ctx, aco_ptr<Instruction> &instr)
{
   /* Mark when an instruction converts VCC into SCC */
   if ((instr->opcode == aco_opcode::s_and_b64 || /* wave64 */
        instr->opcode == aco_opcode::s_and_b32) && /* wave32 */
       !instr->operands[0].isConstant() &&
       instr->operands[0].physReg() == vcc &&
       !instr->operands[1].isConstant() &&
       instr->operands[1].physReg() == exec) {
      set_label(ctx, instr->definitions[1].tempId(), label_vcc_to_scc, instr, ctx.current_block);
   }

   /* When consuming an SCC which was converted from VCC in the same block, use VCC directly */
   if (instr->format == Format::PSEUDO_BRANCH &&
       instr->operands.size() == 1 &&
       !instr->operands[0].isConstant() &&
       instr->operands[0].physReg() == scc &&
       ctx.info[instr->operands[0].tempId()].labels.test(label_vcc_to_scc) &&
       ctx.info[instr->operands[0].tempId()].block == ctx.current_block) {
      Instruction *vcc2scc = ctx.info[instr->operands[0].tempId()].instr;
      ctx.uses[instr->operands[0].tempId()]--;
      instr->operands[0] = vcc2scc->operands[0];
   }
}

} /* End of empty namespace */

void optimize_postRA(Program* program)
{
   pr_opt_ctx ctx;
   ctx.program = program;
   ctx.uses = dead_code_analysis(program);
   ctx.info.resize(program->peekAllocationId());

   /* Forward pass
    * Goes through each instruction exactly once, and can transform
    * instructions or adjust the use counts of temps.
    */
   for (auto &block : program->blocks) {
      ctx.current_block = &block;
      for (aco_ptr<Instruction> &instr : block.instructions)
         process_instruction(ctx, instr);
   }

   /* Cleanup pass
    * Gets rid of instructions which are deleted or no longer have
    * any uses.
    */
   for (auto &block : program->blocks) {
      ctx.current_block = &block;
      std::vector<aco_ptr<Instruction>> sel_instr;
      sel_instr.reserve(block.instructions.size());

      for (aco_ptr<Instruction> &instr : block.instructions) {
         if (instr && !is_dead(ctx.uses, instr.get()))
            sel_instr.emplace_back(std::move(instr));
      }

      block.instructions.swap(sel_instr);
   }
}

} /* End of aco namespace */