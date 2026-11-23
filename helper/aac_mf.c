/* AAC in an MP4 through Media Foundation: the Windows counterpart of aac.c (helper.md §0 1, §2,
 * task T10), set up as rekordbox for Windows' FileReadMp4 sets it up (rb::WindowsMediaAudioFormat,
 * Pioneer's fork of JUCE's): a Source Reader over a byte stream with an async callback, a new media
 * type of major type Audio and subtype PCM and no bit depth on the last audio stream, which is
 * then the only one selected, read in order from the start. The decoder is Microsoft's AAC
 * decoder MFT (msauddecmft.dll), an OS component, giving its default 16-bit PCM.
 *
 * The byte stream: MFCreateMFByteStreamOnStream wraps a seekable IStream that reads on demand
 * from a duplicate of the track handle used for the header probe.
 *
 * Media Foundation is loaded at run time, by full System32 path (mf_load), never a static import:
 * on a Windows N edition without the Media Feature Pack the exes still start, and only AAC fails.
 *
 * Q3 (helper.md §2, measured in the VM, the numbers there): one output per file, whatever the
 * thread, the read mode or the stack's alignment, so aac_decode_all decodes once. It returns
 * whole decoded packets from frame zero. decode.c applies the sample-table length to the track. */
#define COBJMACROS
#include <windows.h>
#include <winver.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbsp.h"
#include "big.h"
#include "plat.h"
#include "plat_helper.h"
#include "aac_mf.h"
#include "mf_guids.h"
#include "readable.h"

#ifdef RBSP_TEST_FORMAT_RATES
int rb_aac_test_unproved;
int rb_rates_test_unproved;
#endif

#define RBMF_DEFINE(n, l, w1, w2, b0, b1, b2, b3, b4, b5, b6, b7) \
    static const GUID rb_##n = {l, w1, w2, {b0, b1, b2, b3, b4, b5, b6, b7}};
RBMF_GUIDS(RBMF_DEFINE)
#undef RBMF_DEFINE

/* ---------------------------------------------------------------- Media Foundation, loaded at run time */

/* mfplat.dll's MFCreateMFByteStreamOnStream (Windows 7 on) isn't declared by mingw-w64's mfidl.h:
 * Microsoft's declaration. The others' types come from the headers' own declarations */
typedef HRESULT(STDAPICALLTYPE *mf_bs_on_stream_fn)(IStream *pStream, IMFByteStream **ppByteStream);
static struct {
    __typeof__(&MFStartup) startup;
    __typeof__(&MFShutdown) shutdown;
    __typeof__(&MFCreateAttributes) create_attributes;
    __typeof__(&MFCreateMediaType) create_media_type;
    mf_bs_on_stream_fn bs_on_stream;
    __typeof__(&MFCreateSourceReaderFromByteStream) reader_from_bs;
    int ok;
} mf;

/* a function pointer from GetProcAddress (FARPROC), without a cast between function types */
#define MF_PROC(dst, mod, name)                     \
    do {                                            \
        FARPROC f_ = GetProcAddress((mod), (name)); \
        memcpy(&(dst), &f_, sizeof f_);             \
    } while (0)

static int mf_system_path(wchar_t path[MAX_PATH + 32], const wchar_t *name) {
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    size_t k = wcslen(name);
    if (n == 0 || n >= MAX_PATH || k > 30) return 0;
    path[n] = L'\\';
    memcpy(path + n + 1, name, (k + 1) * sizeof *name);
    return 1;
}

/* System32's DLL by its full path, never the search order; never freed: it serves the process */
static HMODULE mf_dll(const wchar_t *name) {
    wchar_t path[MAX_PATH + 32];
    return mf_system_path(path, name) ? LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
}

static char decoder_version[AAC_MF_VERSION_MAX];

