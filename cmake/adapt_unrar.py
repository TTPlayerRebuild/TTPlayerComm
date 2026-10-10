"""Keep cancellation responsive while rebuilding a solid RAR dictionary."""
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)
text = (source / 'rdwrfn.cpp').read_text(encoding='utf-8-sig')
needle = '  if (Cmd->DllOpMode!=RAR_SKIP)'
assert text.count(needle) == 1, 'UnRAR callback boundary changed'
replacement = '''  // TTPlayer: skipped solid predecessors still need cancellation callbacks.
  // The host discards these bytes; no destination file is opened.
  if (Cmd->DllOpMode==RAR_SKIP && Cmd->Callback!=NULL &&
      Cmd->Callback(UCM_PROCESSDATA,Cmd->UserData,(LPARAM)Addr,Count)==-1)
    ErrHandler.Exit(RARX_USERBREAK);
  if (Cmd->DllOpMode!=RAR_SKIP)'''
license_text = (source / 'license.txt').read_text(encoding='utf-8-sig')
header = '/* Upstream UnRAR; local callback adaptation.\n' + license_text + '\n*/\n'
(destination / 'rdwrfn.cpp').write_text(header + text.replace(needle, replacement), encoding='utf-8')
