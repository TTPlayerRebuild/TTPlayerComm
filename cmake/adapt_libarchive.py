"""Guarded build-tree changes for libarchive 3.8.9; never change upstream files."""
from pathlib import Path
import shutil
import sys

src, dst = map(Path, sys.argv[1:])
shutil.copytree(src, dst, dirs_exist_ok=True)

def edit(name, before, after, count=1):
    p = dst / name
    text = p.read_text(encoding='utf-8')
    if text.count(before) != count:
        raise SystemExit(f'{name}: expected {count} matches: {before[:80]}')
    p.write_text(text.replace(before, after), encoding='utf-8', newline='\n')

edit('CMakeLists.txt', 'ELSEIF(WINDOWS_VERSION STREQUAL "WIN7")',
     'ELSEIF(WINDOWS_VERSION STREQUAL "WINXP")\n    SET(_WIN32_WINNT 0x0501)\n  ELSEIF(WINDOWS_VERSION STREQUAL "WIN7")')

seven = 'libarchive/archive_read_support_format_7zip.c'
edit(seven, '#include "archive_read_private.h"', '''#include "archive_read_private.h"
/* Private service hooks: limits are scoped to this reader's worker thread. */
extern uint64_t ttpcomm_archive_dictionary_limit(void);
extern size_t ttpcomm_archive_entry_limit(void);
extern int ttpcomm_archive_canceled(void);
extern void *ttpcomm_7z_malloc(size_t);
extern void *ttpcomm_7z_calloc(size_t, size_t);
extern void *ttpcomm_7z_realloc(void *, size_t);
extern void ttpcomm_7z_free(void *);
''')
p = dst / seven
t = p.read_text(encoding='utf-8')
import re
for fn in ('malloc', 'calloc', 'realloc', 'free'):
    t = re.sub(r'\b' + fn + r'\(', 'ttpcomm_7z_' + fn + '(', t)
# liblzma owns this small properties object; it is not a 7zip allocation.
t = t.replace('ttpcomm_7z_free(ff->options)', 'free(ff->options)')
p.write_text(t, encoding='utf-8', newline='\n')
edit(seven, 'r = lzma_raw_decoder(&(zip->lzstream), filters);', '''if (lzma_raw_decoder_memusage(filters) > ttpcomm_archive_dictionary_limit()) {
            free(ff->options);
            archive_set_error(&a->archive, ENOMEM, "7-Zip dictionary limit");
            return (ARCHIVE_FATAL);
        }
        r = lzma_raw_decoder(&(zip->lzstream), filters);''')
edit(seven, '__archive_ppmd7_functions.Ppmd7_Construct(&zip->ppmd7_context);', '''if (msize > ttpcomm_archive_dictionary_limit()) {
            archive_set_error(&a->archive, ENOMEM, "7-Zip dictionary limit");
            return (ARCHIVE_FATAL);
        }
        __archive_ppmd7_functions.Ppmd7_Construct(&zip->ppmd7_context);''')
edit(seven, 'if (parse_7zip_int64(a, &v) < 0 || v > UMAX_ENTRY)',
     'if (parse_7zip_int64(a, &v) < 0 || v > UMAX_ENTRY || v > ttpcomm_archive_entry_limit())')
# Current 7-Zip applies Delta to WAV even with Copy/PPMd. Upstream handles it
# through liblzma for LZMA1/2 only. Preserve byte history across output chunks.
edit(seven, 'uint32_t\t\t bcj_state;', 'uint32_t\t\t bcj_state;\n    uint8_t delta_history[256];\n    unsigned delta_distance, delta_position;')
edit(seven, 'coder2->codec != _7Z_SPARC) {', 'coder2->codec != _7Z_SPARC && coder2->codec != _7Z_DELTA) {')
edit(seven, '''zip->codec2 = coder2->codec;
			zip->bcj_state = 0;''', '''zip->codec2 = coder2->codec;
            if (coder2->codec == _7Z_DELTA) {
                if (coder2->propertiesSize != 1) {
                    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT, "Invalid Delta properties");
                    return ARCHIVE_FATAL;
                }
                zip->delta_distance = 1 + coder2->properties[0];
                zip->delta_position = 0;
                memset(zip->delta_history, 0, sizeof(zip->delta_history));
            }
			zip->bcj_state = 0;''')
edit(seven, '''	/*
	 * Decode BCJ.
	 */''', '''    if (zip->codec2 == _7Z_DELTA && zip->codec != _7Z_LZMA && zip->codec != _7Z_LZMA2) {
        uint8_t *bytes = buff;
        size_t i;
        for (i = 0; i < *outbytes; ++i) {
            bytes[i] += zip->delta_history[(zip->delta_distance + zip->delta_position) & 255];
            zip->delta_history[zip->delta_position-- & 255] = bytes[i];
        }
    }
	/*
	 * Decode BCJ.
	 */''')
edit(seven, '''extract_pack_stream(struct archive_read *a, size_t minimum)
{''', '''extract_pack_stream(struct archive_read *a, size_t minimum)
{
    if (ttpcomm_archive_canceled()) {
        archive_set_error(&a->archive, ECANCELED, "Archive canceled");
        return ARCHIVE_FATAL;
    }''')

