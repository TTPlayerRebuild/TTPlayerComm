"""Keep transaction bookkeeping independent of suspended threads' CRT heap."""
from pathlib import Path
import re
import sys

source, dest = map(Path, sys.argv[1:])
dest.mkdir(parents=True, exist_ok=True)
text = (source / 'src/detours.cpp').read_text(encoding='utf-8')
# These two upstream structures are PODs, without constructors/destructors.
# DetourUpdateThread suspends other threads. Such a thread could own the CRT
# heap lock, so allocate/free this small, transient bookkeeping via VirtualAlloc.
for name, count in [('DetourThread', 1), ('DetourOperation', 2)]:
    old = 'new NOTHROW ' + name
    assert text.count(old) == count
    text = text.replace(old, f'static_cast<{name}*>(VirtualAlloc(NULL, sizeof({name}), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))')
text, count = re.subn(r'delete ([ot]);', r'VirtualFree(\1, 0, MEM_RELEASE);', text)
assert count == 7
(dest / 'detours.cpp').write_text(text, encoding='utf-8')
