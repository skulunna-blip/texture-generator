#pragma once

// MinGW compatibility shim for the ONNX Runtime C API.
//
// onnxruntime_c_api.h spells its SAL annotation macros out inline for
// non-Windows builds, but on Windows it expects <specstrings.h> to have
// supplied them. MinGW-w64 does ship a specstrings.h, yet it leaves
// _Frees_ptr_opt_ (among others) undefined, so the header fails to parse.
//
// SAL macros are static-analysis annotations that generate no code, so filling
// in the missing ones as empty is safe. The list, and the object-like vs
// function-like split, mirror what the header defines for non-Windows builds.
// Each is guarded individually: including <windows.h> or a fuller specstrings.h
// first must not turn this into a redefinition error.

#if defined(_WIN32)
#  if !defined(_stdcall)
#    define _stdcall __stdcall // ignored on x64; avoids dragging in <windows.h>
#  endif
#  if !defined(_In_)
#    define _In_
#  endif
#  if !defined(_In_z_)
#    define _In_z_
#  endif
#  if !defined(_In_opt_)
#    define _In_opt_
#  endif
#  if !defined(_In_opt_z_)
#    define _In_opt_z_
#  endif
#  if !defined(_Out_)
#    define _Out_
#  endif
#  if !defined(_Outptr_)
#    define _Outptr_
#  endif
#  if !defined(_Out_opt_)
#    define _Out_opt_
#  endif
#  if !defined(_Inout_)
#    define _Inout_
#  endif
#  if !defined(_Inout_opt_)
#    define _Inout_opt_
#  endif
#  if !defined(_Frees_ptr_opt_)
#    define _Frees_ptr_opt_
#  endif
#  if !defined(_Ret_maybenull_)
#    define _Ret_maybenull_
#  endif
#  if !defined(_Ret_notnull_)
#    define _Ret_notnull_
#  endif
#  if !defined(_Check_return_)
#    define _Check_return_
#  endif
#  if !defined(_Outptr_result_maybenull_)
#    define _Outptr_result_maybenull_
#  endif
#  if !defined(_In_reads_)
#    define _In_reads_(X)
#  endif
#  if !defined(_Inout_updates_)
#    define _Inout_updates_(X)
#  endif
#  if !defined(_Out_writes_)
#    define _Out_writes_(X)
#  endif
#  if !defined(_Inout_updates_all_)
#    define _Inout_updates_all_(X)
#  endif
#  if !defined(_Out_writes_bytes_all_)
#    define _Out_writes_bytes_all_(X)
#  endif
#  if !defined(_Out_writes_all_)
#    define _Out_writes_all_(X)
#  endif
#  if !defined(_Success_)
#    define _Success_(X)
#  endif
#  if !defined(_Outptr_result_buffer_maybenull_)
#    define _Outptr_result_buffer_maybenull_(X)
#  endif
#endif

#include "onnxruntime_c_api.h"
