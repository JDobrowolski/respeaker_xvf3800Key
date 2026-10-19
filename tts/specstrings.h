/*
 * specstrings.h - puste adnotacje SAL (Microsoft) dla TCC. onnxruntime_c_api.h dolacza ten
 * naglowek na Windows; TCC go nie ma, a adnotacje sa tylko dla analizatora kodu MSVC.
 */
#ifndef TTS_SPECSTRINGS_SHIM_H
#define TTS_SPECSTRINGS_SHIM_H

#define _In_
#define _In_z_
#define _In_opt_
#define _In_opt_z_
#define _In_reads_(n)
#define _Inout_
#define _Inout_opt_
#define _Inout_updates_(n)
#define _Inout_updates_all_(n)
#define _Out_
#define _Out_opt_
#define _Out_writes_(n)
#define _Out_writes_all_(n)
#define _Out_writes_bytes_all_(n)
#define _Outptr_
#define _Outptr_result_maybenull_
#define _Outptr_result_buffer_maybenull_(n)
#define _Ret_maybenull_
#define _Ret_notnull_
#define _Check_return_
#define _Success_(x)
#define _Frees_ptr_opt_
#define _Return_type_success_(x)

/* ORT_API_CALL = _stdcall (pisownia MSVC); na x64 jest jedna konwencja wywolan */
#ifndef _stdcall
#define _stdcall
#endif

#endif
