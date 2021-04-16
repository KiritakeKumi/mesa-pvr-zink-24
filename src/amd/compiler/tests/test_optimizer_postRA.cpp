/*
 * Copyright © 2021 Valve Corporation
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
 */

#include "helpers.h"

using namespace aco;

BEGIN_TEST(optimizer_postRA.vcmp)
    PhysReg reg_v0(256);
    PhysReg reg_s0(0);
    PhysReg reg_s2(2);
    PhysReg reg_s4(4);

    //>> v1: %a:v[0] = p_startpgm
    ASSERTED bool setup_ok = setup_cs("v1", GFX6);
    assert(setup_ok);

    auto &startpgm = bld.instructions->at(0);
    assert(startpgm->opcode == aco_opcode::p_startpgm);
    startpgm->definitions[0].setFixed(reg_v0);

    Temp v_in = inputs[0];

    {
        /* Recognize when the result of VOPC goes to VCC, and use that for the branching then. */

        //! s2: %b:vcc = v_cmp_eq_u32 0, %a:v[0]
        //! s2: %e:s[2-3] = p_cbranch_z %b:vcc
        //! p_unit_test 0, %e:s[2-3]
        auto vcmp = bld.vopc(aco_opcode::v_cmp_eq_u32, bld.def(bld.lm, vcc), Operand(0u), Operand(v_in, reg_v0));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), bld.vcc(vcmp), Operand(exec, bld.lm));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(0, Operand(br, reg_s2));
    }

    //; del b, e

    {
        /* When VCC is overwritten inbetween, don't optimize. */

        //! s2: %b:vcc = v_cmp_eq_u32 0, %a:v[0]
        //! s2: %c:s[0-1], s1: %d:scc = s_and_b64 %b:vcc, %x:exec
        //! s2: %f:vcc = s_mov_b64 0
        //! s2: %e:s[2-3] = p_cbranch_z %d:scc
        //! p_unit_test 1, %e:s[2-3], %f:vcc
        auto vcmp = bld.vopc(aco_opcode::v_cmp_eq_u32, bld.def(bld.lm, vcc), Operand(0u), Operand(v_in, reg_v0));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), bld.vcc(vcmp), Operand(exec, bld.lm));
        auto ovrwr = bld.sop1(Builder::s_mov, bld.def(bld.lm, vcc), Operand(0u));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(1, Operand(br, reg_s2), Operand(ovrwr, vcc));
    }

    //; del b, c, d, e, f

    {
        /* When the result of VOPC goes to an SGPR pair other than VCC, don't optimize */

        //! s2: %b:s[4-5] = v_cmp_eq_u32 0, %a:v[0]
        //! s2: %c:s[0-1], s1: %d:scc = s_and_b64 %b:s[4-5], %x:exec
        //! s2: %e:s[2-3] = p_cbranch_z %d:scc
        //! p_unit_test 2, %e:s[2-3]
        auto vcmp = bld.vopc(aco_opcode::v_cmp_eq_u32, bld.def(bld.lm, reg_s4), Operand(0u), Operand(v_in, reg_v0));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), Operand(vcmp, reg_s4), Operand(exec, bld.lm));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(2, Operand(br, reg_s2));
    }

    //; del b, c, d, e

    {
        /* When the VCC isn't written by VOPC, don't optimize */

        //! s2: %b:vcc, s1: %f:scc = s_or_b64 1, %0:s[4-5]
        //! s2: %c:s[0-1], s1: %d:scc = s_and_b64 %b:vcc, %x:exec
        //! s2: %e:s[2-3] = p_cbranch_z %d:scc
        //! p_unit_test 2, %e:s[2-3]
        auto salu = bld.sop2(Builder::s_or, bld.def(bld.lm, vcc), bld.def(s1, scc), Operand(1u), Operand(reg_s4, bld.lm));
        auto sand = bld.sop2(Builder::s_and, bld.def(bld.lm, reg_s0), bld.def(s1, scc), Operand(salu, vcc), Operand(exec, bld.lm));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, reg_s2), bld.scc(sand.def(1).getTemp()));
        writeout(2, Operand(br, reg_s2));
    }

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.scc_branch_opt)
    //>> s1: %a, s2: %y = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s2", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};
    PhysReg reg_s4{4};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Operand op_in_0(in_0);
    op_in_0.setFixed(reg_s0);
    Operand op_in_1(in_1);
    op_in_1.setFixed(reg_s4);

    {
        //! s1: %d:s[2], s1: %e:scc = s_bfe_u32 %a:s[0], 0x40018
        //! s2: %f:vcc = p_cbranch_nz %e:scc
        //! p_unit_test 0, %f:vcc
        auto salu = bld.sop2(aco_opcode::s_bfe_u32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_0, Operand(0x40018u));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), Operand(salu, reg_s2), Operand(0u));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, vcc), bld.scc(scmp));
        writeout(0, Operand(br, vcc));
    }

    //; del d, e, f

    {
        //! s1: %d:s[2], s1: %e:scc = s_bfe_u32 %a:s[0], 0x40018
        //! s2: %f:vcc = p_cbranch_z %e:scc
        //! p_unit_test 0, %f:vcc
        auto salu = bld.sop2(aco_opcode::s_bfe_u32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_0, Operand(0x40018u));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), Operand(salu, reg_s2), Operand(0u));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, vcc), bld.scc(scmp));
        writeout(0, Operand(br, vcc));
    }

    //; del d, e, f

    {
        //! s1: %d:s[2], s1: %e:scc = s_bfe_u32 %a:s[0], 0x40018
        //! s2: %f:vcc = p_cbranch_z %e:scc
        //! p_unit_test 0, %f:vcc
        auto salu = bld.sop2(aco_opcode::s_bfe_u32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_0, Operand(0x40018u));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), Operand(salu, reg_s2), Operand(0u));
        auto br = bld.branch(aco_opcode::p_cbranch_nz, bld.def(s2, vcc), bld.scc(scmp));
        writeout(0, Operand(br, vcc));
    }

    //; del d, e, f

    {
        //! s1: %d:s[2], s1: %e:scc = s_bfe_u32 %a:s[0], 0x40018
        //! s2: %f:vcc = p_cbranch_nz %e:scc
        //! p_unit_test 0, %f:vcc
        auto salu = bld.sop2(aco_opcode::s_bfe_u32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_0, Operand(0x40018u));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), Operand(salu, reg_s2), Operand(0u));
        auto br = bld.branch(aco_opcode::p_cbranch_nz, bld.def(s2, vcc), bld.scc(scmp));
        writeout(0, Operand(br, vcc));
    }

    //; del d, e, f

    {
        //! s2: %d:s[2-3], s1: %e:scc = s_and_b64 %y:s[4-5], 0x12345
        //! s2: %f:vcc = p_cbranch_z %e:scc
        //! p_unit_test 0, %f:vcc
        auto salu = bld.sop2(aco_opcode::s_and_b64, bld.def(s2, reg_s2), bld.def(s1, scc), op_in_1, Operand(0x12345u));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u64, bld.def(s1, scc), Operand(salu, reg_s2), Operand(0UL));
        auto br = bld.branch(aco_opcode::p_cbranch_nz, bld.def(s2, vcc), bld.scc(scmp));
        writeout(0, Operand(br, vcc));
    }

    //; del d, e, f

    {
        /* SCC is overwritten in between, don't optimize */

        //! s1: %d:s[2], s1: %e:scc = s_bfe_u32 %a:s[0], 0x40018
        //! s1: %h:s[3], s1: %x:scc = s_add_u32 %a:s[0], 1
        //! s1: %g:scc = s_cmp_eq_u32 %d:s[2], 0
        //! s2: %f:vcc = p_cbranch_z %g:scc
        //! p_unit_test 0, %f:vcc, %h:s[3]
        auto salu = bld.sop2(aco_opcode::s_bfe_u32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_0, Operand(0x40018u));
        auto ovrw = bld.sop2(aco_opcode::s_add_u32, bld.def(s1, reg_s3), bld.def(s1, scc), op_in_0, Operand(1u));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), Operand(salu, reg_s2), Operand(0u));
        auto br = bld.branch(aco_opcode::p_cbranch_z, bld.def(s2, vcc), bld.scc(scmp));
        writeout(0, Operand(br, vcc), Operand(ovrw, reg_s3));
    }

    //; del d, e, f, g, h, x

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_and_eq)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_eq has two temp operands, and definition 0 is used, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_and_b32 %d:s[2], %e:scc
        //! p_unit_test 0, %f:s[3]
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_eq has two temp operands, and definition 1 (SCC) is used, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_and_b32 %d:s[2], %e:scc
        //! p_unit_test 1, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_eq has a const 0 operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 1, %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0
        //! p_unit_test 2, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const 0 operand, and definition 0 is used once, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 1, %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const non-zero operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0, %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0x123
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const 0 operand, and definition 0 is used, and SCC is clobbered,
         * we try to optimize, but have to keep the inserted SCC copy.
         */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 1, %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0
        //! s1: %f:s[3] = p_parallelcopy %e:scc
        //! s1: %h:s[2], s1: %_:scc = s_xor_b32 %b:s[0], %c:s[1]
        //! p_unit_test 5, %f:s[3], %h:s[2]
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        auto sxor = bld.sop2(aco_opcode::s_xor_b32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_1, op_in_2);
        writeout(5, Operand(sand.def(0).getTemp(), reg_s3), Operand(sxor.def(0).getTemp(), reg_s2));
    }

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_and_lg)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_lg has two temp operands, and definition 0 is used once, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], %c:s[1], %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], %c:s[1]
        //! p_unit_test 0, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e

    {
        /* When s_cmp_lg has two temp operands, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], %c:s[1], %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], %c:s[1]
        //! p_unit_test 1, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_lg has a const 0 operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0, %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0
        //! p_unit_test 2, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_lg has a const 0 operand, and definition 0 is used once, optimize */


        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0, %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e

    {
        /* When s_cmp_lg has a const non-zero operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0x123, %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0x123
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_lg has a const 0 operand, and definition 0 is used, and SCC is clobbered,
         * we try to optimize but have to keep the inserted SCC copy.
         */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0, %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0
        //! s1: %f:s[3] = p_parallelcopy %e:scc
        //! s1: %h:s[2], s1: %_:scc = s_xor_b32 %b:s[0], %c:s[1]
        //! p_unit_test 5, %f:s[3], %h:s[2]
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        auto sxor = bld.sop2(aco_opcode::s_xor_b32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_1, op_in_2);
        writeout(5, Operand(sand.def(0).getTemp(), reg_s3), Operand(sxor.def(0).getTemp(), reg_s2));
    }

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_or_eq)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_eq has two temp operands, and definition 0 is used once, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %c:s[1], %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], %c:s[1]
        //! p_unit_test 0, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e

    {
        /* When s_cmp_eq has two temp operands, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %c:s[1], %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], %c:s[1]
        //! p_unit_test 1, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const 0 operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0
        //! p_unit_test 2, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const 0 operand, and definition 0 is used once, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const non-zero operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0x123, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0x123
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const 0 operand, and definition 0 is used, and SCC is clobbered,
         * we try to optimize but have to keep the inserted SCC copy.
         */

        //! s1: %d:s[2] = s_cselect_b32 0, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_eq_u32 %d:s[2], 0
        //! s1: %f:s[3] = p_parallelcopy %e:scc
        //! s1: %h:s[2], s1: %_:scc = s_xor_b32 %b:s[0], %c:s[1]
        //! p_unit_test 5, %f:s[3], %h:s[2]
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_eq_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        auto sxor = bld.sop2(aco_opcode::s_xor_b32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_1, op_in_2);
        writeout(5, Operand(sand.def(0).getTemp(), reg_s3), Operand(sxor.def(0).getTemp(), reg_s2));
    }

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_or_lg)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_lg has two temp operands, and definition 0 is used once, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_or_b32 %d:s[2], %e:scc
        //! p_unit_test 0, %f:s[3]
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_lg has two temp operands, and definition 1 (SCC) is used, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_or_b32 %d:s[2], %e:scc
        //! p_unit_test 1, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_lg has a const 0 operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 1, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0
        //! p_unit_test 2, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_lg has a const 0 operand, and definition 0 is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 1, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, Operand(sand.def(0).getTemp(), reg_s3));
    }

    //; del d, e

    {
        /* When s_cmp_lg has a const non-zero operand, and definition 1 (SCC) is used, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0x123
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, Operand(sand.def(1).getTemp(), scc));
    }

    //; del d, e

    {
        /* When s_cmp_eq has a const 0 operand, and definition 0 is used, and SCC is clobbered,
         * we try to optimize but have to keep the inserted SCC copy.
         */

        //! s1: %d:s[2] = s_cselect_b32 1, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_lg_u32 %d:s[2], 0
        //! s1: %f:s[3] = p_parallelcopy %e:scc
        //! s1: %h:s[2], s1: %_:scc = s_xor_b32 %b:s[0], %c:s[1]
        //! p_unit_test 5, %f:s[3], %h:s[2]
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lg_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        auto sxor = bld.sop2(aco_opcode::s_xor_b32, bld.def(s1, reg_s2), bld.def(s1, scc), op_in_1, op_in_2);
        writeout(5, Operand(sand.def(0).getTemp(), reg_s3), Operand(sxor.def(0).getTemp(), reg_s2));
    }

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_and_lt)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_lt_u has two temp operands, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], %c:s[1], %a:scc
        //! s1: %e:scc = s_cmp_lt_u32 %d:s[2], %c:s[1]
        //! p_unit_test 0, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_u has a const 0 operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0, %a:scc
        //! s1: %e:scc = s_cmp_lt_u32 %d:s[2], 0
        //! p_unit_test 1, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_u has a const non-zero operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0x123, %a:scc
        //! s1: %e:scc = s_cmp_lt_u32 %d:s[2], 0x123
        //! p_unit_test 2, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_i has two temp operands, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], %c:s[1], %a:scc
        //! s1: %e:scc = s_cmp_lt_i32 %d:s[2], %c:s[1]
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_i has a const 0 operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0, %a:scc
        //! s1: %e:scc = s_cmp_lt_i32 %d:s[2], 0
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_i has a const non-zero operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0x123, %a:scc
        //! s1: %e:scc = s_cmp_lt_i32 %d:s[2], 0x123
        //! p_unit_test 5, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(5, bld.scc(sand.def(1).getTemp()));
    }

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_and_ge)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_ge_u has two temp operands, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_ge_u32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_and_b32 %d:s[2], %e:scc
        //! p_unit_test 0, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_ge_i has two temp operands, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_ge_i32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_and_b32 %d:s[2], %e:scc
        //! p_unit_test 1, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_ge_u has a const 0 operand, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_ge_u32 %b:s[0], 0
        //! s1: %f:s[3], s1: %g:scc = s_and_b32 %d:s[2], %e:scc
        //! p_unit_test 2, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_ge_i has a const 0 operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0x80000000, %a:scc
        //! s1: %e:scc = s_cmp_ge_i32 %d:s[2], 0
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_u has a const non-zero operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0, %a:scc
        //! s1: %e:scc = s_cmp_ge_u32 %d:s[2], 0x123
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_i has a const 0x80000000 (INT32_MIN) operand, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_ge_i32 %b:s[0], 0x80000000
        //! s1: %f:s[3], s1: %g:scc = s_and_b32 %d:s[2], %e:scc
        //! p_unit_test 5, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), op_in_1, Operand(0x80000000u));
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(5, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_ge_u has a const non-zero 1st operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], -1, %a:scc
        //! s1: %e:scc = s_cmp_le_u32 %d:s[2], 0x123
        //! p_unit_test 6, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), Operand(0x123u), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(6, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_u has a const 0 1st operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], -1, %a:scc
        //! s1: %e:scc = s_cmp_le_u32 %d:s[2], 0
        //! p_unit_test 7, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), Operand(0u), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(7, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_i has a const non-zero 1st operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %b:s[0], 0x7fffffff, %a:scc
        //! s1: %e:scc = s_cmp_le_i32 %d:s[2], 0x123
        //! p_unit_test 8, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), Operand(0x123u), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(8, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_i has a const 0x7fffffffu (INT32_MAX) 1st operand, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_le_i32 %b:s[0], 0x7fffffff
        //! s1: %f:s[3], s1: %g:scc = s_and_b32 %d:s[2], %e:scc
        //! p_unit_test 9, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), Operand(0x7fffffffu), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_and_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(9, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_or_lt)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_lt_u has two temp operands, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_lt_u32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_or_b32 %d:s[2], %e:scc
        //! p_unit_test 0, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_lt_i has two temp operands, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_lt_i32 %b:s[0], %c:s[1]
        //! s1: %f:s[3], s1: %g:scc = s_or_b32 %d:s[2], %e:scc
        //! p_unit_test 1, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_lt_u has a const 0 operand, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_lt_u32 %b:s[0], 0
        //! s1: %f:s[3], s1: %g:scc = s_or_b32 %d:s[2], %e:scc
        //! p_unit_test 2, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_lt_i has a const 0 operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0x80000000, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_lt_i32 %d:s[2], 0
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_u has a const non-zero operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_lt_u32 %d:s[2], 0x123
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_i has a const 0x80000000 (INT32_MIN) operand, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_lt_i32 %b:s[0], 0x80000000
        //! s1: %f:s[3], s1: %g:scc = s_or_b32 %d:s[2], %e:scc
        //! p_unit_test 5, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), op_in_1, Operand(0x80000000u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(5, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    {
        /* When s_cmp_lt_u has a const non-zero 1st operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 -1, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_gt_u32 %d:s[2], 0x123
        //! p_unit_test 6, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), Operand(0x123u), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(6, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_u has a const 0 1st operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 -1, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_gt_u32 %d:s[2], 0
        //! p_unit_test 7, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_u32, bld.def(s1, scc), Operand(0u), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(7, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_i has a const non-zero 1st operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0x7fffffff, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_gt_i32 %d:s[2], 0x123
        //! p_unit_test 8, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), Operand(0x123u), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(8, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_lt_i has a const 0x7fffffffu (INT32_MAX) 1st operand, we can't optimize this sequence */

        //! s1: %d:s[2] = p_parallelcopy %a:scc
        //! s1: %e:scc = s_cmp_gt_i32 %b:s[0], 0x7fffffff
        //! s1: %f:s[3], s1: %g:scc = s_or_b32 %d:s[2], %e:scc
        //! p_unit_test 9, %g:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_lt_i32, bld.def(s1, scc), Operand(0x7fffffffu), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(9, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e, f, g

    finish_optimizer_postRA_test();
