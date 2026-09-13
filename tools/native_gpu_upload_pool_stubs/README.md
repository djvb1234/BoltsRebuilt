These isolated original CPU controls compile the actual
`src/gpu/native/native_gpu_upload_pool.cpp` and the SDK's actual
`src/ui/graphics_upload_buffer_pool.cpp`. The only substituted SDK header is
`rex/logging.h`; it removes the runtime logging dependency. Windows D3D12
interfaces, WRL ownership and allocator assertions remain real. Complete mock
device/resource vtables reject unexpected calls. No D3D12 device is created and
no GPU work, process attachment, SDK mutation or runtime DLL loading occurs.

The root agent owns compilation and execution under its machine lock. From an
x64 Visual Studio developer shell, in a unique scratch output directory:

```powershell
cl /nologo /std:c++20 /EHsc /W4 /O2 /UNDEBUG /DNB_NATIVE_GPU_UPLOAD_POOL_TEST /DNOMINMAX /DWIN32_LEAN_AND_MEAN /IC:\rex\nb\tools\native_gpu_upload_pool_stubs /IC:\rex\nb\src\gpu /IC:\rex\sdk\include C:\rex\nb\tools\test_native_gpu_upload_pool.cpp C:\rex\nb\src\gpu\native\native_gpu_upload_pool.cpp C:\rex\sdk\src\ui\graphics_upload_buffer_pool.cpp /Fe:native_gpu_upload_pool_cpu.exe
.\native_gpu_upload_pool_cpu.exe
```

Do **not** add `NDEBUG`: the old failed-retirement bug occurs in an SDK assertion
expression. The source rejects assertion-disabled builds. The seven control
groups cover exact resource/CPU/GPU slice identity, literal unchanged bytes,
reclaim boundaries, alignment padding, support and budget fallback, GPU
create/map/null-map fallback latching, complete optional allocation failure after
page retirement and retry, host allocation exceptions at the factory and page
boundary, terminal shutdown and deliberate COM-reference retention. Retention
tests drain mock references only after proving the pool did not release them;
this is fixture cleanup, not permission to release real resources without GPU
completion proof.

The fault injector replaces ordinary global `operator new` only in this
single-threaded test executable and arms a one-shot countdown around specific
production calls. It does not modify production or SDK allocation code. The
controls do not validate actual adapter support, GPU-visible coherence, GPU
completion or the command processor's shutdown proof.
