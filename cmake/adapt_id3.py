"""Build-tree adaptations for the pinned libid3tag source.

Tenacity 0.16.4. Preserve TTPlayer's defaults and serialization according to
6002260D, 6002221B and 600228EB, alongside the newer bounded parsing fixes.
"""
from pathlib import Path
import re
import sys

source, destination = map(Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)

def replace(text, old, new, count=None):
    actual = text.count(old)
    if not actual or (count is not None and actual != count):
        raise RuntimeError(f"Pinned source changed: {old!r}: {actual}")
    return text.replace(old, new)

for path in (source / 'src').iterdir():
    if path.suffix not in ('.c', '.h', '.dat'):
        continue
    text = path.read_text(encoding='utf-8')
    if path.name == 'tag.c':
        # Upstream retains even a tag rejected before frame parsing. TTPlayer
        # rejects invalid headers/CRC, but retains frames on a later frame error.
        text = replace(text, '''/* Return what we've read so far instead of trashing the whole tag
  if (0) {
  fail:
    id3_tag_delete(tag);
    tag = 0;
  }
*/
fail:
''', '''  if (0) {
  fail:
    id3_tag_delete(tag);
    tag = 0;
  }
''', 1)
        text = replace(text, 'tag->version       = ID3_TAG_VERSION;', 'tag->version       = 0x0300;', 1)
        text = replace(text, 'return (length < 128) ? 0 : v1_parse(data);', 'return 0; /* Original ordinal 53 accepts ID3v2 only. */', 1)
        text = replace(text, 'if (frame == 0 || id3_tag_attachframe(tag, frame) == -1)\n        goto fail;', 'if (frame == 0) break;\n      if (id3_tag_attachframe(tag, frame) == -1) {\n        id3_frame_delete(frame);\n        break; /* Retain successfully parsed frames, matching 60022C0E. */\n      }', 1)
        text = replace(text, '/* ID3_TAG_OPTION_UNSYNCHRONISATION | */\n                         ID3_TAG_OPTION_COMPRESSION | ID3_TAG_OPTION_CRC;', 'ID3_TAG_OPTION_APPENDEDTAG;', 1)
        text = replace(text, 'id3_frame_render(tag->frames[i], 0, 0)', 'id3_frame_render(tag->frames[i], 0, tag->version, 0)')
        text = replace(text, 'id3_frame_render(tag->frames[i], ptr, tag->options)', 'id3_frame_render(tag->frames[i], ptr, tag->version, tag->options)')
        text = replace(text, '  if (tag->options & ID3_TAG_OPTION_ID3V1)\n    return v1_render(tag, buffer);\n', '', 1)
        text = replace(text, 'id3_render_int(ptr, ID3_TAG_VERSION, 2)', 'id3_render_int(ptr, tag->version, 2)', 1)
        # Preserve TTPlayer's version-specific extended-header serialization.
        split = text.index('id3_length_t id3_tag_render(')
        head, body = text[:split], text[split:]
        body = replace(body, 'ehsize += id3_render_syncsafe(ptr, 0, 5);', 'ehsize += tag->version < 0x400 ? id3_render_int(ptr, 0, 4) : id3_render_syncsafe(ptr, 0, 5);', 1)
        body = replace(body, 'if (ehsize_ptr)\n      id3_render_syncsafe(&ehsize_ptr, ehsize, 4);', 'if (ehsize_ptr) {\n      if (tag->version < 0x400) id3_render_int(&ehsize_ptr, ehsize, 4);\n      else id3_render_syncsafe(&ehsize_ptr, ehsize, 4);\n    }', 1)
        body = replace(body, 'id3_render_syncsafe(&crc_ptr,\n      id3_crc_compute(frames_ptr, *ptr - frames_ptr), 5);', 'if (tag->version < 0x400)\n      id3_render_int(&crc_ptr, id3_crc_compute(frames_ptr, *ptr - frames_ptr), 4);\n    else\n      id3_render_syncsafe(&crc_ptr, id3_crc_compute(frames_ptr, *ptr - frames_ptr), 5);', 1)
        text = head + body
    elif path.name == 'frame.h':
        text = replace(text, 'id3_byte_t **, int);', 'id3_byte_t **, unsigned int, int);', 1)
    elif path.name == 'frame.c':
        text = replace(text, 'size  = id3_parse_syncsafe(ptr, 4);', 'size  = id3_parse_uint(ptr, 4); /* Original v2.4 parser. */', 1)
        text = replace(text, 'id3_byte_t **ptr, int options)', 'id3_byte_t **ptr, unsigned int version, int options)', 1)
        text = replace(text, 'id3_render_syncsafe(&size_ptr, size - 10, 4);', 'version < 0x400 ? id3_render_int(&size_ptr, size - 10, 4) : id3_render_syncsafe(&size_ptr, size - 10, 4);', 2)
        text = replace(text, 'size += id3_render_syncsafe(ptr, decoded_length, 4);', 'size += version < 0x400 ? id3_render_int(ptr, decoded_length, 4) : id3_render_syncsafe(ptr, decoded_length, 4);', 1)
        start = text.index('      if (flags & ID3_FRAME_FLAG_COMPRESSION) {', text.index('id3_length_t id3_frame_render'))
        end = text.index('\n    }\n  }\n\n  /* unsynchronisation */', start)
        text = text[:start] + '      /* Original 6002221B does not compress the rendered payload. */\n      flags &= ~ID3_FRAME_FLAG_COMPRESSION;\n' + text[end:]
    elif path.name == 'util.c':
        # A final ff 00 consumes both input bytes. The upstream loop then
        # dereferences end once more; retain ff without reading past the buffer.
        text = replace(text, '''  *new++ = *old;

  return new - data;''', '''  if (old < end)
    *new++ = *old;

  return new - data;''', 1)
    elif path.name == 'utf16.c':
        # A terminated APIC description can be followed by one image byte.
        # Upstream's odd-length recovery must not consume that binary byte.
        text = replace(text, '  id3_utf16_t *utf16ptr, *utf16;',
                       '  id3_utf16_t *utf16ptr, *utf16;\n  int terminated = 0;', 1)
        text = replace(text, '''  while (end - *ptr > 0 && (*utf16ptr = id3_utf16_get(ptr, byteorder)))
    ++utf16ptr;''', '''  while (end - *ptr > 0) {
    *utf16ptr = id3_utf16_get(ptr, byteorder);
    if (!*utf16ptr) { terminated = 1; break; }
    ++utf16ptr;
  }''', 1)
        text = replace(text, 'if (end == *ptr && length % 2 != 0)',
                       'if (!terminated && end == *ptr && length % 2 != 0)', 1)
        # Ordinal 54 historically writes encoding=1 in little endian. Preserve
        # that byte stream while retaining the new bounded decoder below.
        text = replace(text, '    default:\n    case ID3_UTF16_BYTEORDER_BE:',
                       '    case ID3_UTF16_BYTEORDER_BE:', 1)
        text = replace(text, '    case ID3_UTF16_BYTEORDER_LE:',
                       '    default:\n    case ID3_UTF16_BYTEORDER_LE:', 1)
    elif path.name == 'field.c':
        # A zero BYTE can be part of a UTF-16 code unit, not a terminator.
        text = replace(text, "while (end - *ptr > 0 && **ptr != '\\0')", 'while (end - *ptr > 0)', 1)
    out = destination / path.name
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')


