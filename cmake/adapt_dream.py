"""Adapt the pinned Goom 1.9.3 sources in the build tree for TTPlayer's ABI.

The reference DLL uses per-instance filter/IFS storage, a smaller random table,
and MSVC's historical evaluation of repeated RAND macros. Keep those changes
explicit; never edit the downloaded source or silently accept another version.
"""
from pathlib import Path
import re
import sys

source, destination = map(Path, sys.argv[1:3])
source = next(source.rglob('goom_core.c')).parent
destination.mkdir(parents=True, exist_ok=True)

def read(name):
    return (source / name).read_text(encoding='latin1')

def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError(f'Goom source changed: {old[:100]!r}')
    return text.replace(old, new)

def clean(text):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    text = re.sub(r'^\s*#include[^\n]*', '', text, flags=re.M)
    return re.sub(r'\n\s*\n', '\n', text)

def rand_differences(text):
    # In the original compiled code both macro expansions read the same slot,
    # cancel each other, but still advance the position twice (60020131).
    text = re.sub(r'RAND\s*\(\s*\)\s*%\s*(\w+)\s*-\s*RAND\s*\(\s*\)\s*%\s*\1',
                  '(rand_pos += 2, 0)', text)
    return re.sub(r'iRAND\s*\(\s*(\d+)\s*\)\s*-\s*iRAND\s*\(\s*\1\s*\)',
                  '(rand_pos += 2, 0)', text)

for name in ['graphic.h', 'lines.h', 'filters.h', 'ifs.h', 'drawmethod_vc.h']:
    (destination / name).write_text(read(name), encoding='utf-8')
(destination / 'config.h').write_text('''#pragma once
#include <stdint.h>
typedef uint32_t guint32;
typedef int32_t gint32;
typedef int16_t gint16;
#define M_PI 3.14159265358979323846
#define ROUGE 2
#define VERT 1
#define BLEU 0
#define ALPHA 3
#define NB_FX 8
#define EFFECT_DISTORS 4
#define STOP_SPEED 128
#define TIME_BTW_CHG 300
#define __VC__ 1
#define MMX 1
''', encoding='utf-8')

lines = clean(read('lines.c'))
lines = lines.replace('draw_line (p,', 'draw_line ((int*)p,')
lines = lines.replace('lightencolor (&color,', 'lightencolor ((int*)&color,')
lines = replace(lines, 'free ((*l)->points);', 'if (!*l) return;\n free ((*l)->points2);\n free ((*l)->points);')
lines = replace(lines, 'l = NULL;', '*l = NULL;')
lines = replace(lines, 'GMLine *l = (GMLine *) malloc (sizeof (GMLine));',
    'GMLine *l = (GMLine *) calloc (1, sizeof (GMLine));\n try {')
lines = replace(lines, '\treturn l;', '\treturn l;\n } catch (...) { free(l->points); free(l->points2); free(l); throw; }')
# clean() removed in-function includes; use a portable equivalent of paddusb.
lines = re.sub(r'#if !defined\(__VC__\) \|\| !defined\(MMX\)\s*DRAWMETHOD;\s*#else\s*#endif',
               '*p = saturating_add(*p, col);', lines)
lines = re.sub(r'#if !defined\(__VC__\) \|\| !defined\(MMX\).*?#endif', '', lines, flags=re.S)
lines = re.sub(r'#if defined\(__VC__\) && defined\(MMX\).*?#endif', '', lines, flags=re.S)
# Fixed-point line clipping in upstream can reach the next row's first pixel.
# Bound each store to the image while preserving valid original coordinates.
lines = lines.replace('*p = saturating_add(*p, col);',
    'if (p >= data && p < data + screenx * screeny) *p = saturating_add(*p, col);')

# Original /fp:fast precomputed reciprocals are single-precision constants.
lines = lines.replace('2.0f * M_PI * (float) i / 512.0f', 'double(i) * 0.012271846644580364f')
lines = lines.replace('cosa = param * cos (l[i].angle);', 'cosa = float(double(param) * cos(double(i) * 0.012271846644580364f));')
lines = lines.replace('sina = param * sin (l[i].angle);', 'double sin_value = double(param) * sin(l[i].angle);')
lines = lines.replace('(float) ry / 2.0f + sina;', 'float(double(ry) / 2.0 + sin_value);')
for field in ['x','y','angle']:
    lines = re.sub(r'\(l->points2\[i\]\.'+field+r' \+ 39.0f \* l->points\[i\]\.'+field+r'\) / 40.0f',
        '(double(l->points2[i].'+field+') + 39.0 * l->points[i].'+field+') * double(0.025f)', lines)
