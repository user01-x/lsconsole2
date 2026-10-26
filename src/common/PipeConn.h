#pragma once
// One end of a message-mode named pipe, driven by its own I/O thread with overlapped reads and
// writes. Overlapped because a blocking ReadFile on a synchronous handle stalls every WriteFile on
// the same handle until a message arrives.
//
// Send() never blocks: it queues. Callbacks run on the I/O thread; OnClosed must not call Close().
#include <windows.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lsc2 {

class PipeConn {
public:
	using OnMessage = std::function<void(std::string &&)>;
	using OnClosed = std::function<void()>;

	PipeConn() = default;
	~PipeConn() { Close(); }
	PipeConn(const PipeConn &) = delete;
	PipeConn &operator=(const PipeConn &) = delete;

	// Takes ownership of `pipe` (opened with FILE_FLAG_OVERLAPPED, in message read mode).
	bool Start(HANDLE pipe, OnMessage onMessage, OnClosed onClosed);
	void Send(std::string msg);
	void Close();
	bool Alive() const { return alive_; }

private:
	void Run();

	HANDLE pipe_ = INVALID_HANDLE_VALUE;
	HANDLE stopEv_ = nullptr;
	HANDLE sendEv_ = nullptr;
	std::mutex mu_;
	std::deque<std::string> outq_;
	std::thread th_;
	std::atomic<bool> alive_{false};
	OnMessage onMessage_;
	OnClosed onClosed_;
};

} // namespace lsc2