def write(name, text):
    out = destination / name
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')


header = (source / 'include/id3tag.h.in').read_text(encoding='utf-8')
for component, value in [('MAJOR', '0'), ('MINOR', '16'), ('PATCH', '4')]:
    header = replace(header, f'@PROJECT_VERSION_{component}@', value, 1)
write('id3tag.h', header)

# Generate bounded lookups from upstream declarations using the existing Python
# build dependency. No platform-specific gperf binary or vendored generated C.
# This deliberately accepts only the pinned grammar, not arbitrary gperf input.
for name, expected in [('compat', 73), ('frametype', 84)]:
    text = (source / 'src' / f'{name}.gperf').read_text(encoding='utf-8')
    prologue, rest = text.split('%}', 1)
    if name == 'frametype':
        # TTPlayer stores WXXX URLs as UCS4 string fields (type 4), not Latin1.
        prologue = replace(prologue, '''FIELDS(WXXX) = {
  ID3_FIELD_TYPE_TEXTENCODING,
  ID3_FIELD_TYPE_STRING,
  ID3_FIELD_TYPE_LATIN1
};''', '''FIELDS(WXXX) = {
  ID3_FIELD_TYPE_TEXTENCODING,
  ID3_FIELD_TYPE_STRING,
  ID3_FIELD_TYPE_STRING
};''', 1)
    declaration, entries, *epilogue = rest.split('%%')
    if declaration.strip() != f'struct id3_{name};':
        raise RuntimeError(f'Unexpected {name} declaration')
    rows = []
    for line in entries.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        match = re.fullmatch(r'([A-Z0-9]{3,4}),\s*(.+)', line)
        if not match:
            raise RuntimeError(f'Unexpected {name} row: {line!r}')
        rows.append(match.groups())
    if len(rows) != expected or len({key for key, _ in rows}) != expected:
        raise RuntimeError(f'Unexpected {name} entry count: {len(rows)}')
    rows.sort()
    table = '\n'.join(f'  {{"{key}", {value}}},' for key, value in rows)
    generated = f'''\n/* Generated from upstream {name}.gperf; bounded binary search. */
static struct id3_{name} const ttp_{name}_table[] = {{
{table}
}};
struct id3_{name} const *id3_{name}_lookup(char const *id, size_t length)
{{
  size_t lo = 0, hi = sizeof(ttp_{name}_table) / sizeof(ttp_{name}_table[0]);
  if (!id || length < 3 || length > 4) return 0;
  while (lo < hi) {{
    size_t mid = lo + (hi - lo) / 2;
    char const *key = ttp_{name}_table[mid].id;
    size_t keylen = strlen(key);
    int order = memcmp(id, key, length < keylen ? length : keylen);
    if (!order) order = (length > keylen) - (length < keylen);
    if (!order) return &ttp_{name}_table[mid];
    if (order < 0) hi = mid;
    else lo = mid + 1;
  }}
  return 0;
}}
'''
    if not prologue.startswith('%{'):
        raise RuntimeError(f'Unexpected {name} prologue')
    write(f'{name}.c', prologue[2:] + generated + ''.join(epilogue))

# Preserve the original 148-entry genre interpretation and five old spellings;
# indices 148..191 must not acquire a different meaning after an upgrade.
genre_source = (source / 'src/genre.dat.in').read_text(encoding='utf-8')
genres = [line for line in genre_source.splitlines() if re.match('[a-zA-Z]', line)]
if len(genres) != 192:
    raise RuntimeError('Unexpected upstream genre table')
genres = genres[:148]
for index, value in {85: 'Bebob', 123: 'A Cappella', 129: 'Hardcore', 133: 'Negerpunk', 146: 'JPop'}.items():
    genres[index] = value
data = genre_source[:genre_source.index('*/') + 2] + '\n/* TTPlayer compatibility table generated at build time. */\n'
for i, value in enumerate(genres):
    data += f'static id3_ucs4_t const genre_{i}[] = {{' + ','.join(str(ord(c)) for c in value) + ',0};\n'
data += 'static id3_ucs4_t const *const genre_table[] = {\n' + ',\n'.join(f'genre_{i}' for i in range(len(genres))) + '\n};\n'
write('genre.dat', data)
