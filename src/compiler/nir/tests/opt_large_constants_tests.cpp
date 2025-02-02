/*
 * Copyright © 2025 Valve Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "nir_test.h"

class nir_large_constants_test : public nir_test {
protected:
   nir_large_constants_test();

   void run_test();

   nir_variable *array;
};

nir_large_constants_test::nir_large_constants_test()
   : nir_test::nir_test("nir_large_constants_test", MESA_SHADER_COMPUTE)
{
}

void
nir_large_constants_test::run_test()
{
   nir_def *index = nir_load_workgroup_index(b);
   nir_def *value = nir_load_array_var(b, array, index);
   nir_use(b, value);

   NIR_PASS(_, b->shader, nir_opt_large_constants, NULL, 0);
   nir_opt_dce(b->shader);
}

TEST_F(nir_large_constants_test, small_int_array)
{
   uint32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_uint_type(), length, 0), "array");
   for (uint32_t i = 0; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_int(b, i), 0x1);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 32
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x76543210 = 1985229328)
          32    %2 = load_const (0x00000002)
          32    %3 = ishl %0, %2 (0x2)
          32    %4 = ushr %1 (0x76543210), %3
          32    %5 = load_const (0x0000000f = 15)
          32    %6 = iand %4, %5 (0xf)
                     @use (%6)
                     // succs: b1
          block b1:
      }
   )"));
}

TEST_F(nir_large_constants_test, small_uint8_t_array)
{
   uint32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_uint8_t_type(), length, 0), "array");
   for (uint32_t i = 0; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_intN_t(b, i, 8), 0x1);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 8
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x76543210 = 1985229328)
          32    %2 = load_const (0x00000002)
          32    %3 = ishl %0, %2 (0x2)
          32    %4 = ushr %1 (0x76543210), %3
          32    %5 = load_const (0x0000000f = 15)
          32    %6 = iand %4, %5 (0xf)
          8     %7 = u2u8 %6
                     @use (%7)
                     // succs: b1
          block b1:
      }
   )"));
}

TEST_F(nir_large_constants_test, small_bool_array)
{
   uint32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_bool_type(), length, 0), "array");
   for (uint32_t i = 0; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_bool(b, i & 1), 0x1);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 32
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x000000aa = 170)
          32    %2 = ushr %1 (0xaa), %0
          32    %3 = load_const (0x00000001)
          32    %4 = iand %2, %3 (0x1)
          32    %5 = load_const (0x00000000)
          1     %6 = ine %4, %5 (0x0)
                     @use (%6)
                     // succs: b1
          block b1:
      }
   )"));
}

TEST_F(nir_large_constants_test, small_uint64_t_array)
{
   uint32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_uint64_t_type(), length, 0), "array");
   for (uint32_t i = 0; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_int64(b, i), 0x1);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 64
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x76543210 = 1985229328)
          32    %2 = load_const (0x00000002)
          32    %3 = ishl %0, %2 (0x2)
          32    %4 = ushr %1 (0x76543210), %3
          32    %5 = load_const (0x0000000f = 15)
          32    %6 = iand %4, %5 (0xf)
          64    %7 = u2u64 %6
                     @use (%7)
                     // succs: b1
          block b1:
      }
   )"));
}

TEST_F(nir_large_constants_test, small_float_natural_numbers_including_zero_array)
{
   uint32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_float_type(), length, 0), "array");
   for (uint32_t i = 0; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_float(b, i), 0x1);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 32
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x76543210 = 1985229328)
          32    %2 = load_const (0x00000002)
          32    %3 = ishl %0, %2 (0x2)
          32    %4 = ushr %1 (0x76543210), %3
          32    %5 = load_const (0x0000000f = 15)
          32    %6 = iand %4, %5 (0xf)
          32    %7 = u2f32 %6
                     @use (%7)
                     // succs: b1
          block b1:
      }
   )"));
}

TEST_F(nir_large_constants_test, small_float_natural_numbers_including_zero_vec_array)
{
   uint32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_vec2_type(), length, 0), "array");
   for (uint32_t i = 0; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_vec2(b, i, length - 1 - i), 0x3);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 64
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x00000000)
          32    %2 = load_const (0x00000003)
          32    %3 = ishl %0, %2 (0x3)
          32    %4 = iadd %1 (0x0), %3
          32x2  %5 = @load_constant (%4) (base=0, range=64, access=none, align_mul=4, align_offset=0)
                     @use (%5)
                     // succs: b1
          block b1:
      }
   )"));
}

TEST_F(nir_large_constants_test, small_float_whole_numbers_array)
{
   int32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_float_type(), length, 0), "array");
   for (int32_t i = 0; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_float(b, i - 4), 0x1);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 32
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x00000000)
          32    %2 = load_const (0x00000002)
          32    %3 = ishl %0, %2 (0x2)
          32    %4 = iadd %1 (0x0), %3
          32    %5 = @load_constant (%4) (base=0, range=32, access=none, align_mul=4, align_offset=0)
                     @use (%5)
                     // succs: b1
          block b1:
      }
   )"));
}

TEST_F(nir_large_constants_test, small_fraction_array)
{
   int32_t length = 8;
   array = nir_local_variable_create(b->impl, glsl_array_type(glsl_float_type(), length, 0), "array");
   for (int32_t i = 0; i < length / 2; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_float(b, i / 2.0 - 2), 0x1);
   for (int32_t i = length / 2; i < length; i++)
      nir_store_array_var_imm(b, array, i, nir_imm_float(b, (i - length / 2) / 3.0), 0x1);

   run_test();

   check_nir_string(NIR_REFERENCE_SHADER(R"(
      shader: MESA_SHADER_COMPUTE
      name: nir_large_constants_test
      workgroup_size: 1, 1, 1
      subgroup_size: 0
      constants: 32
      decl_function main () (entrypoint)

      impl main {
          block b0:  // preds:
          32    %0 = @load_workgroup_index
          32    %1 = load_const (0x00000000)
          32    %2 = load_const (0x00000002)
          32    %3 = ishl %0, %2 (0x2)
          32    %4 = iadd %1 (0x0), %3
          32    %5 = @load_constant (%4) (base=0, range=32, access=none, align_mul=4, align_offset=0)
                     @use (%5)
                     // succs: b1
          block b1:
      }
   )"));
}
