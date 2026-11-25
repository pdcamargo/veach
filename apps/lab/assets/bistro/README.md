# Amazon Lumberyard Bistro (exterior)

From NVIDIA's Open Research Content Archive: [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro),
package `Bistro_v5_2.zip` (version 5.2). **CC BY 4.0**; the legal code is `LICENSE.txt`, and the package's
`README.txt` and `CHANGELOG.txt` are kept as published. Attribution: *Amazon Lumberyard Bistro, Open Research
Content Archive (ORCA)* (see [THIRD_PARTY_NOTICES](../../../../THIRD_PARTY_NOTICES.md)).

**Modified:** converted once from `BistroExterior.fbx` to glTF with Blender by
[tools/assets/convert_bistro.py](../../../../tools/assets/convert_bistro.py), which documents every change:
materials rebuilt as metallic-roughness (the Specular map's R = occlusion, G = roughness, B = metalness), DirectX
normal maps' green channel flipped, alpha-tested materials as `MASK`, textures re-encoded as JPEG or PNG, the
camera dropped. The source scale is kept (props are about 1.6x real size, as in the source and in Falcor).

`BistroExterior.gltf.meta` caps textures at 1024 so cooking stays practical. Not downloaded by CI (Git LFS
fetches only what CI tests use); its goldens and benchmarks run in the pre-push gate
([ADR 0036](../../../../docs/adr/0036-no-self-hosted-gpu-runner.md)).
