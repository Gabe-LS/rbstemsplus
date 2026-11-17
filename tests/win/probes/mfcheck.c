/* T1 check (release.md §3.5): llvm-mingw's mingw-w64 headers and import libraries are enough for
 * the helper's Media Foundation path (mfapi.h, mfidl.h, mfreadwrite.h; mfplat, mfreadwrite,
 * mfuuid). Decodes FILE to 32-bit float PCM with the Source Reader and prints the format, the
 * frame count and an FNV-1a hash of the samples.  mfcheck.exe FILE */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <stdint.h>

#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("%s failed: 0x%08lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) { printf("usage: mfcheck FILE\n"); return 2; }
    CK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    CK(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    IMFSourceReader *rd = NULL;
    CK(MFCreateSourceReaderFromURL(argv[1], NULL, &rd));
    IMFMediaType *want = NULL;
    CK(MFCreateMediaType(&want));
    CK(IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
    CK(IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_Float));
    CK(IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want));
    IMFMediaType *got = NULL;
    CK(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got));
    UINT32 rate = 0, ch = 0, bits = 0;
    IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
    IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    PROPVARIANT dur;
    PropVariantInit(&dur);
    IMFSourceReader_GetPresentationAttribute(rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &dur);
    uint64_t frames = 0, h = 1469598103934665603ULL;
    for (;;) {
        DWORD flags = 0; LONGLONG ts = 0; IMFSample *s = NULL;
        CK(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, &ts, &s));
        if (s) {
            IMFMediaBuffer *b = NULL;
            CK(IMFSample_ConvertToContiguousBuffer(s, &b));
            BYTE *p = NULL; DWORD n = 0;
            CK(IMFMediaBuffer_Lock(b, &p, NULL, &n));
            for (DWORD i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
            frames += n / (ch * (bits / 8));
            IMFMediaBuffer_Unlock(b);
            IMFMediaBuffer_Release(b);
            IMFSample_Release(s);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    printf("rate %u, channels %u, bits %u, duration %.3f s, frames %llu, fnv1a %016llx\n", rate, ch, bits,
           dur.vt == VT_UI8 ? dur.uhVal.QuadPart / 1e7 : -1.0, (unsigned long long)frames, (unsigned long long)h);
    IMFSourceReader_Release(rd);
    MFShutdown();
    return 0;
}
