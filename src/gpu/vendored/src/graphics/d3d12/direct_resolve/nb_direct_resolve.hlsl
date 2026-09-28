// nb: direct host render target resolve (src/gpu/vendored/UPSTREAM.md, `direct_host_resolve`).
//
// One compute shader per (resolve copy shader, source render target format and sample count): the
// SDK's own resolve copy shader source, compiled unchanged from the Xenia revision its precompiled
// bytecode was built from (xenia/, commit 04d5c40d), except that every read of the EDRAM buffer is
// answered from the render target through the dump shader's conversion. That removes the dump pass
// (one full read of the render target at every sample and one full write of the EDRAM buffer) from the
// resolve, while each value reaching the copy shader's arithmetic is bit for bit the word the dump
// would have written.
//
// With NB_DIRECT_RESOLVE_VERIFY the shader writes nothing: it compares each value the copy shader
// would store against what the SDK's own dump and copy just stored at that position and counts the
// differences (nb_direct_resolve_verify_frames).
//
// Defined by the C++ side (D3D12RenderTargetCache::GetOrCreateDirectResolvePipeline):
// SHADING_LANGUAGE_HLSL_XE=1, XE_RESOLVE_RESOLUTION_SCALED when scaled,
// NB_DIRECT_RESOLVE_COPY_SHADER - index of the copy shader in the list below,
// NB_DIRECT_RESOLVE_SOURCE_FORMAT - xenos::ColorRenderTargetFormat of the render target,
// NB_DIRECT_RESOLVE_SOURCE_IS_UINT - whether the render target's SRV returns integers,
// NB_DIRECT_RESOLVE_SOURCE_MSAA - xenos::MsaaSamples of the render target (0, 1, 2),
// NB_DIRECT_RESOLVE_SCALE_X, NB_DIRECT_RESOLVE_SCALE_Y - the draw resolution scale,
// NB_DIRECT_RESOLVE_2X_HOST_SAMPLE_0, _1 - draw_util::GetD3D10SampleIndexForGuest2xMSAA(0 / 1).

#include "xesl.xesli"
#include "nb_direct_resolve_address.hlsli"

cbuffer nb_direct_resolve_constants : register(b1, space0) {
  // Render target base tile in bits 0-10, pitch in tiles in bits 11-20.
  uint nb_direct_resolve_source_info;
  // Verification: index of the pair of counters (differences, compared stores) for this resolve.
  uint nb_direct_resolve_verify_slot;
};

#if NB_DIRECT_RESOLVE_SOURCE_IS_UINT
  #define NB_DIRECT_RESOLVE_TEXEL uint4
#else
  #define NB_DIRECT_RESOLVE_TEXEL float4
#endif
#if NB_DIRECT_RESOLVE_SOURCE_MSAA != 0
  Texture2DMS<NB_DIRECT_RESOLVE_TEXEL> nb_direct_resolve_source : register(t0, space0);
#else
  Texture2D<NB_DIRECT_RESOLVE_TEXEL> nb_direct_resolve_source : register(t0, space0);
#endif

#ifdef NB_DIRECT_RESOLVE_VERIFY
  RWByteAddressBuffer nb_direct_resolve_verify : register(u1, space0);
#endif

// Bit field insertion exactly as the DXBC bfi the dump shader uses.
uint NbDirectResolveBfi(uint width, uint offset, uint insert, uint base) {
  uint mask = ((1u << width) - 1u) << offset;
  return (base & ~mask) | ((insert << offset) & mask);
}

