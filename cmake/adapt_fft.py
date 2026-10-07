"""Preserve TTPlayer's x87 spill points in the 512-sample spectrum path.

6000F790 (radix four) and 600104B0 (real unpack) keep selected temporaries
in x87 registers and spill others to float. Merely selecting /fp or changing
the final magnitude rounding does not reproduce these arithmetic boundaries.
"""
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)
for name in ('kiss_fft.c', 'kiss_fftr.c'):
    text = (source / name).read_text(encoding='utf-8')
    if name == 'kiss_fft.c':
        marker = 'static void kf_bfly4('
        assert text.count(marker) == 1
        text = text.replace(marker, '#include "fft_rounding.h"\n\n' + marker)
        start = text.index('{', text.index(marker)) + 1
        text = text[:start] + '''
    if (st->nfft == 256 && !st->inverse) {
        ttp_fft_bfly4(Fout, fstride, st->twiddles, m);
        return;
    }
''' + text[start:]
    else:
        text = text.replace('#include "_kiss_fft_guts.h"', '#include "_kiss_fft_guts.h"\n#include "fft_rounding.h"')
        start = text.index('        C_ADD( f1k, fpk , fpnk );')
        end = text.index('\n    }', start)
        text = text[:start] + '''        if (ncfft == 256) {
            ttp_fft_unpack(fpk, fpnk, st->super_twiddles[k-1],
                           &freqdata[k], &freqdata[ncfft-k]);
        } else {
''' + text[start:end] + '\n        }' + text[end:]
    out = destination / name
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        out.write_text(text, encoding='utf-8')