static void read_decoder_version(void) {
    wchar_t path[MAX_PATH + 32];
    if (!mf_system_path(path, L"msauddecmft.dll")) return;
    HMODULE version = mf_dll(L"version.dll");
    if (!version) return;

    __typeof__(&GetFileVersionInfoSizeW) get_size;
    __typeof__(&GetFileVersionInfoW) get_info;
    __typeof__(&VerQueryValueW) query;
    MF_PROC(get_size, version, "GetFileVersionInfoSizeW");
    MF_PROC(get_info, version, "GetFileVersionInfoW");
    MF_PROC(query, version, "VerQueryValueW");
    if (!get_size || !get_info || !query) return;

    DWORD unused, size = get_size(path, &unused);
    void *data = size ? malloc(size) : NULL;
    if (!data) return;
    void *value = NULL;
    UINT len = 0;
    if (get_info(path, 0, size, data) && query(data, L"\\", &value, &len) && value && len >= sizeof(VS_FIXEDFILEINFO)) {
        const VS_FIXEDFILEINFO *v = value;
        if (v->dwSignature == VS_FFI_SIGNATURE)
            snprintf(decoder_version, sizeof decoder_version, "%u.%u.%u.%u",
                     (unsigned)HIWORD(v->dwFileVersionMS), (unsigned)LOWORD(v->dwFileVersionMS),
                     (unsigned)HIWORD(v->dwFileVersionLS), (unsigned)LOWORD(v->dwFileVersionLS));
    }
    free(data);
}

const char *aac_mf_decoder_version(void) {
    static plat_once_t once = PLAT_ONCE_INIT;
    plat_once(&once, read_decoder_version);
    return decoder_version;
}

static void mf_stop(void) { mf.shutdown(); }

static void mf_load(void) {
    HMODULE p = mf_dll(L"mfplat.dll"), r = p ? mf_dll(L"mfreadwrite.dll") : NULL;
    if (!r) return;                      /* a Windows N edition without the Media Feature Pack */
    MF_PROC(mf.startup, p, "MFStartup");
    MF_PROC(mf.shutdown, p, "MFShutdown");
    MF_PROC(mf.create_attributes, p, "MFCreateAttributes");
    MF_PROC(mf.create_media_type, p, "MFCreateMediaType");
    MF_PROC(mf.bs_on_stream, p, "MFCreateMFByteStreamOnStream");
    MF_PROC(mf.reader_from_bs, r, "MFCreateSourceReaderFromByteStream");
    if (!mf.startup || !mf.shutdown || !mf.create_attributes || !mf.create_media_type || !mf.bs_on_stream || !mf.reader_from_bs
        || FAILED(mf.startup(MF_VERSION, MFSTARTUP_NOSOCKET))) return;
    if (atexit(mf_stop)) { mf_stop(); return; }
    mf.ok = 1;
}

int aac_mf_available(char *why, size_t whyn) {
    static plat_once_t once = PLAT_ONCE_INIT;
    plat_once(&once, mf_load);
    if (mf.ok) return 1;
    snprintf(why, whyn, "Media Foundation isn't there (a Windows N edition without the Media Feature Pack?)");
    return 0;
}

/* One sample's read, async: waited for in 1 s slices, at most READ_WAIT_MS / 1000 of them, so a
 * process suspended (NtSuspendProcess) partway through a read costs one slice, not the read. A soft
 * guard against a decoder that never answers: the app's Job is the hard bound on the helper. */
#define READ_WAIT_MS 120000

/* ---------------------------------------------------------------- the async callback */

typedef struct {
    IMFSourceReaderCallback cb;          /* first: the object is its interface */
    volatile LONG refs;
    HANDLE done;                         /* auto-reset: one OnReadSample */
    HRESULT hr;
    DWORD flags;
    LONGLONG ts;
    IMFSample *sample;
} reader_cb;

static ULONG STDMETHODCALLTYPE cb_addref(IMFSourceReaderCallback *t) { return (ULONG)InterlockedIncrement(&((reader_cb *)t)->refs); }

