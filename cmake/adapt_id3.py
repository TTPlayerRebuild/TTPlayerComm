"""Build-tree adaptations for the pinned libid3tag source.

The original DLL modified defaults and serialisation. These changes follow
6002260D, 6002221B and 600228EB, not upstream's ID3V2_3 convenience option.
"""
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)

def replace(text, old, new, count=None):
    actual = text.count(old)
    if not actual or (count is not None and actual != count):
        raise RuntimeError(f"Pinned source changed: {old!r}: {actual}")
    return text.replace(old, new)

for path in source.iterdir():
    if path.suffix not in ('.c', '.h', '.dat'):
        continue
    text = path.read_text(encoding='utf-8')
    if path.name == 'tag.c':
        text = replace(text, 'tag->version       = ID3_TAG_VERSION;', 'tag->version       = 0x0300;', 1)
        text = replace(text, 'return (length < 128) ? 0 : v1_parse(data);', 'return 0; /* Original ordinal 53 accepts ID3v2 only. */', 1)
        text = replace(text, 'if (frame == 0 || id3_tag_attachframe(tag, frame) == -1)\n\tgoto fail;', 'if (frame == 0) break;\n      if (id3_tag_attachframe(tag, frame) == -1) {\n        id3_frame_delete(frame);\n        break; /* Retain successfully parsed frames, matching 60022C0E. */\n      }', 1)
        text = replace(text, '/* ID3_TAG_OPTION_UNSYNCHRONISATION | */\n                         ID3_TAG_OPTION_COMPRESSION | ID3_TAG_OPTION_CRC;', 'ID3_TAG_OPTION_APPENDEDTAG;', 1)
        text = replace(text, 'id3_frame_render(tag->frames[i], 0, 0)', 'id3_frame_render(tag->frames[i], 0, tag->version, 0)')
        text = replace(text, 'id3_frame_render(tag->frames[i], ptr, tag->options)', 'id3_frame_render(tag->frames[i], ptr, tag->version, tag->options)')
        text = replace(text, '  if (tag->options & ID3_TAG_OPTION_ID3V1)\n    return v1_render(tag, buffer);\n\n  if (tag->options & ID3_TAG_OPTION_ID3V2_3)\n    return v2_3_render(tag, buffer);\n', '', 1)
        text = replace(text, 'id3_render_int(ptr, ID3_TAG_VERSION, 2)', 'id3_render_int(ptr, tag->version, 2)', 1)
        # Leave the unused Audacity v2_3 helper alone; patch the exported renderer.
        split = text.index('id3_length_t id3_tag_render(')
        head, body = text[:split], text[split:]
        body = replace(body, 'ehsize += id3_render_syncsafe(ptr, 0, 5);', 'ehsize += tag->version < 0x400 ? id3_render_int(ptr, 0, 4) : id3_render_syncsafe(ptr, 0, 5);', 1)
        body = replace(body, 'if (ehsize_ptr)\n      id3_render_syncsafe(&ehsize_ptr, ehsize, 4);', 'if (ehsize_ptr) {\n      if (tag->version < 0x400) id3_render_int(&ehsize_ptr, ehsize, 4);\n      else id3_render_syncsafe(&ehsize_ptr, ehsize, 4);\n    }', 1)
        body = replace(body, 'id3_render_syncsafe(&crc_ptr,\n\t\t\tid3_crc_compute(frames_ptr, *ptr - frames_ptr), 5);', 'if (tag->version < 0x400)\n      id3_render_int(&crc_ptr, id3_crc_compute(frames_ptr, *ptr - frames_ptr), 4);\n    else\n      id3_render_syncsafe(&crc_ptr, id3_crc_compute(frames_ptr, *ptr - frames_ptr), 5);', 1)
        text = head + body
    elif path.name == 'frame.h':
        text = replace(text, 'id3_byte_t **, int);', 'id3_byte_t **, unsigned int, int);', 1)
    elif path.name == 'frame.c':
        text = replace(text, 'size  = id3_parse_syncsafe(ptr, 4);', 'size  = id3_parse_uint(ptr, 4); /* Original v2.4 parser. */', 1)
        text = replace(text, 'id3_byte_t **ptr, int options)', 'id3_byte_t **ptr, unsigned int version, int options)', 1)
        text = replace(text, 'if (options & ID3_TAG_OPTION_ID3V2_3)', 'if (version < 0x400)')
        text = replace(text, 'size += id3_render_syncsafe(ptr, decoded_length, 4);', 'size += version < 0x400 ? id3_render_int(ptr, decoded_length, 4) : id3_render_syncsafe(ptr, decoded_length, 4);', 1)
        start = text.index('      if (flags & ID3_FRAME_FLAG_COMPRESSION) {', text.index('id3_length_t id3_frame_render'))
        end = text.index('\n    }\n  }\n\n  /* unsynchronisation */', start)
        text = text[:start] + '      /* Original 6002221B does not compress the rendered payload. */\n      flags &= ~ID3_FRAME_FLAG_COMPRESSION;\n' + text[end:]
    out = destination / path.name
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')