gz = 'libarchive/archive_read_support_filter_gzip.c'
edit(gz, '''#if 0
	int hcrc = ((int)p[len + 1] << 8) | (int)p[len];
	int crc = /* XXX TODO: Compute header CRC. */;
	if (crc != hcrc)
		return (0);
	bits += 16;
#endif''', '''#ifdef HAVE_ZLIB_H
        if ((crc32(0, p, (uInt)len) & 0xffff) != archive_le16dec(p + len))
            return (0);
        bits += 16;
#endif''')
edit(gz, '''if (len == 0)
		return (ARCHIVE_EOF);''', '''if (len == 0) {
        const void *tail = __archive_read_filter_ahead(f->upstream, 1, &avail);
        if (!tail && avail == 0) return (ARCHIVE_EOF);
        archive_set_error(&f->archive->archive, ARCHIVE_ERRNO_FILE_FORMAT,
            "Invalid or truncated gzip header");
        return (ARCHIVE_FATAL);
    }''')
edit(gz, '/* XXX TODO: Verify the length and CRC. */', '''if (archive_le32dec(p) != (uint32_t)gzip->crc ||
        archive_le32dec(p + 4) != (uint32_t)gzip->stream.total_out) {
        archive_set_error(&f->archive->archive, ARCHIVE_ERRNO_FILE_FORMAT,
            "Gzip CRC or length mismatch");
        return (ARCHIVE_FATAL);
    }''')
edit(gz, 'ret = inflate(&(gzip->stream), 0);', '''{
            unsigned char *start = gzip->stream.next_out;
            ret = inflate(&(gzip->stream), 0);
            gzip->crc = crc32(gzip->crc, start, (uInt)(gzip->stream.next_out - start));
            if (!ttpcomm_archive_gzip_account((size_t)(gzip->stream.next_out - start))) {
                archive_set_error(&f->archive->archive, ECANCELED, "Archive limit or cancellation");
                return (ARCHIVE_FATAL);
            }
        }''')
edit(gz, '#include "archive_read_private.h"', '#include "archive_read_private.h"\nextern int ttpcomm_archive_gzip_account(size_t);')
# TAR can finish before the gzip trailer. A successful read must verify the
# remaining compressed container too, but cancellation/error cleanup must not.
edit(gz, '#endif /* HAVE_ZLIB_H */', '''extern int ttpcomm_archive_canceled(void);
int ttpcomm_archive_verify_gzip(struct archive *a)
{
    struct archive_read_filter *f = ((struct archive_read *)a)->filter;
    if (f->code != ARCHIVE_FILTER_GZIP) return ARCHIVE_OK;
    while (!((struct gzip *)f->data)->eof) {
        const void *discard;
        if (ttpcomm_archive_canceled()) {
            archive_set_error(a, ECANCELED, "Archive canceled");
            return ARCHIVE_FATAL;
        }
        if (gzip_filter_read(f, &discard) < 0) return ARCHIVE_FATAL;
    }
    return ARCHIVE_OK;
}
#endif /* HAVE_ZLIB_H */''')

# Old GNU/ustar headers have no charset declaration. Prefer valid UTF-8, then
# the user's Windows ANSI codepage; never apply this fallback to PAX UTF-8.
tar = 'libarchive/archive_read_support_format_tar.c'
edit(tar, '#include "archive_read_private.h"', '''#include "archive_read_private.h"
static int ttpcomm_tar_pathname(struct archive_entry *entry,
    const char *name, size_t length, struct archive_string_conv *sc)
{
    size_t n = 0;
    int count;
    wchar_t *wide;
    while (n < length && name[n]) ++n;
    if (n <= 32767 && n &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, (int)n, NULL, 0) == 0 &&
        GetACP() != CP_UTF8) {
        count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, name, (int)n, NULL, 0);
        if (count > 0) {
            wide = (wchar_t *)malloc(((size_t)count + 1) * sizeof(wchar_t));
            if (!wide) { errno = ENOMEM; return -1; }
            if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, name, (int)n, wide, count) == count) {
                wide[count] = 0;
                archive_entry_copy_pathname_w(entry, wide);
                free(wide);
                return 0;
            }
            free(wide);
        }
    }
    return archive_entry_copy_pathname_l(entry, name, length, sc);
}''')
for arguments in [
    'longname.s,\n\t\t    archive_strlen(&longname), tar->sconv',
    '\n\t    header->name, sizeof(header->name), tar->sconv',
    'pathname,\n\t\t    pathname_length, tar->sconv',
    '\n\t\t    header->name, sizeof(header->name), tar->sconv',
]:
    edit(tar, 'archive_entry_copy_pathname_l(entry, ' + arguments + ')',
         'ttpcomm_tar_pathname(entry, ' + arguments + ')') if not arguments.startswith('\n') else edit(
             tar, 'archive_entry_copy_pathname_l(entry,' + arguments + ')',
             'ttpcomm_tar_pathname(entry,' + arguments + ')')
