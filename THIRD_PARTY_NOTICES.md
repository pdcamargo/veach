# Third-party notices

Veach is MIT-licensed (see [LICENSE](LICENSE)). It includes or links the third-party software below. Each entry gives the pinned version, where it is used, and its full license text. Versions and hashes are pinned in `cmake/VeachDependencies.cmake`; update this file whenever a dependency is added or bumped.

| Dependency | Version | License | Used in |
|---|---|---|---|
| [Flecs](https://github.com/SanderMertens/flecs) | 4.2.0 | MIT | Engine (ECS). Linked into every Veach binary. |
| [Luau](https://github.com/luau-lang/luau) | 0.748 | MIT | Engine (scripting VM and compiler) and the luau-analyze tool. |
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.20 | zlib | Engine (windowing and input), statically linked into veach_platform. |
| [GLM](https://github.com/g-truc/glm) | 1.0.3 | MIT (Veach uses GLM under its MIT option) | Engine (math, `veach::math`). Header-only. |
| [JSON for Modern C++ (nlohmann/json)](https://github.com/nlohmann/json) | 3.12.0 | MIT | Engine (JSON data files and schema output). Header-only. |
| [Tracy Profiler](https://github.com/wolfpld/tracy) | 0.15.0 | BSD-3-Clause | Optional: linked only when built with VEACH_TRACY=ON (profile preset). |
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | Tests only; not linked into apps. |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | 0.43.2 | MIT | Build tooling, vendored as cmake/CPM.cmake. |
| [Slang](https://github.com/shader-slang/slang) | 2026.22 | Apache-2.0 WITH LLVM-exception | Build tool only (`slangc` compiles shaders at build time); not linked into or shipped with apps. |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | 1.9.2609 | LLVM Release License; `dxil.dll` under the Microsoft Software License Terms | Windows build tool only (Slang's DXIL backend); not linked into or shipped with apps. |
| [stb](https://github.com/nothings/stb) (`stb_image`, `stb_image_write`) | c16853e | MIT or public domain (your choice) | Engine (PNG screenshots), test tools. |
| [NVIDIA FLIP](https://github.com/NVlabs/flip) | d3ab02a | BSD-3-Clause | `veach imgdiff` (golden-image comparison) only. |
| [enkiTS](https://github.com/dougbinks/enkiTS) | 1.12 | zlib | Engine (the job system's task scheduler), compiled into `veach_core`. |
| [fastgltf](https://github.com/spnda/fastgltf) | 1.0.1 | MIT | Engine (glTF import). |
| [simdjson](https://github.com/simdjson/simdjson) | 5.1.0 | Apache-2.0 or MIT (Veach uses simdjson under its MIT option) | Engine, through fastgltf (JSON parsing). Downloaded by fastgltf's build. |
| [simdutf](https://github.com/simdutf/simdutf) | 9.3.3 | Apache-2.0 or MIT (Veach uses simdutf under its MIT option) | Engine, through fastgltf (base64 decoding). Downloaded by fastgltf's build. |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.3 | MIT | Engine (meshlets at cook time). |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) | b943862 | MIT or public domain (your choice) | Engine texture cooking (BC7 encoding); its decoder in tests. Only `bc7enc.cpp` and `bc7decomp.cpp` are built. |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | 5.7.0 | MIT | Engine (rigid-body physics, private to `engine/physics`, ADR 0058); today only the SP-11 spike in `tests/sp11/`. |
| [LLVM libc](https://github.com/llvm/llvm-project/tree/main/libc) (header-only math, `libc/shared/math`) | e6091a8 | Apache-2.0 WITH LLVM-exception | Engine (`veach::dmath`, correctly rounded math for canonical code). Headers only, compiled into `veach_core`. |
| [Transvoxel data tables](https://github.com/EricLengyel/Transvoxel) (Eric Lengyel) | 51a494f | MIT | Engine (`veach::voxel`, spike SP-08: marching cubes and Transvoxel lookup tables). Vendored as `engine/voxel/src/transvoxel_tables.inl`. |
| [DirectX 12 Agility SDK](https://aka.ms/directx12agility) (NuGet `Microsoft.Direct3D.D3D12`) | 1.620.2 | Microsoft Software License Terms (runtime DLLs); MIT (headers) | Windows builds only: headers, plus `D3D12Core.dll` and `d3d12SDKLayers.dll` copied next to each executable. |
| [Direct3D WARP](https://aka.ms/direct3dwarp) (NuGet `Microsoft.Direct3D.WARP`) | 1.0.22 | Microsoft Software License Terms (testing only, not redistributable) | Windows builds only: `d3d10warp.dll` copied next to each executable for DX12 on GPU-less CI. Must not ship with Veach. |

## Flecs 4.2.0

Source: https://github.com/SanderMertens/flecs  
License: MIT

```text
MIT License

Copyright (c) 2025 Sander Mertens

Portions Copyright (c) Meta Platforms, Inc. and affiliates

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Luau 0.748

Source: https://github.com/luau-lang/luau  
License: MIT

```text
MIT License

Copyright (c) 2019-2026 Roblox Corporation
Copyright (c) 1994–2019 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## SDL 3.4.20

Source: https://github.com/libsdl-org/SDL  
License: zlib

```text
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## GLM 1.0.3

Source: https://github.com/g-truc/glm  
License: MIT (Veach uses GLM under its MIT option)

```text
================================================================================
OpenGL Mathematics (GLM)
--------------------------------------------------------------------------------
GLM is licensed under The Happy Bunny License or MIT License

================================================================================
The Happy Bunny License (Modified MIT License)
--------------------------------------------------------------------------------
Copyright (c) 2005 - G-Truc Creation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Restrictions:
 By making use of the Software for military purposes, you choose to make a
 Bunny unhappy.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

================================================================================
The MIT License
--------------------------------------------------------------------------------
Copyright (c) 2005 - G-Truc Creation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## JSON for Modern C++ (nlohmann/json) 3.12.0

Source: https://github.com/nlohmann/json  
License: MIT

```text
MIT License 

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Tracy Profiler 0.15.0

Source: https://github.com/wolfpld/tracy  
License: BSD-3-Clause

```text
Tracy Profiler (https://github.com/wolfpld/tracy) is licensed under the
3-clause BSD license.

Copyright (c) 2017-2026, Bartosz Taudul <wolf@nereid.pl>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## doctest 2.5.3

Source: https://github.com/doctest/doctest  
License: MIT

```text
The MIT License (MIT)

Copyright (c) 2016-2023 Viktor Kirilov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## CPM.cmake 0.43.2

Source: https://github.com/cpm-cmake/CPM.cmake  
License: MIT

```text
MIT License
-----------
[[
Copyright (c) 2019-2023 Lars Melchior and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Slang 2026.22

Source: https://github.com/shader-slang/slang  
License: Apache-2.0 WITH LLVM-exception

```text
SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

LLVM Exceptions to the Apache 2.0 License

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into an Object form of such source code, you
may redistribute such embedded portions in such Object form without complying
with the conditions of Sections 4(a), 4(b) and 4(d) of the License.

In addition, if you combine or link compiled forms of this Software with
software that is licensed under the GPLv2 ("Combined Software") and if a
court of competent jurisdiction determines that the patent provision (Section
3), the indemnity provision (Section 9) or other Section of the License
conflicts with the conditions of the GPLv2, you may retroactively and
prospectively choose to deem waived or otherwise exclude such Section(s) of
the License, but only in their entirety and only with respect to the Combined
Software.
```

## DirectX Shader Compiler 1.9.2609

Source: https://github.com/microsoft/DirectXShaderCompiler  
License: LLVM Release License (compiler); Microsoft Software License Terms (`dxil.dll` in the release package)

Used only on Windows build machines to produce DXIL; Veach does not redistribute it. LLVM Release License:

```text
==============================================================================
LLVM Release License
==============================================================================
University of Illinois/NCSA
Open Source License

Copyright (c) 2003-2015 University of Illinois at Urbana-Champaign.
All rights reserved.

Developed by:

    LLVM Team

    University of Illinois at Urbana-Champaign

    http://llvm.org

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal with
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimers.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimers in the
      documentation and/or other materials provided with the distribution.

    * Neither the names of the LLVM Team, University of Illinois at
      Urbana-Champaign, nor the names of its contributors may be used to
      endorse or promote products derived from this Software without specific
      prior written permission.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
SOFTWARE.
```

## stb (c16853e)

Source: https://github.com/nothings/stb  
License: MIT or public domain

```text
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## NVIDIA FLIP (d3ab02a)

Source: https://github.com/NVlabs/flip  
License: BSD-3-Clause

```text
BSD 3-Clause License

Copyright (c) 2020-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

SPDX-FileCopyrightText: Copyright (c) 2020-2025 NVIDIA CORPORATION & AFFILIATES
SPDX-License-Identifier: BSD-3-Clause
```

## enkiTS 1.12

Source: https://github.com/dougbinks/enkiTS  
License: zlib

```text
Copyright (c) 2013 Doug Binks

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgement in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## fastgltf 1.0.1

Source: https://github.com/spnda/fastgltf  
License: MIT

```text
The MIT License

Copyright (c) 2022 - 2026 Sean Apeler.
All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## simdjson 5.1.0

Source: https://github.com/simdjson/simdjson  
License: Apache-2.0 or MIT (Veach uses simdjson under its MIT option)

Downloaded by fastgltf's build (`FASTGLTF_DOWNLOAD_SIMDJSON`) as the release's amalgamated `simdjson.h` and `simdjson.cpp`. MIT license text from `LICENSE-MIT` at tag v5.1.0:

```text
Copyright 2018-2025 The simdjson authors

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Code bundled inside simdjson 5.1.0, as stated in its source comments:

- CPU feature detection derived from PyTorch: BSD-3-Clause, text in "Code bundled in simdjson and simdutf" below.
- fast_float 8.2.10 (renamed `simdjson_fast_float`): Apache-2.0, MIT or BSL-1.0; Veach uses the MIT option, "Copyright (c) 2021 The simdjson_fast_float authors", same terms as the MIT text above.
- A port of Dragonbox (Copyright 2020-2025 Junekey Jeon and contributors) and string-view-lite (Copyright 2017-2020 Martin Moene): Boost Software License 1.0, https://www.boost.org/LICENSE_1_0.txt.

## simdutf 9.3.3

Source: https://github.com/simdutf/simdutf  
License: Apache-2.0 or MIT (Veach uses simdutf under its MIT option)

Downloaded by fastgltf's build (`FASTGLTF_DOWNLOAD_SIMDUTF`) as the release's amalgamated `simdutf.h` and `simdutf.cpp`. MIT license text from `LICENSE-MIT` at tag v9.3.3:

```text
Copyright 2021 The simdutf authors

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Code bundled inside simdutf 9.3.3, as stated in its source comments: the same PyTorch-derived CPU feature detection (BSD-3-Clause, below), and two routines marked "based on code from Google Fuchsia (Apache Licensed)".

### Code bundled in simdjson and simdutf: PyTorch CPU detection

```text
Copyright (c) 2016-     Facebook, Inc            (Adam Paszke)
Copyright (c) 2014-     Facebook, Inc            (Soumith Chintala)
Copyright (c) 2011-2014 Idiap Research Institute (Ronan Collobert)
Copyright (c) 2012-2014 Deepmind Technologies    (Koray Kavukcuoglu)
Copyright (c) 2011-2012 NEC Laboratories America (Koray Kavukcuoglu)
Copyright (c) 2011-2013 NYU                      (Clement Farabet)
Copyright (c) 2006-2010 NEC Laboratories America (Ronan Collobert, Leon Bottou,
Iain Melvin, Jason Weston) Copyright (c) 2006      Idiap Research Institute
(Samy Bengio) Copyright (c) 2001-2004 Idiap Research Institute (Ronan Collobert,
Samy Bengio, Johnny Mariethoz)

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the names of Facebook, Deepmind Technologies, NYU, NEC Laboratories
America and IDIAP Research Institute nor the names of its contributors may be
   used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
```

## meshoptimizer 1.3

Source: https://github.com/zeux/meshoptimizer  
License: MIT

Used at cook time to split meshes into meshlets.

```
MIT License

Copyright (c) 2016-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## bc7enc_rdo (b943862)

Source: https://github.com/richgel999/bc7enc_rdo  
License: MIT or public domain (your choice)

Veach builds only `bc7enc.cpp` and `bc7decomp.cpp` (with their headers `bc7enc.h` and `bc7decomp.h`), which carry the license below. The repository also contains `bc7e.ispc` (Apache-2.0, Copyright (C) 2018-2021 Binomial LLC) and LodePNG (zlib); Veach does not compile either.

```text
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
If you use this software in a product, attribution / credits is requested but not required.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright(c) 2020-2021 Richard Geldreich, Jr.
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files(the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions :
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain(www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non - commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain.We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors.We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
```

## Jolt Physics 5.7.0

Source: https://github.com/jrouwe/JoltPhysics  
License: MIT

Rigid-body physics, built with double-precision positions and cross-platform determinism.

```text
Copyright 2021 Jorrit Rouwe

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## LLVM libc (e6091a8)

Source: https://github.com/llvm/llvm-project (commit e6091a8158c863643d4fbe44b70fe62dd0ef3eba; only `libc/shared`,
`libc/src/__support`, `libc/src/string`, `libc/hdr` and `libc/include/llvm-libc-*` are used)  
License: Apache-2.0 WITH LLVM-exception

```text
==============================================================================
The LLVM Project is under the Apache License v2.0 with LLVM Exceptions:
==============================================================================

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

    TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

    1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

    2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

    3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

    4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

    5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

    6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

    7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

    8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

    9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

    END OF TERMS AND CONDITIONS

    APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

    Copyright [yyyy] [name of copyright owner]

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.


---- LLVM Exceptions to the Apache 2.0 License ----

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into an Object form of such source code, you
may redistribute such embedded portions in such Object form without complying
with the conditions of Sections 4(a), 4(b) and 4(d) of the License.

In addition, if you combine or link compiled forms of this Software with
software that is licensed under the GPLv2 ("Combined Software") and if a
court of competent jurisdiction determines that the patent provision (Section
3), the indemnity provision (Section 9) or other Section of the License
conflicts with the conditions of the GPLv2, you may retroactively and
prospectively choose to deem waived or otherwise exclude such Section(s) of
the License, but only in their entirety and only with respect to the Combined
Software.

==============================================================================
Software from third parties included in the LLVM Project:
==============================================================================
The LLVM Project contains third party software which is under different license
terms. All such code will be identified clearly using at least one of two
mechanisms:
1) It will be in a separate directory tree with its own `LICENSE.txt` or
   `LICENSE` file at the top containing the specific license and restrictions
   which apply to that software, or
2) It will contain specific license and restriction terms at the top of every
   file.

==============================================================================
Legacy LLVM License (https://llvm.org/docs/DeveloperPolicy.html#legacy):
==============================================================================
University of Illinois/NCSA
Open Source License

Copyright (c) 2007-2019 University of Illinois at Urbana-Champaign.
All rights reserved.

Developed by:

    LLVM Team

    University of Illinois at Urbana-Champaign

    http://llvm.org

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal with
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimers.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimers in the
      documentation and/or other materials provided with the distribution.

    * Neither the names of the LLVM Team, University of Illinois at
      Urbana-Champaign, nor the names of its contributors may be used to
      endorse or promote products derived from this Software without specific
      prior written permission.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
SOFTWARE.
```

## Transvoxel data tables (51a494f)

Source: https://github.com/EricLengyel/Transvoxel (`Transvoxel.cpp`, commit 51a494f03c5b, 2023-11-02)  
License: MIT

Vendored with CRLF line endings changed to LF, as `engine/voxel/src/transvoxel_tables.inl`. The algorithm itself is
described at https://transvoxel.org/, which says it is free of patent claims.

```text
MIT License

Copyright (c) 2009 Eric Lengyel

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## DirectX 12 Agility SDK 1.620.2

Source: NuGet package `Microsoft.Direct3D.D3D12` 1.620.2, https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.620.2  
License: Microsoft Software License Terms (runtime files under `build/native/bin/`); MIT (headers under `build/native/include/` and `metadata/`)

Windows builds only. Veach compiles against the headers and copies `D3D12Core.dll` and `d3d12SDKLayers.dll` (both on the package's list of distributable files) next to each executable. License for the runtime files (`LICENSE.txt` in the package):

```text
MICROSOFT SOFTWARE LICENSE TERMS

MICROSOFT DIRECTX

These license terms are an agreement between you and Microsoft
Corporation (or one of its affiliates). They apply to the software named
above and any Microsoft services or software updates (except to the
extent such services or updates are accompanied by new or additional
terms, in which case those different terms apply prospectively and do
not alter your or Microsoft’s rights relating to pre-updated software or
services). IF YOU COMPLY WITH THESE LICENSE TERMS, YOU HAVE THE RIGHTS
BELOW. BY USING THE SOFTWARE, YOU ACCEPT THESE TERMS.

INSTALLATION AND USE RIGHTS. 

General. Subject to the terms of this agreement, you may install and use any number of copies of the software, and solely for use on Windows.

Included Microsoft Applications. The software may include other Microsoft applications. These license terms apply to those included applications, if any, unless other license terms are provided with the other Microsoft applications.

Microsoft Platforms. The software may include components from Microsoft Windows. These components are governed by separate agreements and their own product support policies, as described in the license terms found in the installation directory for that component or in the “Licenses” folder accompanying the software.

Third Party Components. The software may include third party components with separate legal notices or governed by other agreements, as may be described in the ThirdPartyNotices file(s) accompanying the software.

DATA. 

Data Collection. The software may collect information about you and your use of the software, and send that to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may opt-out of many of these scenarios, but not all, as described in the product documentation.  There are also some features in the software that may enable you to collect data from users of your applications. If you use these features to enable data collection in your applications, you must comply with applicable law, including providing appropriate notices to users of your applications. You can learn more about data collection and use in the help documentation and the privacy statement at https://aka.ms/privacy. Your use of the software operates as your consent to these practices.

Processing of Personal Data. To the extent Microsoft is a processor or subprocessor of personal data in connection with the software, Microsoft makes the commitments in the European Union General Data Protection Regulation Terms of the Online Services Terms to all customers effective May 25, 2018, at https://docs.microsoft.com/en-us/legal/gdpr.

DISTRIBUTABLE CODE. The software may contain code you are permitted to distribute (i.e. make available for third parties) in applications you develop, as described in this Section. 

Distribution Rights. The code and test files described below are distributable if included with the software. 

Distributables. You may copy and distribute the object code form of the software listed in the distributables file list in the software; and

Third Party Distribution. You may permit distributors of your applications to copy and distribute any of this distributable code you elect to distribute with your applications.

Distribution Requirements. For any code you distribute, you must:

i. add significant primary functionality to it in your applications; 

ii.  require distributors and external end users to agree to terms that protect it and Microsoft at least as much as this agreement; and

iii. indemnify, defend, and hold harmless Microsoft from any claims, including attorneys’ fees, related to the distribution or use of your applications, except to the extent that any claim is based solely on the unmodified distributable code.

Distribution Restrictions. You may not:

use Microsoft’s trademarks or trade dress in your application in any way that suggests your application comes from or is endorsed by Microsoft; or modify or distribute the source code of any distributable code so that any part of it becomes subject to any license that requires that the distributable code, any other part of the software, or any of Microsoft’s other intellectual property be disclosed or distributed in source code form, or that others have the right to modify it.

SCOPE OF LICENSE. The software is licensed, not sold. Microsoft reserves all other rights. Unless applicable law gives you more rights despite this limitation, you will not (and have no right to):

work around any technical limitations in the software that only allow you to use it in certain ways;

reverse engineer, decompile or disassemble the software, or otherwise attempt to derive the source code for the software, except and to the extent required by third party licensing terms governing use of certain open source components that may be included in the software;

remove, minimize, block, or modify any notices of Microsoft or its suppliers in the software;

use the software in any way that is against the law or to create or propagate malware; or

share, publish, distribute, or lease the software (except for any distributable code, subject to the terms above), provide the software as a stand-alone offering for others to use, or transfer the software or this agreement to any third party.

EXPORT RESTRICTIONS. You must comply with all domestic and international export laws and regulations that apply to the software, which include restrictions on destinations, end users, and end use. For further information on export restrictions, visit https://aka.ms/exporting.

SUPPORT SERVICES. Microsoft is not obligated under this agreement to provide any support services for the software. Any support provided is “as is”, “with all faults”, and without warranty of any kind.

UPDATES. The software may periodically check for updates, and download and install them for you. You may obtain updates only from Microsoft or authorized sources. Microsoft may need to update your system to provide you with updates. You agree to receive these automatic updates without any additional notice. Updates may not include or support all existing software features, services, or peripheral devices.

ENTIRE AGREEMENT. This agreement, and any other terms Microsoft may provide for supplements, updates, or third-party applications, is the entire agreement for the software.

APPLICABLE LAW AND PLACE TO RESOLVE DISPUTES. If you acquired the software in the United States or Canada, the laws of the state or province where you live (or, if a business, where your principal place of business is located) govern the interpretation of this agreement, claims for its breach, and all other claims (including consumer protection, unfair competition, and tort claims), regardless of conflict of laws principles. If you acquired the software in any other country, its laws apply. If U.S. federal jurisdiction exists, you and Microsoft consent to exclusive jurisdiction and venue in the federal court in King County, Washington for all disputes heard in court. If not, you and Microsoft consent to exclusive jurisdiction and venue in the Superior Court of King County, Washington for all disputes heard in court.

CONSUMER RIGHTS; REGIONAL VARIATIONS. This agreement describes certain legal rights. You may have other rights, including consumer rights, under the laws of your state or country. Separate and apart from your relationship with Microsoft, you may also have rights with respect to the party from which you acquired the software. This agreement does not change those other rights if the laws of your state or country do not permit it to do so. For example, if you acquired the software in one of the below regions, or mandatory country law applies, then the following provisions apply to you:

a.  Australia. You have statutory guarantees under the Australian
    Consumer Law and nothing in this agreement is intended to affect
    those rights.

b.  Canada. If you acquired this software in Canada, you may stop
    receiving updates by turning off the automatic update feature,
    disconnecting your device from the Internet (if and when you
    re-connect to the Internet, however, the software will resume
    checking for and installing updates), or uninstalling the software.
    The product documentation, if any, may also specify how to turn off
    updates for your specific device or software.

c.  Germany and Austria.

  i. Warranty. The properly licensed software will perform substantially
  as described in any Microsoft materials that accompany the software.
  However, Microsoft gives no contractual guarantee in relation to the
  licensed software.

  ii. Limitation of Liability. In case of intentional conduct, gross
  negligence, claims based on the Product Liability Act, as well as, in
  case of death or personal or physical injury, Microsoft is liable
  according to the statutory law.

Subject to the foregoing clause ii., Microsoft will only be liable for slight negligence if Microsoft is in breach of such material contractual obligations, the fulfillment of which facilitate the due performance of this agreement, the breach of which would endanger the purpose of this agreement and the compliance with which a party may constantly trust in (so-called "cardinal obligations"). In other cases of slight negligence, Microsoft will not be liable for slight negligence.

DISCLAIMER OF WARRANTY. THE SOFTWARE IS LICENSED “AS IS.” YOU BEAR THE RISK OF USING IT. MICROSOFT GIVES NO EXPRESS WARRANTIES, GUARANTEES, OR CONDITIONS. TO THE EXTENT PERMITTED UNDER APPLICABLE LAWS, MICROSOFT EXCLUDES ALL IMPLIED WARRANTIES, INCLUDING MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.

LIMITATION ON AND EXCLUSION OF DAMAGES. IF YOU HAVE ANY BASIS FOR RECOVERING DAMAGES DESPITE THE PRECEDING DISCLAIMER OF WARRANTY, YOU CAN RECOVER FROM MICROSOFT AND ITS SUPPLIERS ONLY DIRECT DAMAGES UP TO U.S. $5.00. YOU CANNOT RECOVER ANY OTHER DAMAGES, INCLUDING CONSEQUENTIAL, LOST PROFITS, SPECIAL, INDIRECT, OR INCIDENTAL DAMAGES.

This limitation applies to (a) anything related to the software,
services, content (including code) on third party Internet sites, or
third party applications; and (b) claims for breach of contract,
warranty, guarantee, or condition; strict liability, negligence, or
other tort; or any other claim; in each case to the extent permitted by
applicable law.

It also applies even if Microsoft knew or should have known about the
possibility of the damages. The above limitation or exclusion may not
apply to you because your state, province, or country may not allow the
exclusion or limitation of incidental, consequential, or other damages.

Please note: As this software is distributed in Canada, some of the
clauses in this agreement are provided below in French.

Remarque: Ce logiciel étant distribué au Canada, certaines des clauses
dans ce contrat sont fournies ci-dessous en français.

EXONÉRATION DE GARANTIE. Le logiciel visé par une licence est offert «
tel quel ». Toute utilisation de ce logiciel est à votre seule risque et
péril. Microsoft n’accorde aucune autre garantie expresse. Vous pouvez
bénéficier de droits additionnels en vertu du droit local sur la
protection des consommateurs, que ce contrat ne peut modifier. La ou
elles sont permises par le droit locale, les garanties implicites de
qualité marchande, d’adéquation à un usage particulier et d’absence de
contrefaçon sont exclues.

LIMITATION DES DOMMAGES-INTÉRÊTS ET EXCLUSION DE RESPONSABILITÉ POUR LES
DOMMAGES. Vous pouvez obtenir de Microsoft et de ses fournisseurs une
indemnisation en cas de dommages directs uniquement à hauteur de 5,00 $
US. Vous ne pouvez prétendre à aucune indemnisation pour les autres
dommages, y compris les dommages spéciaux, indirects ou accessoires et
pertes de bénéfices.

Cette limitation concerne:

• tout ce qui est relié au logiciel, aux services ou au contenu (y
compris le code) figurant sur des sites Internet tiers ou dans des
programmes tiers; et

• les réclamations au titre de violation de contrat ou de garantie, ou
au titre de responsabilité stricte, de négligence ou d’une autre faute
dans la limite autorisée par la loi en vigueur.

Elle s’applique également, même si Microsoft connaissait ou devrait
connaître l’éventualité d’un tel dommage. Si votre pays n’autorise pas
l’exclusion ou la limitation de responsabilité pour les dommages
indirects, accessoires ou de quelque nature que ce soit, il se peut que
la limitation ou l’exclusion ci-dessus ne s’appliquera pas à votre
égard.

EFFET JURIDIQUE. Le présent contrat décrit certains droits juridiques.
Vous pourriez avoir d’autres droits prévus par les lois de votre pays.
Le présent contrat ne modifie pas les droits que vous confèrent les lois
de votre pays si celles-ci ne le permettent pas.
```

License for the headers (`LICENSE-CODE.txt` in the package):

```text
Copyright (c) Microsoft Corporation.

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Direct3D WARP 1.0.22

Source: NuGet package `Microsoft.Direct3D.WARP` 1.0.22, https://www.nuget.org/packages/Microsoft.Direct3D.WARP/1.0.22  
License: Microsoft Software License Terms (testing only; no redistribution)

Windows builds only. `d3d10warp.dll` is copied next to each executable in the build tree so DX12 can run on CI machines without a GPU. Its license allows use only to develop and test applications and forbids distributing it, so it must not ship with Veach. License (`LICENSE.TXT` in the package):

```text
MICROSOFT SOFTWARE LICENSE TERMS

MICROSOFT DIRECT3D WARP

Purpose – for testing purposes only.
 
IF YOU LIVE IN (OR ARE A BUSINESS WITH A PRINCIPAL PLACE OF BUSINESS IN) THE UNITED 
STATES, PLEASE READ THE “BINDING ARBITRATION AND CLASS ACTION WAIVER” SECTION 
BELOW. IT AFFECTS HOW DISPUTES ARE RESOLVED.
 
These license terms are an agreement between you and Microsoft Corporation (or one of its affiliates). They 
apply to the software named above and any Microsoft services or software updates (except to the extent such 
services or updates are accompanied by new or additional terms, in which case those different terms apply 
prospectively and do not alter your or Microsoft’s rights relating to pre-updated software or services). IF YOU 
COMPLY WITH THESE LICENSE TERMS, YOU HAVE THE RIGHTS BELOW. BY USING THE SOFTWARE, YOU 
ACCEPT THESE TERMS.

1.	INSTALLATION AND USE RIGHTS.

a)	General. You may install and use any number of copies of the software to develop and test your 
applications, solely for the purpose stated above and otherwise for your internal business purposes, 
and solely for use on Windows.

2.	DATA. 

a)	Data Collection. The software may collect information about you and your use of the software, and 
send that to Microsoft. Microsoft may use this information to provide services and improve our 
products and services. You may opt-out of many of these scenarios, but not all, as described in the 
product documentation.  There are also some features in the software that may enable you to collect 
data from users of your applications. If you use these features to enable data collection in your 
applications, you must comply with applicable law, including providing appropriate notices to users of 
your applications. You can learn more about data collection and use in the help documentation and 
the privacy statement at https://aka.ms/privacy. Your use of the software operates as your consent to 
these practices.

b)	Processing of Personal Data. To the extent Microsoft is a processor or subprocessor of personal 
data in connection with the software, Microsoft makes the commitments in the European Union 
General Data Protection Regulation Terms of the Online Services Terms to all customers effective May 
25, 2018, at https://docs.microsoft.com/en-us/legal/gdpr.

3.	SCOPE OF LICENSE. The software is licensed, not sold. Microsoft reserves all other rights. Unless 
applicable law gives you more rights despite this limitation, you will not (and have no right to):

a)	work around any technical limitations in the software that only allow you to use it in certain ways;

b)	reverse engineer, decompile or disassemble the software, or otherwise attempt to derive the source 
code for the software, except and to the extent required by third party licensing terms governing use 
of certain open source components that may be included in the software;

c)	remove, minimize, block, or modify any notices of Microsoft or its suppliers in the software;

d)	use the software in any way that is against the law or to create or propagate malware; or

e)	share, publish, distribute, or lease the software, provide the software as a stand-alone offering for 
others to use, or transfer the software or this agreement to any third party.

4.	EXPORT RESTRICTIONS. You must comply with all domestic and international export laws and 
regulations that apply to the software, which include restrictions on destinations, end users, and end use. 
For further information on export restrictions, visit https://aka.ms/exporting.

5.	SUPPORT SERVICES. Microsoft is not obligated under this agreement to provide any support services for 
the software. Any support provided is “as is”, “with all faults”, and without warranty of any kind. 

6.	UPDATES. The software may periodically check for updates, and download and install them for you. You 
may obtain updates only from Microsoft or authorized sources. Microsoft may need to update your system 
to provide you with updates. You agree to receive these automatic updates without any additional notice. 
Updates may not include or support all existing software features, services, or peripheral devices.

7.	BINDING ARBITRATION AND CLASS ACTION WAIVER. This Section applies if you live in (or, if 
a business, your principal place of business is in) the United States.  If you and Microsoft have a 
dispute, you and Microsoft agree to try for 60 days to resolve it informally. If you and Microsoft can’t, you 
and Microsoft agree to binding individual arbitration before the American Arbitration 
Association under the Federal Arbitration Act (“FAA”), and not to sue in court in front of a judge or 
jury. Instead, a neutral arbitrator will decide. Class action lawsuits, class-wide arbitrations, private 
attorney-general actions, and any other proceeding where someone acts in a representative 
capacity are not allowed; nor is combining individual proceedings without the consent of all 
parties. The complete Arbitration Agreement contains more terms and is at https://aka.ms/arb-
agreement-4. You and Microsoft agree to these terms.

8.	ENTIRE AGREEMENT. This agreement, and any other terms Microsoft may provide for supplements, 
updates, or third-party applications, is the entire agreement for the software.

9.	APPLICABLE LAW AND PLACE TO RESOLVE DISPUTES. If you acquired the software in the United 
States or Canada, the laws of the state or province where you live (or, if a business, where your principal 
place of business is located) govern the interpretation of this agreement, claims for its breach, and all 
other claims (including consumer protection, unfair competition, and tort claims), regardless of conflict of 
laws principles, except that the FAA governs everything related to arbitration. If you acquired the software 
in any other country, its laws apply, except that the FAA governs everything related to arbitration. If U.S. 
federal jurisdiction exists, you and Microsoft consent to exclusive jurisdiction and venue in the federal court 
in King County, Washington for all disputes heard in court (excluding arbitration). If not, you and Microsoft 
consent to exclusive jurisdiction and venue in the Superior Court of King County, Washington for all 
disputes heard in court (excluding arbitration).

10.	CONSUMER RIGHTS; REGIONAL VARIATIONS. This agreement describes certain legal rights. You 
may have other rights, including consumer rights, under the laws of your state, province, or country. 
Separate and apart from your relationship with Microsoft, you may also have rights with respect to the 
party from which you acquired the software. This agreement does not change those other rights if the 
laws of your state, province, or country do not permit it to do so. For example, if you acquired the 
software in one of the below regions, or mandatory country law applies, then the following provisions 
apply to you:

a)	Australia. You have statutory guarantees under the Australian Consumer Law and nothing in this 
agreement is intended to affect those rights.

b)	Canada. If you acquired this software in Canada, you may stop receiving updates by turning off the 
automatic update feature, disconnecting your device from the Internet (if and when you re-connect to 
the Internet, however, the software will resume checking for and installing updates), or uninstalling 
the software. The product documentation, if any, may also specify how to turn off updates for your 
specific device or software.

c)	Germany and Austria.

i.	Warranty. The properly licensed software will perform substantially as described in any Microsoft 
materials that accompany the software. However, Microsoft gives no contractual guarantee in 
relation to the licensed software.

ii.	Limitation of Liability. In case of intentional conduct, gross negligence, claims based on the 
Product Liability Act, as well as, in case of death or personal or physical injury, Microsoft is liable 
according to the statutory law.

Subject to the foregoing clause ii., Microsoft will only be liable for slight negligence if Microsoft is in 
breach of such material contractual obligations, the fulfillment of which facilitate the due performance 
of this agreement, the breach of which would endanger the purpose of this agreement and the 
compliance with which a party may constantly trust in (so-called "cardinal obligations"). In other cases 
of slight negligence, Microsoft will not be liable for slight negligence.

11.	DISCLAIMER OF WARRANTY. THE SOFTWARE IS LICENSED “AS IS.” YOU BEAR THE RISK OF 
USING IT. MICROSOFT GIVES NO EXPRESS WARRANTIES, GUARANTEES, OR CONDITIONS. 
TO THE EXTENT PERMITTED UNDER APPLICABLE LAWS, MICROSOFT EXCLUDES ALL IMPLIED 
WARRANTIES, INCLUDING MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND 
NON-INFRINGEMENT.

12.	LIMITATION ON AND EXCLUSION OF DAMAGES. IF YOU HAVE ANY BASIS FOR RECOVERING 
DAMAGES DESPITE THE PRECEDING DISCLAIMER OF WARRANTY, YOU CAN RECOVER FROM 
MICROSOFT AND ITS SUPPLIERS ONLY DIRECT DAMAGES UP TO U.S. $5.00. YOU CANNOT 
RECOVER ANY OTHER DAMAGES, INCLUDING CONSEQUENTIAL, LOST PROFITS, SPECIAL, 
INDIRECT OR INCIDENTAL DAMAGES.

This limitation applies to (a) anything related to the software, services, content (including 
code) on third party Internet sites, or third party applications; and (b) claims for breach of 
contract, warranty, guarantee, or condition; strict liability, negligence, or other tort; or any 
other claim; in each case to the extent permitted by applicable law.
It also applies even if Microsoft knew or should have known about the possibility of the 
damages. The above limitation or exclusion may not apply to you because your state, 
province, or country may not allow the exclusion or limitation of incidental, consequential, or 
other damages.

Please note: As this software is distributed in Canada, some of the clauses in this agreement are 
provided below in French.

Remarque: Ce logiciel étant distribué au Canada, certaines des clauses dans ce contrat sont 
fournies ci-dessous en français.

EXONÉRATION DE GARANTIE. Le logiciel visé par une licence est offert « tel quel ». Toute 
utilisation de ce logiciel est à votre seule risque et péril. Microsoft n’accorde aucune autre 
garantie expresse. Vous pouvez bénéficier de droits additionnels en vertu du droit local sur la 
protection des consommateurs, que ce contrat ne peut modifier. La ou elles sont permises par le 
droit locale, les garanties implicites de qualité marchande, d’adéquation à un usage particulier et 
d’absence de contrefaçon sont exclues.

LIMITATION DES DOMMAGES-INTÉRÊTS ET EXCLUSION DE RESPONSABILITÉ POUR LES 
DOMMAGES. Vous pouvez obtenir de Microsoft et de ses fournisseurs une indemnisation en cas de 
dommages directs uniquement à hauteur de 5,00 $ US. Vous ne pouvez prétendre à aucune 
indemnisation pour les autres dommages, y compris les dommages spéciaux, indirects ou 
accessoires et pertes de bénéfices.

Cette limitation concerne:
•	tout ce qui est relié au logiciel, aux services ou au contenu (y compris le code) figurant sur 
des sites Internet tiers ou dans des programmes tiers; et
•	les réclamations au titre de violation de contrat ou de garantie, ou au titre de responsabilité 
stricte, de négligence ou d’une autre faute dans la limite autorisée par la loi en vigueur.
Elle s’applique également, même si Microsoft connaissait ou devrait connaître l’éventualité d’un 
tel dommage. Si votre pays n’autorise pas l’exclusion ou la limitation de responsabilité pour les 
dommages indirects, accessoires ou de quelque nature que ce soit, il se peut que la limitation ou 
l’exclusion ci-dessus ne s’appliquera pas à votre égard.

EFFET JURIDIQUE. Le présent contrat décrit certains droits juridiques. Vous pourriez avoir 
d’autres droits prévus par les lois de votre pays. Le présent contrat ne modifie pas les droits que 
vous confèrent les lois de votre pays si celles-ci ne le permettent pas.
```

## Third-party assets

Sample content in the repository that is not Veach's own work.

| Asset | Location | License | Used in |
|---|---|---|---|
| [FlightHelmet](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/4995a638c96dbc94376d3d6164e70fee3ac7f7a2/Models/FlightHelmet) (Khronos glTF Sample Assets) | `apps/lab/assets/flight_helmet/` | CC0 1.0 | Lab scenes and golden tests. |
| [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro), exterior (NVIDIA ORCA) | `apps/lab/assets/bistro/` | CC BY 4.0 (modified) | Lab scenes, goldens and benchmarks. |

### FlightHelmet

Source: https://github.com/KhronosGroup/glTF-Sample-Assets at commit 4995a638c96dbc94376d3d6164e70fee3ac7f7a2, `Models/FlightHelmet`  
License: CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/) for the model files

Khronos's `LICENSE.md` is kept as published next to the model; it licenses the metadata files themselves under CC BY 4.0. See `apps/lab/assets/flight_helmet/README.md` and `apps/lab/assets/flight_helmet/LICENSE.md`.

### Amazon Lumberyard Bistro (exterior)

Source: NVIDIA Open Research Content Archive (ORCA), https://developer.nvidia.com/orca/amazon-lumberyard-bistro, package `Bistro_v5_2.zip` (version 5.2)  
License: CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/); the full legal code is `apps/lab/assets/bistro/LICENSE.txt`

Attribution: "Amazon Lumberyard Bistro, Open Research Content Archive (ORCA)"

```text
@misc{ORCAAmazonBistro,
   title = {Amazon Lumberyard Bistro, Open Research Content Archive (ORCA)},
   author = {Amazon Lumberyard},
   year = {2017},
   month = {July},
   note = {\small \texttt{http://developer.nvidia.com/orca/amazon-lumberyard-bistro}},
   url = {http://developer.nvidia.com/orca/amazon-lumberyard-bistro}
}
```

Changes: this is a modified version. `BistroExterior.fbx` was converted once to glTF with Blender (`tools/assets/convert_bistro.py`). Materials were rebuilt as metallic-roughness from the Specular map (R = occlusion, G = roughness, B = metalness), the green channel of the DirectX normal maps was flipped, alpha-tested materials became `MASK`, textures were re-encoded as JPEG or PNG, and the camera was dropped. See `apps/lab/assets/bistro/README.md`.
