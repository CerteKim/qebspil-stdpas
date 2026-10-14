// SPDX-License-Identifier: GPL-2.0-only
/*
 * Best-effort log file on the ESP.
 *
 * The EFI console output is gone as soon as the kernel starts, so every
 * qebspil message is additionally appended to \qebspil.log next to the binary.
 * That survives the boot and can be read from Linux (/boot/qebspil.log), which
 * makes it possible to see what qebspil did without watching the screen.
 */
#include <stdarg.h>

#include <efi.h>
#include <efilib.h>

#include "qebspil.h"

#define QLOG_LINE_MAX	400

static EFI_FILE_HANDLE qlog_root;
static EFI_FILE_HANDLE qlog_file;
static BOOLEAN qlog_tried;

static void qlog_open(void)
{
	EFI_STATUS status;

	if (qlog_file || qlog_tried)
		return;

	/* Can only be resolved after BS->HandleProtocol(LoadedImageProtocol) */
	if (!LoadedImage)
		return;

	qlog_tried = TRUE;

	qlog_root = LibOpenRoot(LoadedImage->DeviceHandle);
	if (!qlog_root) {
		AsciiPrint((CHAR8 *)"qebspil: cannot open root volume for the log\n");
		return;
	}

	/* \qebspil.log next to the binary; truncates a log from a previous boot */
	status = qlog_root->Open(qlog_root, &qlog_file, u"qebspil.log",
				 EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ |
				 EFI_FILE_MODE_WRITE, 0);
	if (EFI_ERROR(status)) {
		AsciiPrint((CHAR8 *)"qebspil: cannot create \\qebspil.log: %r\n", status);
		qlog_file = NULL;
	}
}

void qlog(const char *fmt, ...)
{
	CHAR8 buf[QLOG_LINE_MAX];
	CHAR16 wbuf[QLOG_LINE_MAX];
	va_list ap;
	UINTN i, size;

	va_start(ap, fmt);
	AsciiVSPrint(buf, sizeof(buf), (CHAR8 *)fmt, ap);
	va_end(ap);

	/* Console output, like the previous Print() calls */
	AsciiPrint((CHAR8 *)"%a", buf);

	qlog_open();
	if (!qlog_file)
		return;

	for (i = 0; buf[i] && i < (QLOG_LINE_MAX - 1); i++)
		wbuf[i] = (CHAR16)(UINT8)buf[i];
	wbuf[i] = L'\0';

	size = i * sizeof(CHAR16);
	qlog_file->Write(qlog_file, &size, wbuf);
	qlog_file->Flush(qlog_file);
}
