/**
 * FileName: ReconLog.h
 *
 * A logger that is safe to call from DWM's render thread.
 *
 * The previous recon logger did fopen/fprintf/fclose per line, which stalled
 * the render thread badly enough to crash-loop dwm.exe. This one formats into
 * a stack buffer, appends to a fixed in-memory arena with a single atomic
 * fetch_add, and never touches the disk from the caller's thread. A private
 * worker thread flushes the arena to disk periodically so the data survives a
 * later crash.
*/
#pragma once
#include <Windows.h>

namespace MDWMBlurGlassExt::ReconLog
{
	// Starts the flusher thread and truncates the log file. Safe to call twice.
	void Start(const wchar_t* path);
	// Flushes what is buffered and stops the worker. Safe to call twice.
	void Stop();

	// Render-thread safe: no allocation, no I/O, no locks.
	void Write(const char* fmt, ...);
	// Flush now (from any thread that can afford blocking I/O).
	void Flush();
}
