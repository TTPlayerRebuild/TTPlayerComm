"""Adapt the pinned SSRC source in the build tree to ttpcomm's double ABI."""
from pathlib import Path
import sys
source,destination=map(Path,sys.argv[1:]);source/= 'plugins/Shibatch'
destination.mkdir(parents=True,exist_ok=True)
def replace(text,old,new,count=1):
    if text.count(old)!=count:raise RuntimeError(f'SSRC pinned source changed: {old!r}')
    return text.replace(old,new)
for name in ['ssrc.cpp','ssrc.h','fft.h','dbesi0.c']:
    text=(source/name).read_text(encoding='utf-8-sig')
    if name=='ssrc.h':
        text=replace(text,'#include <avisynth.h>\n#include <avs/config.h>','#include "ssrc_support.h"')
        text=replace(text,'#include "mem_block.h"','')
        text=replace(text,'void Read(int size);','void Read(int size);\n    void Clear() {buf_data=0;}')
        text=replace(text,'unsigned int GetLatency();','void ClearOutput(){out.Clear();}\n\tunsigned int GetLatency();')
        text=replace(text,'double GetPeak()', 'void AppendOutput(const REAL_inout* p,int n){out.Write(p,n);}\n\tdouble GetPeak()')
    if name=='ssrc.cpp':
        text=replace(text,'#include <avs/alignment.h>\n#include <avs/config.h>','')
        text=replace(text,'#define M_PI 3.1415926535897932384626433832795028842','#define M_PI 3.1415927410125732421875')
        text=replace(text,'new Upsampler<float>(c)','new Upsampler<double>(c)')
        text=replace(text,'new Downsampler<float>(c)','new Downsampler<double>(c)')
        text=replace(text,'while(done<s)','while(true)')
        text=replace(text,'unsigned int sumread,sumwrite;','unsigned long long sumread,sumwrite;',2)
        # TTPlayer trims delay in frames, limits EOF to floor(total*ratio)+2,
        # and always advances the convolution history, including at EOF.
        for unused in range(2):
            start=text.index('  make_outbuf(nsmplwrt2, outbuf, delay2);')
            end=text.index('\n  {\n    int ds =',start)
            text=text[:start]+'''  int skip = nsmplwrt2 < delay ? nsmplwrt2 : delay;
  delay -= skip;
  int emitted = nsmplwrt2 - skip;
  if (ending) {
    long long limit = static_cast<long long>(sumread*dfrq/sfrq) - sumwrite + 2;
    if (limit < 0) limit=0;
    if (emitted > limit) emitted=static_cast<int>(limit);
  }
  AppendOutput(outbuf+skip*nch,emitted*nch);
  sumwrite += emitted;
''' +text[end:]
    out=destination/name
    if not out.exists() or out.read_text(encoding='utf-8')!=text:out.write_text(text,encoding='utf-8')
