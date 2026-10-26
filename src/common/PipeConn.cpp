#include "PipeConn.h"
#include <vector>

namespace lsc2 {

bool PipeConn::Start(HANDLE pipe, OnMessage onMessage, OnClosed onClosed)
{
	Close();
	pipe_ = pipe;
	onMessage_ = std::move(onMessage);
	onClosed_ = std::move(onClosed);
	stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	sendEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!stopEv_ || !sendEv_) return false;
	alive_ = true;
	th_ = std::thread([this] { Run(); });
	return true;
}

void PipeConn::Send(std::string msg)
{
	if (!alive_) return;
	{
		std::lock_guard<std::mutex> lk(mu_);
		outq_.push_back(std::move(msg));
	}
	SetEvent(sendEv_);
}

void PipeConn::Close()
{
	if (th_.joinable())
	{
		SetEvent(stopEv_);
		if (th_.get_id() == std::this_thread::get_id()) th_.detach();
		else th_.join();
	}
	if (pipe_ != INVALID_HANDLE_VALUE) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
	if (stopEv_) { CloseHandle(stopEv_); stopEv_ = nullptr; }
	if (sendEv_) { CloseHandle(sendEv_); sendEv_ = nullptr; }
	std::lock_guard<std::mutex> lk(mu_);
	outq_.clear();
	alive_ = false;
}

void PipeConn::Run()
{
	OVERLAPPED rov = {}, wov = {};
	rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::vector<char> rbuf(64 * 1024);
	std::string partial, writing;
	bool readPending = false, writePending = false, broken = false, stopped = false;

	while (!broken && !stopped)
	{
		if (!readPending)
		{
			ResetEvent(rov.hEvent);
			if (!ReadFile(pipe_, rbuf.data(), (DWORD)rbuf.size(), nullptr, &rov))
			{
				DWORD e = GetLastError();
				if (e != ERROR_IO_PENDING && e != ERROR_MORE_DATA) { broken = true; break; }
			}
			readPending = true;
		}
		if (!writePending)
		{
			{
				std::lock_guard<std::mutex> lk(mu_);
				if (!outq_.empty()) { writing = std::move(outq_.front()); outq_.pop_front(); writePending = true; }
			}
			if (writePending)
			{
				ResetEvent(wov.hEvent);
				if (!WriteFile(pipe_, writing.data(), (DWORD)writing.size(), nullptr, &wov) && GetLastError() != ERROR_IO_PENDING)
				{
					broken = true;
					break;
				}
			}
		}

		HANDLE hs[3] = {stopEv_, rov.hEvent, writePending ? wov.hEvent : sendEv_};
		DWORD w = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
		if (w == WAIT_OBJECT_0) { stopped = true; break; }
		if (w == WAIT_OBJECT_0 + 1)
		{
			DWORD n = 0;
			BOOL ok = GetOverlappedResult(pipe_, &rov, &n, FALSE);
			readPending = false;
			if (ok)
			{
				partial.append(rbuf.data(), n);
				if (onMessage_) onMessage_(std::move(partial));
				partial.clear();
			}
			else if (GetLastError() == ERROR_MORE_DATA) partial.append(rbuf.data(), n); // the rest follows
			else broken = true;
		}
		else if (w == WAIT_OBJECT_0 + 2 && writePending)
		{
			DWORD n = 0;
			if (!GetOverlappedResult(pipe_, &wov, &n, FALSE)) broken = true;
			writePending = false;
			writing.clear();
		}
		else if (w == WAIT_FAILED) broken = true;
	}

	// The OVERLAPPED blocks and buffers live on this stack: nothing may still be in flight.
	if (readPending || writePending)
	{
		CancelIoEx(pipe_, nullptr);
		DWORD n;
		if (readPending) GetOverlappedResult(pipe_, &rov, &n, TRUE);
		if (writePending) GetOverlappedResult(pipe_, &wov, &n, TRUE);
	}
	CloseHandle(rov.hEvent);
	CloseHandle(wov.hEvent);
	alive_ = false;
	if (!stopped && onClosed_) onClosed_();
}

} // namespace lsc2