// The dump shader's UnclampedFloat32To7e3 (DxbcShaderTranslator), operation for operation.
uint NbDirectResolveFloat32To7e3(float value) {
  uint f32 = asuint(min(max(value, 0.0f), 31.875f));
  uint biased_f32;
  [branch] if (f32 < 0x3E800000u) {
    biased_f32 = ((f32 & 0x7FFFFFu) | 0x800000u) >> min(125u - (f32 >> 23u), 24u);
  } else {
    biased_f32 = f32 + 0xC2000000u;
  }
  return ((biased_f32 + 0x7FFFu + ((biased_f32 >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

// The 32-bit word the dump shader stores for one sample (GetOrCreateDumpPipeline, color formats).
uint NbDirectResolveEncode(NB_DIRECT_RESOLVE_TEXEL texel) {
  uint packed;
#if NB_DIRECT_RESOLVE_SOURCE_FORMAT == 0  // k_8_8_8_8
  uint4 channels = uint4(mad(texel, 255.0f, 0.5f));
  packed = channels.x;
  packed = NbDirectResolveBfi(8u, 8u, channels.y, packed);
  packed = NbDirectResolveBfi(8u, 16u, channels.z, packed);
  packed = NbDirectResolveBfi(8u, 24u, channels.w, packed);
#elif NB_DIRECT_RESOLVE_SOURCE_FORMAT == 2 || NB_DIRECT_RESOLVE_SOURCE_FORMAT == 10
  // k_2_10_10_10, k_2_10_10_10_AS_10_10_10_10
  uint4 channels = uint4(mad(texel, float4(1023.0f, 1023.0f, 1023.0f, 3.0f), 0.5f));
  packed = channels.x;
  packed = NbDirectResolveBfi(10u, 10u, channels.y, packed);
  packed = NbDirectResolveBfi(10u, 20u, channels.z, packed);
  packed = NbDirectResolveBfi(2u, 30u, channels.w, packed);
#elif NB_DIRECT_RESOLVE_SOURCE_FORMAT == 3 || NB_DIRECT_RESOLVE_SOURCE_FORMAT == 12
  // k_2_10_10_10_FLOAT, k_2_10_10_10_FLOAT_AS_16_16_16_16
  packed = NbDirectResolveFloat32To7e3(texel.x);
  packed = NbDirectResolveBfi(10u, 10u, NbDirectResolveFloat32To7e3(texel.y), packed);
  packed = NbDirectResolveBfi(10u, 20u, NbDirectResolveFloat32To7e3(texel.z), packed);
  uint alpha = uint(mad(saturate(texel.w), 3.0f, 0.5f));
  packed = NbDirectResolveBfi(2u, 30u, alpha, packed);
#elif NB_DIRECT_RESOLVE_SOURCE_FORMAT == 4 || NB_DIRECT_RESOLVE_SOURCE_FORMAT == 6
  // k_16_16, k_16_16_FLOAT, loaded as integers.
  packed = NbDirectResolveBfi(16u, 16u, texel.y, texel.x);
#elif NB_DIRECT_RESOLVE_SOURCE_FORMAT == 14
  // k_32_FLOAT, loaded as integers.
  packed = texel.x;
#else
  #error Unsupported direct resolve source format
#endif
  return packed;
}

uint NbDirectResolveLoadInt(uint address_ints) {
  uint4 location = NbDirectResolveSourceLocation(
      address_ints, nb_direct_resolve_source_info, NB_DIRECT_RESOLVE_SCALE_X,
      NB_DIRECT_RESOLVE_SCALE_Y, NB_DIRECT_RESOLVE_SOURCE_MSAA);
#if NB_DIRECT_RESOLVE_SOURCE_MSAA == 2
  NB_DIRECT_RESOLVE_TEXEL texel =
      nb_direct_resolve_source.Load(int2(location.xy), int(location.z));
#elif NB_DIRECT_RESOLVE_SOURCE_MSAA == 1
  NB_DIRECT_RESOLVE_TEXEL texel = nb_direct_resolve_source.Load(
      int2(location.xy),
      int(location.z != 0u ? NB_DIRECT_RESOLVE_2X_HOST_SAMPLE_1 : NB_DIRECT_RESOLVE_2X_HOST_SAMPLE_0));
#else
  NB_DIRECT_RESOLVE_TEXEL texel = nb_direct_resolve_source.Load(int3(location.xy, 0));
#endif
  return NbDirectResolveEncode(texel);
}

uint2 NbDirectResolveLoadInts2(uint address_ints) {
  return uint2(NbDirectResolveLoadInt(address_ints), NbDirectResolveLoadInt(address_ints + 1u));
}

uint3 NbDirectResolveLoadInts3(uint address_ints) {
  return uint3(NbDirectResolveLoadInts2(address_ints), NbDirectResolveLoadInt(address_ints + 2u));
}

uint4 NbDirectResolveLoadInts4(uint address_ints) {
  return uint4(NbDirectResolveLoadInts2(address_ints), NbDirectResolveLoadInts2(address_ints + 2u));
}

// The EDRAM buffer is not bound; its reads go to the functions above. In the resolve copy shaders the
// EDRAM buffer is the only buffer that is read, so the load macros can be replaced outright.
#undef array_buffer_declare_xe
#define array_buffer_declare_xe(value_type, name, glsl_set, glsl_binding, hlsl_t, hlsl_t_space)
#undef uint_vector_buffer_declare_xe
#define uint_vector_buffer_declare_xe(name, glsl_set, glsl_binding, hlsl_t, hlsl_t_space)
// The fast shaders read Buffer<uint4>, indexed in 16-byte units.
#undef array_buffer_load_xe
#define array_buffer_load_xe(name, position) NbDirectResolveLoadInts4(uint(position) << 2u)
// The full shaders read a ByteAddressBuffer, indexed in 32-bit units.
#undef uint_vector_buffer_load1_xe
#define uint_vector_buffer_load1_xe(name, position) NbDirectResolveLoadInt(uint(position))
#undef uint_vector_buffer_load2_xe
#define uint_vector_buffer_load2_xe(name, position) NbDirectResolveLoadInts2(uint(position))
#undef uint_vector_buffer_load3_xe
#define uint_vector_buffer_load3_xe(name, position) NbDirectResolveLoadInts3(uint(position))
#undef uint_vector_buffer_load4_xe
#define uint_vector_buffer_load4_xe(name, position) NbDirectResolveLoadInts4(uint(position))

#ifdef NB_DIRECT_RESOLVE_VERIFY
  // Typed UAV loads of R32G32_UINT and R32G32B32A32_UINT need
  // D3D12_FEATURE_DATA_D3D12_OPTIONS::TypedUAVLoadAdditionalFormats, checked on the C++ side.
  void NbDirectResolveVerifyStore(RWBuffer<uint> dest, uint position, uint value) {
    uint differs = dest[position] != value ? 1u : 0u;
    nb_direct_resolve_verify.InterlockedAdd(nb_direct_resolve_verify_slot * 8u, differs);
    nb_direct_resolve_verify.InterlockedAdd(nb_direct_resolve_verify_slot * 8u + 4u, 1u);
  }
  void NbDirectResolveVerifyStore(RWBuffer<uint2> dest, uint position, uint2 value) {
    uint differs = any(dest[position] != value) ? 1u : 0u;
    nb_direct_resolve_verify.InterlockedAdd(nb_direct_resolve_verify_slot * 8u, differs);
    nb_direct_resolve_verify.InterlockedAdd(nb_direct_resolve_verify_slot * 8u + 4u, 1u);
  }
  void NbDirectResolveVerifyStore(RWBuffer<uint4> dest, uint position, uint4 value) {
    uint differs = any(dest[position] != value) ? 1u : 0u;
    nb_direct_resolve_verify.InterlockedAdd(nb_direct_resolve_verify_slot * 8u, differs);
    nb_direct_resolve_verify.InterlockedAdd(nb_direct_resolve_verify_slot * 8u + 4u, 1u);
  }
  #undef array_buffer_store_xe
  #define array_buffer_store_xe(name, position, value) \
      NbDirectResolveVerifyStore(name, uint(position), value)
#endif

// Keep this list in the order of NbDirectResolveCopyShaderFile (render_target_cache.cpp).
#if NB_DIRECT_RESOLVE_COPY_SHADER == 0
  #include "resolve_fast_32bpp_1x2xmsaa.xesli"
#elif NB_DIRECT_RESOLVE_COPY_SHADER == 1
  #include "resolve_fast_32bpp_4xmsaa.xesli"
#elif NB_DIRECT_RESOLVE_COPY_SHADER == 2
  #include "resolve_full_8bpp.xesli"
#elif NB_DIRECT_RESOLVE_COPY_SHADER == 3
  #include "resolve_full_16bpp.xesli"
#elif NB_DIRECT_RESOLVE_COPY_SHADER == 4
  #include "resolve_full_32bpp.xesli"
#elif NB_DIRECT_RESOLVE_COPY_SHADER == 5
  #include "resolve_full_64bpp.xesli"
#elif NB_DIRECT_RESOLVE_COPY_SHADER == 6
  #include "resolve_full_128bpp.xesli"
#else
  #error Unsupported direct resolve copy shader
#endif
