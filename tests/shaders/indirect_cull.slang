// RHI indirect tests (tests/indirect_tests.cpp, rhi.AC1/AC2): `cull` keeps the items whose value is below a
// threshold and compacts their indices, `write_args` turns the survivor count into indirect arguments, and
// `count_threads`, dispatched with those arguments, counts the threads it ran. Layout matches IndirectCullParams.

import veach.bindless;

struct Params {
    DescriptorHandle<StructuredBuffer<uint>> items;        ///< a value per item
    DescriptorHandle<RWStructuredBuffer<uint>> visible;    ///< [0] the survivor count, then their item indices
    DescriptorHandle<StructuredBuffer<uint>> survivors;    ///< the same buffer, read-only
    DescriptorHandle<RWStructuredBuffer<uint>> args;       ///< the indirect records, as uints (see write_args)
    DescriptorHandle<RWStructuredBuffer<uint>> counter;    ///< [0] threads count_threads ran
    uint item_count;
    uint threshold;  ///< an item survives when its value is below this
};
[[vk::push_constant]] ConstantBuffer<Params> params;

[shader("compute")]
[numthreads(64, 1, 1)]
void cull(uint3 id: SV_DispatchThreadID) {
    if (id.x < params.item_count && params.items[id.x] < params.threshold) {
        uint slot;
        InterlockedAdd(params.visible[0], 1u, slot);
        params.visible[1 + slot] = id.x;
    }
}

// The argument buffer, in uints (byte offsets in tests/indirect_tests.cpp):
//   [0, 3)   DispatchIndirectArgs: one group of 8 threads per 8 survivors
//   [4, 8)   DrawIndirectArgs, empty (a multi-draw's first record draws nothing)
//   [8, 12)  DrawIndirectArgs: a 6-vertex quad per survivor
//   [12, 17) DrawIndexedIndirectArgs: a 6-index quad per survivor
[shader("compute")]
[numthreads(1, 1, 1)]
void write_args() {
    const uint n = params.survivors[0];
    params.args[0] = (n + 7) / 8;
    params.args[1] = 1;
    params.args[2] = 1;
    params.args[3] = 0;
    for (uint i = 4; i < 8; ++i) {
        params.args[i] = 0;
    }
    params.args[8] = 6;
    params.args[9] = n;
    params.args[10] = 0;
    params.args[11] = 0;
    params.args[12] = 6;
    params.args[13] = n;
    params.args[14] = 0;
    params.args[15] = 0;
    params.args[16] = 0;
}

[shader("compute")]
[numthreads(8, 1, 1)]
void count_threads() {
    InterlockedAdd(params.counter[0], 1u);
}