lines = lines.replace('/ 600.0f', '* (1.0f / 600.0f)')
lines = lines.replace('float   t = exp', 'double t = exp')
lines = lines.replace('cos (pt->angle) / 1000.0f', 'cos (pt->angle) * double(0.001f)')
lines = lines.replace('sin (pt->angle) / 1000.0f', 'sin (pt->angle) * double(0.001f)')

start_draw = lines.index('void\ngoom_lines_draw')
line_draw = lines[start_draw:]
line_draw = line_draw.replace('(pt->x + cosa * data[0])', '(double(pt->x) + double(cosa) * data[0])').replace('(pt->y + sina * data[0])', '(double(pt->y) + double(sina) * data[0])')
line_draw = line_draw.replace('(pt->x + cosa * data[i])', '(double(pt->x) + double(cosa) * data[i])').replace('(pt->y + sina * data[i])', '(double(pt->y) + sina * data[i])')
line_draw = line_draw.replace('\t\t\tfloat   sina =', '\t\t\tdouble sina =')
lines = lines[:start_draw] + line_draw

ifs = clean(read('ifs.c'))
ifs = re.sub(r'static\s+DBL\s*\n(Gauss_Rand|Half_Gauss_Rand)', r'static double\n\1', ifs)
ifs = ifs.replace('DBL     y;', 'double y;')
ifs = ifs.replace('y = (DBL) LRAND () / MAXRAND;', 'y = double(LRAND()) * double(0.000030518509447574615f);')
ifs = ifs.replace('y = A * (1.0 - exp (-y * y * S)) / (1.0 - exp (-S));',
    'float numerator = float(A * (1.0 - exp(-y*y*S))); y = double(numerator) / (1.0 - exp(-double(S)));')
ifs = ifs.replace('if (NRAND (2))', 'y = float(y);\n if (NRAND (2))')
ifs = ifs.replace('(M_PI / 180.0)', 'double(0.01745329238474369f)')

ifs = re.sub(r'/\s*UNIT', '>> 12', ifs)
ifs = ifs.replace('x * F->Lx / (UNIT * 2)', '(x * F->Lx) >> 13').replace('y * F->Ly / (UNIT * 2)', '(y * F->Ly) >> 13')
ifs = ifs.replace('DBL     u, uu, v, vv, u0, u1, u2, u3;', 'double u, uu, v, vv, u3; float u0, u1, u2;')
ifs = ifs.replace('(DBL) (F->Count) * (DBL) (F->Speed) / 1000.0', 'double(F->Count * F->Speed) * double(0.001f)')

prefix, body = ifs.split('static FRACTAL *Root', 1)
body = 'FRACTAL *Root' + body
body = body.replace('*Cur_F;', '*Cur_F = NULL;').replace('IFSPoint *Buf;', 'IFSPoint *Buf = NULL;')
body = body.replace('static int Cur_Pt;', 'int Cur_Pt = 0;')
body = re.sub(r'\bstatic\s+', '', body)
body = body.replace('(FRACTAL *) malloc (sizeof (FRACTAL))', '(FRACTAL *) calloc (1, sizeof (FRACTAL))')
start = body.index('IFSPoint *\ndraw_ifs')
end = body.index('void\nrelease_ifs', start)
body = body[:start] + body[start:end].replace('return;', 'return NULL;') + body[end:]
body = replace(body, '(void) free ((void *) Root);', 'free_ifs_buffers(Root);\n (void) free ((void *) Root);')
display = clean(read('ifs_display.c'))
display = display.replace('couleur = (col[ALPHA]', 'dream_trace[4]=couleur; dream_trace[5]=increment; dream_trace[6]=nbpt;\n couleur = (col[ALPHA]')
# Use the exact four-byte saturated addition without entering MMX state.
a = display.index('#ifdef MMX\n')
b = display.index('\n\t\tjustChanged--', a)
display = display[:a] + '''
 for (i = 0; i < nbpt; i += increment) {
   int x = points[i].x, y = points[i].y;
   if (x > 0 && y > 0 && x < width && y < height) {
     int pos = x + y * width;
     data[pos] = saturating_add(back[pos], couleursl);
   }
 }
''' + display[b:]
ifs = prefix + '\nstruct DreamIfs {\n' + body + display + '\n~DreamIfs() { release_ifs(); }\n};\n'

