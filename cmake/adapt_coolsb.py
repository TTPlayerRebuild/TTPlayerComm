"""Apply the recovered TTPlayer CoolSB 1.2 extension in the build tree only."""
from pathlib import Path
import shutil
import sys

source, dest = map(Path, sys.argv[1:])
source /= 'coolsb'
dest.mkdir(parents=True, exist_ok=True)
for name in ('coolscroll.c', 'coolsblib.c', 'coolscroll.h', 'coolsb_internal.h', 'userdefs.h'):
    shutil.copyfile(source / name, dest / name)


def edit(name, old, new, count=1):
    path = dest / name
    text = path.read_text(encoding='latin1')
    if text.count(old) != count:
        raise RuntimeError(f'{name}: expected {count} occurrences of {old!r}')
    path.write_text(text.replace(old, new), encoding='latin1')


for flag in ('INCLUDE_BUTTONS', 'RESIZABLE_BUTTONS', 'COOLSB_TOOLTIPS'):
    edit('userdefs.h', '#define ' + flag, '#undef ' + flag)
edit('coolsb_internal.h', 'int\t\t\tnMinThumbSize;',
     'int\t\t\tnMinThumbSize;\n\tBOOL fProportionalThumb;')
edit('coolscroll.c', 'thumbsize = MulDiv(si->nPage, workingsize, siMaxMin);',
     'thumbsize = sbar->fProportionalThumb ? MulDiv(si->nPage, workingsize, siMaxMin) : 0;')
edit('coolsblib.c', 'CoolSB_SetMinThumbSize(hwnd, SB_BOTH, CoolSB_GetDefaultMinThumbSize());',
     'CoolSB_SetMinThumbSize(hwnd, SB_BOTH, CoolSB_GetDefaultMinThumbSize(), TRUE);')
for name in ('coolsblib.c', 'coolscroll.h'):
    path = dest / name
    import re
    text = path.read_text(encoding='latin1')
    text, n = re.subn(r'(CoolSB_SetMinThumbSize\s*\(HWND hwnd, UINT wBar, UINT size)\)',
                      r'\1, BOOL proportional)', text)
    if n != 1:
        raise RuntimeError(f'{name}: minimum thumb declaration missing')
    path.write_text(text, encoding='latin1')
# @204 sets both minima for SB_BOTH, but its last assignment affects only the
# last (vertical) bar. Preserve that observable historical behavior.
edit('coolsblib.c', 'BOOL WINAPI CoolSB_SetMinThumbSize(HWND hwnd, UINT wBar, UINT size, BOOL proportional)\n{\n\tSCROLLBAR *sbar;',
     'BOOL WINAPI CoolSB_SetMinThumbSize(HWND hwnd, UINT wBar, UINT size, BOOL proportional)\n{\n\tSCROLLBAR *sbar = NULL;')
path = dest / 'coolsblib.c'
text = path.read_text(encoding='latin1')
at = text.index('BOOL WINAPI CoolSB_SetMinThumbSize(')
head, tail = text[:at], text[at:]
tail = tail.replace('\treturn TRUE;', '\tif (!sbar) return FALSE;\n\tsbar->fProportionalThumb = proportional;\n\treturn TRUE;', 1)
path.write_text(head + tail, encoding='latin1')
# TTPlayer routes SetScrollRange through SetScrollInfo(SIF_RANGE), including its
# return value (the position) and visibility handling; upstream returned TRUE.
path = dest / 'coolsblib.c'
text = path.read_text(encoding='latin1')
start = text.index('int WINAPI CoolSB_SetScrollRange ')
end = text.index('//\n//\tShow or hide', start)
text = text[:start] + '''int WINAPI CoolSB_SetScrollRange(HWND hwnd, int bar, int lo, int hi, BOOL redraw)
{
    SCROLLINFO si = {sizeof(si), SIF_RANGE, lo, hi, 0, 0, 0};
    return SetScrollInfo(hwnd, bar, &si, redraw);
}

''' + text[end:]
path.write_text(text, encoding='latin1')
# Validate allocation / HWND / subclass installation instead of inheriting
# the reference's null dereference and invalid HWND memory write paths.
edit('coolsblib.c', '\tGetClientRect(hwnd, &rect);',
     '\tif (!IsWindow(hwnd)) return FALSE;\n\tGetClientRect(hwnd, &rect);')
edit('coolsblib.c', 'sw = (SCROLLWND *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SCROLLWND));',
     'sw = (SCROLLWND *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SCROLLWND));\n\tif (!sw) return FALSE;')
edit('coolsblib.c', 'SetProp(hwnd, szPropStr, (HANDLE)sw);',
     'if (!SetProp(hwnd, szPropStr, (HANDLE)sw)) { HeapFree(GetProcessHeap(), 0, sw); return FALSE; }')
edit('coolsblib.c', 'sw->oldproc = (WNDPROC)SetWindowLong(hwnd, GWL_WNDPROC, (LONG)CoolSBWndProc);',
     'SetLastError(0);\n\tsw->oldproc = (WNDPROC)SetWindowLong(hwnd, GWL_WNDPROC, (LONG)CoolSBWndProc);\n'
     '\tif (!sw->oldproc) { RemoveProp(hwnd, szPropStr); HeapFree(GetProcessHeap(), 0, sw); return FALSE; }')
