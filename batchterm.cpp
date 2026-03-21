//
//	batchterm.cpp
//	Batch-mode mc6850_impl for non-interactive use
//
//	vim: ts=8 sw=8 noet:
//

#include <cstdio>
#include <unistd.h>
#include <sys/select.h>
#include "batchterm.h"

bool BatchTerminal::poll_read()
{
	fd_set fds;
	struct timeval tv = { 0L, 0L };

	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	(void)select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);

	return FD_ISSET(STDIN_FILENO, &fds);
}

Byte BatchTerminal::read()
{
	int ch = fgetc(stdin);
	return (ch == EOF) ? 0 : (Byte)ch;
}

void BatchTerminal::write(Byte ch)
{
	fputc(ch, stdout);
	fflush(stdout);
}
