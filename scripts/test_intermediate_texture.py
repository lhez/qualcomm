# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

import pytest
import numpy as np

import tvm.testing
from tvm.topi import testing
from tvm.ir.module import IRModule
from tvm import tir
from tvm.script import tir as T


def test_texture_intermediate_te():
    from tvm import te

    A = te.placeholder(shape=(128, 128, 4), dtype="float32")
    B = te.compute((128, 128, 4), lambda i, j, k: A[i, j, k] + 1.0)
    C = te.compute((128, 128, 4), lambda i, j, k: B[i, j, k] * 2.0)
    s = te.create_schedule(C.op)

    target = tvm.target.Target("opencl")

    def schedule_compute_op(op):
        (i, j, k) = s[op].op.axis
        s[op].bind(i, te.thread_axis("blockIdx.x"))
        s[op].bind(j, te.thread_axis("threadIdx.x"))
        s[op].vectorize(k)

    schedule_compute_op(B)
    schedule_compute_op(C)

    A_buffer = tvm.tir.decl_buffer(
        (128, 128, 4),
        "float32",
        scope="global.texture",
    )
    B_buffer = tvm.tir.decl_buffer(
        (128, 128, 4),
        "float32",
        scope="global.texture",
    )
    C_buffer = tvm.tir.decl_buffer(
        (128, 128, 4),
        "float32",
        scope="global.texture",
    )

    print(tvm.lower(s, [A, C]))
    binds = {A: A_buffer, B: B_buffer, C: C_buffer}
    mod = tvm.build(s, [A, C], target=target, binds=binds)
    print(mod.imported_modules[0].get_source())


def test_texture_intermediate_tir():
    @tvm.script.ir_module
    class PlusOneMultTwo:
        @T.prim_func
        def main(a: T.handle, b: T.handle) -> None:
            T.func_attr({"global_symbol": "main", "tir.noalias": True})
            A = T.match_buffer(a, (128, 128, 4), dtype="float32", scope="global.texture")
            # TODO(csullivan) there appears to be a bug in alloc_buffer or handling of global.texture
            # allocation resulting in a segfault from the below line. Need to track this down.
            B = T.alloc_buffer((128, 128, 4), dtype="float32", scope="global")
            C = T.match_buffer(b, (128, 128, 4), dtype="float32", scope="global.texture")
            for block_idx in T.thread_binding(0, 128, thread="blockIdx.x"):
                for thread_idx in T.thread_binding(0, 128, thread="threadIdx.x"):
                    for k in T.serial(4):
                        with T.block("B"):
                            vb, vt, vk = T.axis.remap("SSS", [block_idx, thread_idx, k])
                            B[vb, vt, vk] = A[vb, vt, vk] + T.float32(1)
            for block_idx in T.thread_binding(0, 128, thread="blockIdx.x"):
                for thread_idx in T.thread_binding(0, 128, thread="threadIdx.x"):
                    for k in T.serial(4):
                        with T.block("C"):
                            vb, vt, vk = T.axis.remap("SSS", [block_idx, thread_idx, k])
                            C[vb, vt, vk] = B[vb, vt, vk] * T.float32(2)

    sch = tir.Schedule(PlusOneMultTwo, debug_mask="all")

    def schedule_block(block):
        _, _, inner = sch.get_loops(block)
        sch.vectorize(inner)

    schedule_block(sch.get_block("B"))
    schedule_block(sch.get_block("C"))

    # print(sch.mod["main"].script())
    # print(tvm.lower(sch.mod["main"]))
    target = tvm.target.Target("opencl")
    mod = tvm.build(sch.mod["main"], target=target)
    # print(mod.imported_modules[0].get_source())
