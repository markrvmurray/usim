//
//	picofram.cpp
//	Pico-Thing Persistent RAM (FRAM)
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <cstring>
#include "picofram.h"

PicoFRAM::PicoFRAM(const char* filepath)
	: filepath(filepath)
{
	memset(data, 0xFF, FRAM_SIZE);
	load();
}

void PicoFRAM::load()
{
	if (!filepath) return;
	FILE* fp = fopen(filepath, "rb");
	if (fp) {
		(void)fread(data, 1, FRAM_SIZE, fp);
		fclose(fp);
	}
}

void PicoFRAM::save()
{
	if (!filepath) return;
	FILE* fp = fopen(filepath, "wb");
	if (fp) {
		(void)fwrite(data, 1, FRAM_SIZE, fp);
		fclose(fp);
	}
}

Byte PicoFRAM::read(Word offset)
{
	if (offset < FRAM_SIZE)
		return data[offset];
	return 0xFF;
}

void PicoFRAM::write(Word offset, Byte val)
{
	if (offset < FRAM_SIZE) {
		data[offset] = val;
		save();
	}
}