END_TEST

BEGIN_TEST(optimizer_postRA.shortcircuit_or_ge)
    //>> s1: %a, s1: %b, s1: %c = p_startpgm
    ASSERTED bool setup_ok = setup_cs("s1 s1 s1", GFX6);
    assert(setup_ok);

    PhysReg reg_s0{0};
    PhysReg reg_s1{1};
    PhysReg reg_s2{2};
    PhysReg reg_s3{3};

    Temp in_0 = inputs[0];
    Temp in_1 = inputs[1];
    Temp in_2 = inputs[2];
    Operand op_in_1(in_1);
    Operand op_in_2(in_2);
    op_in_1.setFixed(reg_s0);
    op_in_2.setFixed(reg_s1);

    {
        /* When s_cmp_ge_u has two temp operands, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %c:s[1], %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_ge_u32 %d:s[2], %c:s[1]
        //! p_unit_test 0, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(0, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_u has a const 0 operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_ge_u32 %d:s[2], 0
        //! p_unit_test 1, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(1, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_u has a const non-zero operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0x123, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_ge_u32 %d:s[2], 0x123
        //! p_unit_test 2, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_u32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(2, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_i has two temp operands, optimize */

        //! s1: %d:s[2] = s_cselect_b32 %c:s[1], %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_ge_i32 %d:s[2], %c:s[1]
        //! p_unit_test 3, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), op_in_1, op_in_2);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(3, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_i has a const 0 operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_ge_i32 %d:s[2], 0
        //! p_unit_test 4, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), op_in_1, Operand(0u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(4, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_i has a const non-zero operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0x123, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_ge_i32 %d:s[2], 0x123
        //! p_unit_test 5, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), op_in_1, Operand(0x123u));
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(5, bld.scc(sand.def(1).getTemp()));
    }

    //; del d, e

    {
        /* When s_cmp_ge_i has a const non-zero 1st operand, optimize */

        //! s1: %d:s[2] = s_cselect_b32 0x123, %b:s[0], %a:scc
        //! s1: %e:scc = s_cmp_le_i32 %d:s[2], 0x123
        //! p_unit_test 6, %e:scc
        auto smov = bld.pseudo(aco_opcode::p_parallelcopy, bld.def(s1, reg_s2), bld.scc(in_0));
        auto scmp = bld.sopc(aco_opcode::s_cmp_ge_i32, bld.def(s1, scc), Operand(0x123u), op_in_1);
        auto sand = bld.sop2(aco_opcode::s_or_b32, bld.def(s1, reg_s3), bld.def(s1, scc), Operand(smov, reg_s2), bld.scc(scmp));
        writeout(6, bld.scc(sand.def(1).getTemp()));
    }

    finish_optimizer_postRA_test();
END_TEST
