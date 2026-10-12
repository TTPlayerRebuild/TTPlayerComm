"""Bound the SDK decoder and make long password derivation cancellable."""
from pathlib import Path
import shutil, sys, stat
src, dst = map(Path, sys.argv[1:])
if dst.exists():
    for p in dst.rglob('*'):
        if p.is_file(): p.chmod(stat.S_IWRITE | stat.S_IREAD)
shutil.copytree(src, dst, dirs_exist_ok=True)
for p in dst.rglob('*'):
    if p.is_file(): p.chmod(stat.S_IWRITE | stat.S_IREAD)
def patch(name, old, new, count=1):
    p=dst/name; s=p.read_text(encoding='utf-8-sig')
    if s.count(old)!=count: raise RuntimeError(f'{name}: expected {count} copies of {old[:60]}')
    p.write_text(s.replace(old,new),encoding='utf-8')
hooks='\n#include "archive_seven_hooks.h"\n'
for name in ['CPP/7zip/Archive/7z/7zIn.cpp','CPP/7zip/Archive/7z/7zDecode.cpp','CPP/7zip/Crypto/7zAes.cpp']:
    patch(name,'#include "StdAfx.h"','#include "StdAfx.h"'+hooks)
patch('CPP/7zip/Archive/7z/7zIn.cpp','const CNum numFiles = ReadNum();',
      'const CNum numFiles = ReadNum();\n  if (numFiles > ttpcomm_seven_entry_limit()) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);\n  RINOK(ttpcomm_seven_header_limit((UInt64)numFiles * 256))')
# Count fields may request huge vectors before any entry callback runs.
patch('CPP/7zip/Archive/7z/7zIn.cpp','const CNum numFolders = ReadNum();',
      'const CNum numFolders = ReadNum();\n  if (numFolders > ttpcomm_seven_entry_limit() || FAILED(ttpcomm_seven_header_limit((UInt64)numFolders * 256))) ThrowUnsupported();')
patch('CPP/7zip/Archive/7z/7zIn.cpp','const CNum numPackStreams = ReadNum();',
      'const CNum numPackStreams = ReadNum();\n  if ((UInt64)numPackStreams > (UInt64)ttpcomm_seven_entry_limit() * 4 || FAILED(ttpcomm_seven_header_limit((UInt64)numPackStreams * 64))) ThrowUnsupported();')
patch('CPP/7zip/Archive/7z/7zIn.cpp','data.Alloc(unpackSize);',
      'RINOK(ttpcomm_seven_header_limit(unpackSize))\n    data.Alloc(unpackSize);')
patch('CPP/7zip/Archive/7z/7zIn.cpp','HeadersSize += nextHeaderSize;',
      'RINOK(ttpcomm_seven_header_limit(nextHeaderSize))\n  HeadersSize += nextHeaderSize;')
patch('CPP/7zip/Archive/7z/7zDecode.cpp',
      'HRESULT res = setDecoderProperties->SetDecoderProperties2((const Byte *)props, size32);',
      'RINOK(ttpcomm_seven_dictionary(coderInfo.MethodID, (const Byte *)props, size32))\n        HRESULT res = setDecoderProperties->SetDecoderProperties2((const Byte *)props, size32);')
patch('CPP/7zip/Crypto/7zAes.cpp','Sha256_Update((CSha256 *)(void *)(Byte *)sha, buf, unrollSize);',
      'if (ttpcomm_seven_canceled()) throw HRESULT_FROM_WIN32(ERROR_CANCELLED);\n        Sha256_Update((CSha256 *)(void *)(Byte *)sha, buf, unrollSize);')
# No global password/key retention or cross-thread cache races in Z7_ST.
start='  bool finded = false;\n  if (!_cachedKeys.GetKey(_key))'
p=dst/'CPP/7zip/Crypto/7zAes.cpp';s=p.read_text();a=s.index(start);b=s.index('\n}',a)
s=s[:a]+'  _key.CalcKey();'+s[b:];p.write_text(s,encoding='utf-8')
