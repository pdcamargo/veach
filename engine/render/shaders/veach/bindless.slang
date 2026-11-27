// Every DescriptorHandle dereference in Veach's shaders goes through getDescriptorFromHandle, so this module is
// imported by all of them (ADR 0027). In debug builds (VEACH_VALIDATE_BINDLESS) a DX12 handle is checked against
// the RHI's table of live descriptors before use; a bad one is counted, the first recorded, and the kind's null
// descriptor used instead, and the RHI reports it as rhi.invalid_descriptor. Metal handles are opaque resource
// IDs there: Metal's own shader validation covers them in debug builds.
#ifdef VEACH_VALIDATE_BINDLESS
[[vk::binding(30, 0)]] ByteAddressBuffer veach_descriptor_table : register(t30);     // u32 header[16], then view slots, then sampler slots
[[vk::binding(31, 0)]] RWByteAddressBuffer veach_descriptor_errors : register(u31);  // count, first-recorded flag, slot, entry, kind

/// The slot kind the RHI records (SlotKind in dx12_device.cpp); 0 = not checked.
uint veach_slot_kind<T : IOpaqueDescriptor>() {
    if (T.kind == DescriptorKind.Texture) {
        return T.descriptorAccess == DescriptorAccess.Read ? 1 : 2;
    }
    if (T.kind == DescriptorKind.StorageBuffer) {
        return T.descriptorAccess == DescriptorAccess.Read ? 3 : 4;
    }
    if (T.kind == DescriptorKind.Sampler) {
        return 5;
    }
    if (T.kind == DescriptorKind.AccelerationStructure) {
        return 6;
    }
    return 0;
}

/// The handle to use: `h` if the table says it names a live descriptor of `kind`, else the kind's null descriptor
/// (after counting and recording the bad one). Plain values only: DXIL cannot keep a resource in a mutable local.
uint2 veach_checked_handle(uint2 h, uint kind) {
    if (kind == 0) {
        return h;
    }
    const uint view_capacity = veach_descriptor_table.Load(0);
    const uint capacity = kind == 5 ? veach_descriptor_table.Load(4) : view_capacity;
    const uint base = 16 + (kind == 5 ? view_capacity : 0);
    const bool in_range = h.x != 0 && h.x < capacity;
    const uint entry = in_range ? veach_descriptor_table.Load((base + h.x) * 4) : 0;
    // The entry is (kind << 24) | generation, and the handle carries the entry it was made with: a freed slot
    // reads 0, a reused one a newer generation, a wrong-kind use a different kind.
    if (entry != 0 && entry == h.y && (entry >> 24) == kind) {
        return h;
    }
    veach_descriptor_errors.InterlockedAdd(0, 1);
    uint previous;
    veach_descriptor_errors.InterlockedCompareExchange(4, 0, 1, previous);
    if (previous == 0) {
        veach_descriptor_errors.Store3(8, uint3(h.x, h.y, kind));
    }
    if (kind > 5) {
        return h;  // no null acceleration structure: counted, used as is
    }
    const uint null_slot = kind == 5 ? 1 : kind;  // the RHI's fixed null descriptors
    return uint2(null_slot, veach_descriptor_table.Load((base + null_slot) * 4));
}
#endif

public export T getDescriptorFromHandle<T>(DescriptorHandle<T> handle) where T : IOpaqueDescriptor {
#ifdef VEACH_VALIDATE_BINDLESS
    __target_switch {
    case hlsl:
        // Every checked handle is marked nonuniform: the check may replace it per lane (debug builds only).
        return defaultGetDescriptorFromHandle(
            nonuniform(DescriptorHandle<T>(veach_checked_handle((uint2)handle, veach_slot_kind<T>()))));
    default:
        return defaultGetDescriptorFromHandle(handle);
    }
#else
    return defaultGetDescriptorFromHandle(handle);
#endif
}
