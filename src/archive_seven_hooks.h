#pragma once
#include <windows.h>
#include <stdint.h>
uint32_t ttpcomm_seven_entry_limit();
HRESULT ttpcomm_seven_header_limit(uint64_t);
HRESULT ttpcomm_seven_dictionary(uint64_t,const unsigned char*,unsigned);
bool ttpcomm_seven_canceled();
uint64_t ttpcomm_seven_codec_memory_limit();
HRESULT ttpcomm_seven_codec_memory_error();
