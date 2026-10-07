"""Check actual PE exports against the reconstruction manifest, without loading it."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

def inspect_exports(path):
    data=path.read_bytes()
    def unpack(fmt,offset):return struct.unpack_from('<'+fmt,data,offset)
    if data[:2]!=b'MZ':raise ValueError('Missing MZ header')
    pe=unpack('I',0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0':raise ValueError('Missing PE signature')
    if unpack('H',pe+4)[0]!=0x14c:raise ValueError('Expected x86 machine')
    opt=pe+24
    if unpack('H',opt)[0]!=0x10b:raise ValueError('Expected PE32 optional header')
    sections=[unpack('IIII',opt+unpack('H',pe+20)[0]+40*i+8) for i in range(unpack('H',pe+6)[0])]
    def offset(rva):
        for virtual_size,virtual,raw_size,raw in sections:
            if virtual<=rva<virtual+raw_size:return raw+rva-virtual
        raise ValueError(f'Unmapped export RVA {rva:x}')
    def string(rva):
        p=offset(rva);return data[p:data.index(0,p)].decode('ascii')
    export,size=unpack('II',opt+96)
    base,count,names,addresses,nt,ot=unpack('IIIIII',offset(export)+16)
    named={unpack('H',offset(ot)+2*i)[0]:string(unpack('I',offset(nt)+4*i)[0]) for i in range(names)}
    out={}
    for i in range(count):
        address=unpack('I',offset(addresses)+i*4)[0]
        if not address:continue
        if export<=address<export+size:raise ValueError('Forwarder is not an independent implementation')
        out[base+i]={'name':named.get(i,''),'rva':address}
    return out

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dll',type=Path);ap.add_argument('manifest',type=Path)
    ap.add_argument('--report',type=Path);ap.add_argument('--require-complete',action='store_true')
    args=ap.parse_args();manifest=json.loads(args.manifest.read_text(encoding='utf-8'))
    expected={item['ordinal']:item for item in manifest['exports'] if item['status']!='pending'}
    actual=inspect_exports(args.dll)
    if actual.keys()!=expected.keys():raise ValueError(f'Export mismatch: missing {expected.keys()-actual.keys()}, unexpected {actual.keys()-expected.keys()}')
    for ordinal,item in expected.items():
        if actual[ordinal]['name']!=item['name']:raise ValueError(f'Wrong named/NONAME export at {ordinal}')
    if actual[12]['rva']!=actual[78]['rva']:raise ValueError('12/78 must alias the same free implementation')
    if args.require_complete and (not manifest['complete'] or len(actual)!=67):raise ValueError('This library is not a complete replacement')
    report={'dll':args.dll.name,'sha256':hashlib.sha256(args.dll.read_bytes()).hexdigest(),
        'complete':manifest['complete'],'exports':actual,'pending':[i['ordinal'] for i in manifest['exports'] if i['status']=='pending']}
    if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(f'ABI check passed: {len(actual)}/67 exports; complete={manifest["complete"]}')

if __name__=='__main__':main()