static ULONG STDMETHODCALLTYPE cb_release(IMFSourceReaderCallback *t) {
    reader_cb *c = (reader_cb *)t;
    LONG n = InterlockedDecrement(&c->refs);
    if (!n) {
        if (c->sample) IMFSample_Release(c->sample);
        CloseHandle(c->done);
        free(c);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE cb_qi(IMFSourceReaderCallback *t, REFIID r, void **o) {
    if (IsEqualGUID(r, &rb_IID_IUnknown) || IsEqualGUID(r, &rb_IID_IMFSourceReaderCallback)) {
        *o = t;
        cb_addref(t);
        return S_OK;
    }
    *o = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE cb_read(IMFSourceReaderCallback *t, HRESULT hr, DWORD stream, DWORD flags, LONGLONG ts,
                                         IMFSample *s) {
    (void)stream;
    reader_cb *c = (reader_cb *)t;
    c->hr = hr;
    c->flags = flags;
    c->ts = ts;
    if (s) IMFSample_AddRef(s);
    c->sample = s;
    SetEvent(c->done);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_flush(IMFSourceReaderCallback *t, DWORD stream) {
    (void)t, (void)stream;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_event(IMFSourceReaderCallback *t, DWORD stream, IMFMediaEvent *e) {
    (void)t, (void)stream, (void)e;
    return S_OK;
}

static IMFSourceReaderCallbackVtbl cb_vtbl = {cb_qi, cb_addref, cb_release, cb_read, cb_flush, cb_event};

static reader_cb *cb_new(void) {
    reader_cb *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->cb.lpVtbl = &cb_vtbl;
    c->refs = 1;
    c->done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!c->done) { free(c); return NULL; }
    return c;
}

/* ---------------------------------------------------------------- the byte stream */

typedef struct {
    IStream stm;
    volatile LONG refs;
    plat_file file;
    plat_mutex lock;                     /* serializes the cursor and reads on the synchronous handle */
    uint64_t pos, size;
} file_stream;

static ULONG STDMETHODCALLTYPE stream_addref(IStream *t) {
    return (ULONG)InterlockedIncrement(&((file_stream *)t)->refs);
}

static ULONG STDMETHODCALLTYPE stream_release(IStream *t) {
    file_stream *s = (file_stream *)t;
    LONG n = InterlockedDecrement(&s->refs);
    if (!n) {
        plat_file_close(s->file);
        plat_mutex_destroy(&s->lock);
        free(s);
    }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE stream_qi(IStream *t, REFIID id, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID(id, &rb_IID_IUnknown) && !IsEqualGUID(id, &rb_IID_ISequentialStream)
        && !IsEqualGUID(id, &rb_IID_IStream)) return E_NOINTERFACE;
    *out = t;
    stream_addref(t);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_read(IStream *t, void *buf, ULONG count, ULONG *out) {
    file_stream *s = (file_stream *)t;
    ULONG total = 0;
    if (out) *out = 0;
    if (count && !buf) return E_POINTER;
    int err = 0;
    plat_mutex_lock(&s->lock);
    while (total < count && s->pos < s->size) {
        size_t want = count - total, got;
        if (want > s->size - s->pos) want = (size_t)(s->size - s->pos);
        err = readable_watch_pread(s->file, (char *)buf + total, want, (int64_t)s->pos, &got);
        if (err || !got) break;
        s->pos += got;
        total += (ULONG)got;
    }
    plat_mutex_unlock(&s->lock);
    if (out) *out = total;
    return err ? STG_E_READFAULT : total == count ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE stream_seek(IStream *t, LARGE_INTEGER move, DWORD from, ULARGE_INTEGER *out) {
    file_stream *s = (file_stream *)t;
    if (from != STREAM_SEEK_SET && from != STREAM_SEEK_CUR && from != STREAM_SEEK_END)
        return STG_E_INVALIDFUNCTION;
    uint64_t delta = (uint64_t)move.QuadPart;
    plat_mutex_lock(&s->lock);
    uint64_t base = from == STREAM_SEEK_END ? s->size : from == STREAM_SEEK_CUR ? s->pos : 0;
    /* SET uses an unsigned offset. CUR and END add a signed displacement. */
    if (from != STREAM_SEEK_SET && (move.QuadPart < 0 ? base < 0 - delta : base > UINT64_MAX - delta)) {
        plat_mutex_unlock(&s->lock);
        return STG_E_INVALIDFUNCTION;
    }
    s->pos = base + delta;
    if (out) out->QuadPart = s->pos;
    plat_mutex_unlock(&s->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_stat(IStream *t, STATSTG *st, DWORD flags) {
    (void)flags;
    if (!st) return E_POINTER;
    memset(st, 0, sizeof *st);
    st->type = STGTY_STREAM;
    st->cbSize.QuadPart = ((file_stream *)t)->size;
    st->grfMode = STGM_READ;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_write(IStream *t, const void *buf, ULONG n, ULONG *out) {
    (void)t, (void)buf, (void)n;
    if (out) *out = 0;
    return STG_E_ACCESSDENIED;
}
static HRESULT STDMETHODCALLTYPE stream_resize(IStream *t, ULARGE_INTEGER n) {
    (void)t, (void)n;
    return STG_E_ACCESSDENIED;
}
static HRESULT STDMETHODCALLTYPE stream_copy(IStream *t, IStream *into, ULARGE_INTEGER n,
                                              ULARGE_INTEGER *read, ULARGE_INTEGER *written) {
    (void)t, (void)into, (void)n;
    if (read) read->QuadPart = 0;
    if (written) written->QuadPart = 0;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE stream_commit(IStream *t, DWORD flags) { (void)t, (void)flags; return S_OK; }
static HRESULT STDMETHODCALLTYPE stream_revert(IStream *t) { (void)t; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE stream_region(IStream *t, ULARGE_INTEGER off, ULARGE_INTEGER n, DWORD type) {
    (void)t, (void)off, (void)n, (void)type;
    return STG_E_INVALIDFUNCTION;
}
static HRESULT STDMETHODCALLTYPE stream_clone(IStream *t, IStream **out) {
    (void)t;
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static IStreamVtbl stream_vtbl = {
    stream_qi, stream_addref, stream_release, stream_read, stream_write, stream_seek, stream_resize, stream_copy,
    stream_commit, stream_revert, stream_region, stream_region, stream_stat, stream_clone
};

/* Give the stream a duplicate of the probed handle so its lifetime follows MF's references. */
static IMFByteStream *open_stream(plat_file f, char *why, size_t whyn) {
    plat_stat st;
    if (plat_file_stat(f, &st) || st.size <= 0) {
        snprintf(why, whyn, "an unusual file size for an MP4");
        return NULL;
    }
    file_stream *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(why, whyn, "not enough memory to read it");
        return NULL;
    }
    s->stm.lpVtbl = &stream_vtbl;
    s->refs = 1;
    HANDLE process = GetCurrentProcess();
    if (!DuplicateHandle(process, f.h, process, &s->file.h, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        snprintf(why, whyn, "the file handle can't be duplicated");
        free(s);
        return NULL;
    }
    s->size = (uint64_t)st.size;
    plat_mutex_init(&s->lock);
    readable_watch_stream(s->file);
    IMFByteStream *bs = NULL;
    if (FAILED(mf.bs_on_stream(&s->stm, &bs))) { snprintf(why, whyn, "Media Foundation can't read it"); bs = NULL; }
    IStream_Release(&s->stm);
    return bs;
}

/* ---------------------------------------------------------------- one decode */

static void lower_ext(const char *path, char *ext, size_t n) {
    const char *dot = strrchr(path, '.'), *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    ext[0] = 0;
    if (!dot || (a && dot < a) || (b && dot < b)) return;
    size_t i = 0;
    for (; dot[i] && i + 1 < n; i++) ext[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
    ext[i] = 0;
}

/* the next sample of stream st: blocking, or through the callback */
static HRESULT next_sample(IMFSourceReader *r, DWORD st, reader_cb *cb, DWORD *flags, LONGLONG *ts, IMFSample **s) {
    *s = NULL;
    if (!cb) return IMFSourceReader_ReadSample(r, st, 0, NULL, flags, ts, s);
    HRESULT h = IMFSourceReader_ReadSample(r, st, 0, NULL, NULL, NULL, NULL);
    if (FAILED(h)) return h;
    for (int slices = 0;;) {
        DWORD w = WaitForSingleObject(cb->done, 1000);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_TIMEOUT || ++slices >= READ_WAIT_MS / 1000) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    *flags = cb->flags;
    *ts = cb->ts;
    *s = cb->sample;
    cb->sample = NULL;
    return cb->hr;
}

/* The current type's query result and, on success, its rate, channels and bit depth. */
static HRESULT current_pcm(IMFSourceReader *r, DWORD st, aac_mf_info *in) {
    IMFMediaType *t = NULL;
    HRESULT h = IMFSourceReader_GetCurrentMediaType(r, st, &t);
    if (FAILED(h)) return h;
    UINT32 v;
    in->rate = SUCCEEDED(IMFMediaType_GetUINT32(t, &rb_MF_MT_AUDIO_SAMPLES_PER_SECOND, &v)) ? (int)v : 0;
    in->ch = SUCCEEDED(IMFMediaType_GetUINT32(t, &rb_MF_MT_AUDIO_NUM_CHANNELS, &v)) ? (int)v : 0;
    in->bits = SUCCEEDED(IMFMediaType_GetUINT32(t, &rb_MF_MT_AUDIO_BITS_PER_SAMPLE, &v)) ? (int)v : 0;
    IMFMediaType_Release(t);
    return h;
}

/* A NULL PCM result probes the selected stream through the first PCM frame. */
__attribute__((noinline)) static int decode_once(plat_file f, const char *ext, int async, int expected_stream, int16_t **pcm, size_t *frames,
                                                 aac_mf_info *in, char *why, size_t whyn) {
    memset(in, 0, sizeof *in);
    in->duration = -1;
    if (pcm) *pcm = NULL;
    if (frames) *frames = 0;
    if (!aac_mf_available(why, whyn)) return 1;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
        snprintf(why, whyn, "Windows couldn't initialize COM (0x%08lx)", (unsigned long)(uint32_t)co);
        return (int)co;
    }
    int rc = 1;
    IMFByteStream *bs = NULL;
    IMFAttributes *a = NULL;
    IMFSourceReader *r = NULL;
    IMFMediaType *t = NULL;
    reader_cb *cb = NULL;
    int16_t *buf = NULL;
    size_t cap = 0, n = 0;
    HRESULT h;
    if (!(bs = open_stream(f, why, whyn))) goto done;
    if (async) {
        if (!(cb = cb_new()) || FAILED(mf.create_attributes(&a, 1))
            || FAILED(IMFAttributes_SetUnknown(a, &rb_MF_SOURCE_READER_ASYNC_CALLBACK, (IUnknown *)&cb->cb))) {
            snprintf(why, whyn, "not enough memory to decode it");
            goto done;
        }
    }
    h = mf.reader_from_bs(bs, a, &r);
    if (h == MF_E_UNSUPPORTED_BYTESTREAM_TYPE) {
        /* no type found from the bytes alone: name it, as a file's extension would */
        IMFAttributes *ba = NULL;
        if (SUCCEEDED(IMFByteStream_QueryInterface(bs, &rb_IID_IMFAttributes, (void **)&ba))) {
            IMFAttributes_SetString(ba, &rb_MF_BYTESTREAM_CONTENT_TYPE, strcmp(ext, ".mp4") ? L"audio/mp4" : L"video/mp4");
            IMFAttributes_Release(ba);
            in->typed = 1;
            h = mf.reader_from_bs(bs, a, &r);
        }
    }
    if (FAILED(h)) { snprintf(why, whyn, "Media Foundation can't open it as an MP4 (0x%08lx)", (unsigned long)h); r = NULL; goto done; }
    /* rekordbox's loop: the last audio stream found */
    IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    int last = -1;
    GUID sub = {0};
    for (DWORD i = 0; i < (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM; i++) {
        h = IMFSourceReader_GetNativeMediaType(r, i, 0, &t);
        if (h == MF_E_INVALIDSTREAMNUMBER) break;
        in->streams = (int)i + 1;
        if (h == MF_E_NO_MORE_TYPES) continue;
        if (FAILED(h)) {
            snprintf(why, whyn, "Media Foundation can't enumerate the MP4's streams (0x%08lx)", (unsigned long)h);
            goto done;
        }
        GUID g;
        UINT32 v;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &rb_MF_MT_MAJOR_TYPE, &g)) && IsEqualGUID(&g, &rb_MFMediaType_Audio)) {
            last = (int)i;
            if (FAILED(IMFMediaType_GetGUID(t, &rb_MF_MT_SUBTYPE, &sub))) memset(&sub, 0, sizeof sub);
            in->native_sub = (uint32_t)sub.Data1;
            in->native_rate = SUCCEEDED(IMFMediaType_GetUINT32(t, &rb_MF_MT_AUDIO_SAMPLES_PER_SECOND, &v)) ? (int)v : 0;
            in->native_ch = SUCCEEDED(IMFMediaType_GetUINT32(t, &rb_MF_MT_AUDIO_NUM_CHANNELS, &v)) ? (int)v : 0;
        }
        IMFMediaType_Release(t);
        t = NULL;
    }
    if (last < 0) { snprintf(why, whyn, "no sound track Media Foundation reads"); goto done; }
    in->stream = last;
    if (expected_stream >= 0 && last != expected_stream) {
        snprintf(why, whyn, "Media Foundation's last sound stream changed after probing");
        goto done;
    }
    if (!IsEqualGUID(&sub, &rb_MFAudioFormat_AAC)) {
        if (!pcm) { rc = 0; goto done; }  /* the parser may handle another codec, such as ALAC */
        snprintf(why, whyn, "the sound track Media Foundation reads isn't AAC (subtype 0x%08lx)", (unsigned long)sub.Data1);
        goto done;
    }
    if (FAILED(mf.create_media_type(&t)) || FAILED(IMFMediaType_SetGUID(t, &rb_MF_MT_MAJOR_TYPE, &rb_MFMediaType_Audio))
        || FAILED(IMFMediaType_SetGUID(t, &rb_MF_MT_SUBTYPE, &rb_MFAudioFormat_PCM))) {
        snprintf(why, whyn, "not enough memory to decode it");
        goto done;
    }
    if (FAILED(h = IMFSourceReader_SetCurrentMediaType(r, (DWORD)last, NULL, t))) {
        snprintf(why, whyn, "Windows' AAC decoder doesn't decode it (0x%08lx)", (unsigned long)h);
        goto done;
    }
    IMFSourceReader_SetStreamSelection(r, (DWORD)last, TRUE);
    if (FAILED(current_pcm(r, (DWORD)last, in)) || in->bits != 16 || in->ch < 1 || in->ch > 8 || in->rate < 1) {
        snprintf(why, whyn, "Windows' AAC decoder gives %d-bit, %d-channel, %d Hz samples", in->bits, in->ch, in->rate);
        goto done;
    }
    if (pcm) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(r, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &rb_MF_PD_DURATION, &pv))
            && pv.vt == VT_UI8)
            in->duration = (long long)pv.uhVal.QuadPart;
        PropVariantClear(&pv);
    }
    size_t fb = 2 * (size_t)in->ch;      /* bytes per frame */
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = NULL;
        h = next_sample(r, (DWORD)last, cb, &flags, &ts, &s);
        if (FAILED(h) || (flags & MF_SOURCE_READERF_ERROR)) {
            if (s) IMFSample_Release(s);
            snprintf(why, whyn, "Windows' AAC decoder stopped with an error (0x%08lx)", (unsigned long)h);
            goto done;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            /* A sample delivered with the flag is already in the new type. */
            in->typechanges++;
            aac_mf_info now = *in;
            HRESULT type_hr = current_pcm(r, (DWORD)last, &now);
#ifdef RBSP_TEST_FORMAT_RATES
            if (rb_aac_test_unproved && ((in->native_rate == 22050 && in->native_ch == 2) ||
                                         (in->rate == 22050 && in->ch == 2))) {
                char after[64] = "unavailable";
                if (SUCCEEDED(type_hr))
                    snprintf(after, sizeof after, "%d Hz/%d ch/%d bit", now.rate, now.ch, now.bits);
                fprintf(stderr, "AAC MF type change: stream=%d native=0x%04lx/%d Hz/%d ch "
                        "PCM before=%d Hz/%d ch/%d bit after=%s type_hr=0x%08lx flags=0x%lx frames=%zu sample=%d\n",
                        last, (unsigned long)in->native_sub, in->native_rate, in->native_ch,
                        in->rate, in->ch, in->bits, after, (unsigned long)(uint32_t)type_hr, (unsigned long)flags, n, s != NULL);
            }
#endif
            /* MF assumes implicit SBR for the observed 22.05 kHz stereo start.
             * The 2026-11-22 AAC rows were proved with any start that settles to the header
             * rate and channels before PCM. */
            int same = now.rate == in->rate && now.ch == in->ch && now.bits == in->bits;
            int proved_start = (in->native_rate == 22050 && in->native_ch == 2 && in->rate == 44100 && in->ch == 2) ||
                               ((in->native_rate == 16000 || in->native_rate == 24000) && in->native_ch == 2) ||
                               (in->native_rate == 32000 && in->native_ch == 1);
            int settles = !n && now.rate == in->native_rate && now.ch == in->native_ch && now.bits == 16;
            int frame0 = settles && proved_start;
#ifdef RBSP_TEST_FORMAT_RATES
            if (rb_rates_test_unproved && settles && rb_aac_candidate_rate(in->native_rate, in->native_ch))
                frame0 = 1;
            if (!n && rb_aac_test_unproved) frame0 = 1;   /* research: observe other startup changes */
#endif
            if (FAILED(type_hr) || now.bits != 16 || now.ch < 1 || now.ch > 8 || now.rate < 1 || (!same && !frame0)) {
                if (s) IMFSample_Release(s);
                snprintf(why, whyn, "the sound changes format partway (%d Hz, %d channels)", now.rate, now.ch);
                goto done;
            }
            *in = now;
            fb = 2 * (size_t)in->ch;
        }
        if (flags & MF_SOURCE_READERF_STREAMTICK) in->gaps++;
        if (s) {
            IMFMediaBuffer *b = NULL;
            BYTE *p;
            DWORD len = 0;
            h = IMFSample_ConvertToContiguousBuffer(s, &b);
            if (SUCCEEDED(h)) h = IMFMediaBuffer_Lock(b, &p, NULL, &len);
            if (FAILED(h) || len % fb) {
                if (SUCCEEDED(h)) IMFMediaBuffer_Unlock(b);
                if (b) IMFMediaBuffer_Release(b);
                IMFSample_Release(s);
                snprintf(why, whyn, "Windows' AAC decoder gave a broken sample (0x%08lx, %lu bytes)", (unsigned long)h, (unsigned long)len);
                goto done;
            }
            size_t add = len / fb;
            if (!in->samples) in->first_ts = ts;
            long long want = in->first_ts + (long long)((double)n * 1e7 / in->rate + 0.5);
            if (ts - want > 10000 || want - ts > 10000) in->gaps++;
            in->last_ts = ts;
            in->samples++;
            if (pcm && add) {
                if (n + add > cap) {
                    size_t nc = cap ? cap : (size_t)1 << 20;
                    while (nc < n + add) nc *= 2;
                    int16_t *nb = buf ? big_realloc(buf, nc * fb) : big_alloc(nc * fb);
                    if (!nb) {
                        IMFMediaBuffer_Unlock(b);
                        IMFMediaBuffer_Release(b);
                        IMFSample_Release(s);
                        snprintf(why, whyn, "not enough memory to decode it");
                        goto done;
                    }
                    buf = nb;
                    cap = nc;
                }
                memcpy(buf + n * (size_t)in->ch, p, add * fb);
            }
            n += add;
            IMFMediaBuffer_Unlock(b);
            IMFMediaBuffer_Release(b);
            IMFSample_Release(s);
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) || (!pcm && n)) break;
    }
    if (!n) { snprintf(why, whyn, "Windows' AAC decoder gave no samples"); goto done; }
    if (pcm) {
        *pcm = buf;
        *frames = n;
        buf = NULL;
    }
    rc = 0;
done:
    if (buf) big_free(buf);
    if (t) IMFMediaType_Release(t);
    if (r) IMFSourceReader_Release(r);
    if (a) IMFAttributes_Release(a);
    if (cb) cb_release(&cb->cb);
    if (bs) IMFByteStream_Release(bs);
    if (SUCCEEDED(co)) CoUninitialize();
    return rc;
}

/* the decode, `shift` bytes lower on the stack */
__attribute__((noinline)) static int decode_shifted(plat_file f, const char *ext, size_t shift, int16_t **pcm, size_t *frames,
                                                    aac_mf_info *in, char *why, size_t whyn) {
    volatile char *p = __builtin_alloca(shift + 1);
    p[0] = 0;                            /* the alloca stays: it is used */
    int rc = decode_once(f, ext, 0, -1, pcm, frames, in, why, whyn);
    __asm__ volatile("" : : "r"(p) : "memory");
    return rc;
}

int aac_mf_probe(plat_file f, const char *ext, aac_mf_info *info, char *why, size_t whyn) {
    return decode_once(f, ext, 1, -1, NULL, NULL, info, why, whyn);
}

int aac_mf_decode(const char *path, int async, size_t shift, int16_t **pcm, size_t *frames, aac_mf_info *info, char *why,
                  size_t whyn) {
    *pcm = NULL;
    *frames = 0;
    plat_file f;
    if (plat_track_open(path, &f)) { snprintf(why, whyn, "the file can't be read"); return 1; }
    char ext[8];
    lower_ext(path, ext, sizeof ext);
    int rc = async || !shift ? decode_once(f, ext, async, -1, pcm, frames, info, why, whyn)
                             : decode_shifted(f, ext, shift, pcm, frames, info, why, whyn);
    plat_file_close(f);
    return rc;
}

/* ---------------------------------------------------------------- rbsp.h's interface */

void aac_outputs_free(aac_outputs *o) {
    for (int i = 0; i < o->count; i++) big_free(o->pcm[i]);
    o->count = 0;
}

int aac_decode_all(plat_file f, const char *ext, int stream, aac_outputs *o, char *why, size_t whyn) {
    memset(o, 0, sizeof *o);
    aac_mf_info in;
    if (decode_once(f, ext, 1, stream, &o->pcm[0], &o->frames[0], &in, why, whyn)) return 1;
    o->count = 1;
    o->rate = in.rate;
    o->ch = in.ch;
    return 0;
}