filters = clean(read('filters.c'))
start = filters.index('signed int *brutS = 0')
filters = filters[start:]
filters = replace(filters, 'static int sintable[0xffff];', 'int sintable[0x10000] = {};')
filters = replace(filters, 'int     prevX = 0, prevY = 0;', 'int prevX = 0, prevY = 0;')
filters = replace(filters, 'static int middleX, middleY;', 'int middleX = 0, middleY = 0;')
# Object state; generatePrecalCoef and the fire-motion counters stay shared.
for name in ['vitesse', 'theMode', 'waveEffect', 'hypercosEffect', 'vPlaneEffect', 'hPlaneEffect', 'noisify', 'firedec']:
    filters = re.sub(r'\bstatic\s+(int|char)\s+(\*?'+name+r')\b', r'\1 \2', filters)
filters = replace(filters, 'static int wave = 0;', '')
filters = replace(filters, 'static int wavesp = 0;', '')
filters = replace(filters, 'static char reverse = 0;', '')
filters = replace(filters, 'static unsigned char pertedec = 8;', '')
filters = replace(filters, 'static char firstTime = 1;', '')
filters = 'int wave=0, wavesp=0; char reverse=0, firstTime=1; unsigned char pertedec=8;\n' + filters
filters = filters.replace('(unsigned int *) malloc', '(int *) malloc').replace('= (guint32 *)', '= (int *)')
# The original sine table fills 65535 entries and uses denominator 65534.
filters = filters.replace('sizeof (sintable) / sizeof (sintable[0]) -\n\t\t\t\t\t\t\t\t\t\t\t 1', '65534')
filters = re.sub(r'sizeof\s*\(sintable\)\s*/\s*sizeof\s*\(sintable\[0\]\)\s*-\s*1', '65534', filters)
# scalar path matching 6002110B/60026000, including alpha and edge sampling.
start = filters.index('void\nc_zoom')
end = filters.index('#ifdef USE_ASM', start)
filters = filters[:start] + '''void c_zoom() {
 for (int i=0; i<prevX*prevY; ++i) {
   int px=brutS[2*i]+((brutD[2*i]-brutS[2*i])*buffratio>>16);
   int py=brutS[2*i+1]+((brutD[2*i+1]-brutS[2*i+1])*buffratio>>16);
   if ((unsigned)px >= (unsigned)((prevX-1)*16) || (unsigned)py >= (unsigned)((prevY-1)*16)) px=py=0;
   int pos=(px>>4)+prevX*(py>>4);
   unsigned co=precalCoef[px&15][py&15], out=0;
   for (int c=0;c<4;++c) {
     unsigned sum=((expix1[pos]>>(8*c))&255)*(co&255)
       +((expix1[pos+1]>>(8*c))&255)*((co>>8)&255)
       +((expix1[pos+prevX]>>(8*c))&255)*((co>>16)&255)
       +((expix1[pos+prevX+1]>>(8*c))&255)*(co>>24);
     out |= (sum>>8)<<(8*c);
   }
   expix2[i]=out;
 }
}
''' + filters[end:]
# Discard ASM selection: the portable kernel above implements the original MMX arithmetic.
start = filters.index('#ifdef USE_ASM')
end = filters.index('void\nzoomFilterFastRGB', start)
filters = filters[:start] + filters[end:]
start = filters.index('#ifdef USE_ASM')
end = filters.index('void\npointFilter', start)
filters = filters[:start] + '\n c_zoom();\n}\n\n' + filters[end:]
a = filters.index('inline void\ncalculatePXandPY')
b = filters.index('inline void\nsetPixelRGB', a)
filters = filters[:a] + re.sub(r'sintable\[([^\]]+)\]', r'sine_at(\1)', filters[a:b]) + filters[b:]
filters += '\n int sine_at(unsigned i) const { return i == 65535 ? vitesse : sintable[i]; }\n'

filters = filters.replace('(float) cycle / t3', 'double(cycle) / t3').replace('(float) cycle / t4', 'double(float(cycle)) / t4')
filters = filters.replace('(float) BUFFPOINTMASK * (1.0f - switchMult)', 'double(BUFFPOINTMASK) * (1.0 - double(switchMult))').replace('(float) buffratio * switchMult', 'double(buffratio) * switchMult')
filters = rand_differences(filters).replace('unsigned int *temp = brutD;', 'int *temp = brutD;').replace('mmx_zoom_size = prevX * prevY;', '')
filters = filters.replace('int     precalCoef[16][16];', 'inline static int precalCoef[16][16] = {};')
filters = filters.replace('static int firstime', 'static int firstime')
filters = '\nstruct DreamFilter {\n' + filters + '''
 void resize(int width, int height) {
   if (prevX == width && prevY == height) return;
   free(firedec); firedec=nullptr;
   free(freebrutS); freebrutS=nullptr; brutS=nullptr;
   free(freebrutD); freebrutD=nullptr; brutD=nullptr;
   free(freebrutT); freebrutT=nullptr; brutT=nullptr;
   prevX=width; prevY=height; middleX=width/2; middleY=height-1; firstTime=1;
 }
 ~DreamFilter() { free(firedec); free(freebrutS); free(freebrutD); free(freebrutT); }
};
'''

