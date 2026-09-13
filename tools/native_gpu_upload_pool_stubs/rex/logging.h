#pragma once
// Original isolated CPU-test shim. Only logging is replaced: the production
// pool, SDK allocator, Windows COM interfaces and WRL ownership remain real.
#ifndef NB_NATIVE_GPU_UPLOAD_POOL_TEST
#error This shim is only for the standalone native GPU upload pool CPU controls.
#endif
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
