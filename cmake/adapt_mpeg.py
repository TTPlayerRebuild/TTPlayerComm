"""Prepare fixed mpglib sources; the public buffering/packet ABI is our own."""
from pathlib import Path
import sys
source,dest,project=map(Path,sys.argv[1:])
dest.mkdir(parents=True,exist_ok=True)
def replace(text,old,new,count=1):
    if text.count(old)!=count:raise RuntimeError(f'mpglib source changed: {old!r}')
    return text.replace(old,new)
for file in (source/'mpglib').iterdir():
    if file.suffix not in ('.c','.h'):continue
    text=file.read_text()
    if file.name=='mpg123.h':
        text=replace(text,'#define REAL_IS_FLOAT','// ttpcomm uses double throughout.')
    if file.name in ('layer1.c','layer2.c'):
        text=text.replace('synth_1to1_mono(', 'synth_1to1_mono_unclipped(')
        text=text.replace('synth_1to1(', 'synth_1to1_unclipped(')
    if file.name=='layer3.c':
        text=replace(text,'if (gr_infos->block_type == 0) {',
            'if (gr_infos->block_type == 0) {\n                gr_infos->block_type=4; /* adapter rejects invalid switching */',2)
    if file.name=='common.c':
        start=text.index('int\nset_pointer(')
        text=text[:start]+'''int set_pointer(PMPSTR mp, long backstep) {
    /* The adapter has already installed the validated reservoir prefix. */
    mp->wordpointer -= backstep;
    mp->bitindex = 0;
    return MP3_OK;
}
'''
    if file.name=='tabinit.c':
        start=text.index('static const double dewin[') if 'static const double dewin[' in text else text.index('static const real dewin[')
        end=text.index('};',start)+2
        text=text[:start]+'''static const long original_window[] = {
#include "mpeg_window.inc"
};
'''+text[end:]
        text=text.replace('dewin[j] * scaleval','original_window[j] * (1.0/65536.0) * scaleval')
    out=dest/file.name
    if not out.exists() or out.read_text()!=text:out.write_text(text)