core = clean(read('goom_core.c'))
start = core.index('guint32 *\ngoom_update')
end = core.index('void\ngoom_close', start)
update = core[start:end]
start = update.index('\n\t{\n\t\tstatic char title')
end = update.index('\n\tif (lineMode', start)
update = update[:start] + update[end:]
update = replace(update, 'gint16 data[2][512], int forceMode, float fps, char *songTitle', 'const gint16* left, const gint16* right, int forceMode')
update = update.replace('data[0]', 'left').replace('data[1]', 'right')
update = update.replace('goom_lines_draw (', 'goom_lines_draw (').replace(', left,', ', const_cast<gint16*>(left),').replace(', right,', ', const_cast<gint16*>(right),')
update = update.replace('ifs_update (', 'ifs.ifs_update (').replace('pointFilter (', 'filter.pointFilter (').replace('zoomFilterFastRGB (', 'filter.zoomFilterFastRGB (')
update = update.replace(' + c_offset', '')
choose = core[core.index('void\nchoose_a_goom_line', end):core.index('void\ngoom_draw_text', end)]
choose = choose.replace('c_resoly / 7;', '(float)c_resoly * (1.0f/7.0f);')
update = update.replace('return return_val;', 'dream_trace[0]=ifs_incr; dream_trace[1]=decay_ifs; dream_trace[2]=recay_ifs; dream_trace[3]=loopvar; return return_val;')
update = rand_differences(update)
update = update.replace('STOP_SPEED - iRAND (2) + iRAND (2)', '(rand_pos += 2, STOP_SPEED)')
update = update.replace('((float) speedvar / 40.0f + (float) incvar / 50000.0f) / 1.5f', '(double(speedvar)*0.025f + double(incvar)*0.0002f) * double(1.0f/1.5f)')
out = '''// Generated from pinned LGPL Goom sources; see licenses/GOOM-NOTICES.txt.
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>
namespace {
#include "config.h"
#include "graphic.h"
#include "lines.h"
#include "filters.h"
#include "ifs.h"
static unsigned resolx, c_resoly;
#ifdef TTP_DREAM_DIAGNOSTICS
static int dream_trace[7];
#define DREAM_TRACE(i,v) (dream_trace[i]=(v))
#else
#define DREAM_TRACE(i,v) ((void)0)
#endif
static short rand_tab[0x4000] = {};
static unsigned short rand_pos;
#define RAND() (rand_tab[(rand_pos++) & 0x3fff])
#define iRAND(i) (RAND() % (i))
static unsigned saturating_add(unsigned a, unsigned b) {
 unsigned r=0; for(int i=0;i<4;++i) { unsigned c=((a>>(i*8))&255)+((b>>(i*8))&255); r|=(c>255?255:c)<<(i*8); } return r;
}
'''
out += clean(read('graphic.c')).split('unsigned int HEIGHT')[0]
out += lines + ifs + filters + '\n#undef sqrtperte\n'
out += '\nstruct Dream {\n unsigned *pixel=nullptr,*back=nullptr,*p1=nullptr,*p2=nullptr,*tmp=nullptr;\n unsigned cycle=0,buffsize=0,width=0,height=0; bool failed=false;\n GMLine *gmline1=nullptr,*gmline2=nullptr; DreamFilter filter; DreamIfs ifs;\n'
out += update + choose + '\n};\n}\n'
out = re.sub(r'dream_trace\[(\d+)\]=([^;]+);', r'DREAM_TRACE(\1,\2);', out)
out = re.sub(r'\b(cos|sin|exp|log)\s*\(', r'dream_\1(', out)
out = re.sub(r'\b(malloc|calloc)\s*\(', r'dream_\1(', out)
out = out.replace('namespace {', '''namespace {
+static double dream_cos(double x) { return cos(x); }
+static double dream_sin(double x) { return sin(x); }
+static double dream_exp(double x) { return exp(x); }
+static double dream_log(double x) { return log(x); }
+static void* dream_malloc(size_t n) { void* p=calloc(1,n); if(!p) throw std::bad_alloc(); return p; }
+static void* dream_calloc(size_t n, size_t size) { void* p=calloc(n,size); if(!p) throw std::bad_alloc(); return p; }
+'''.replace('\n+', '\n'), 1)
(destination/'dream_engine.inc').write_text(out, encoding='utf-8')
