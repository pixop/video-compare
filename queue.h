#pragma once
#include <iostream>

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

struct AVPacket;
struct AVFrame;

template <class T>
class Queue {
protected:
	// Data
	std::queue<T> data_queue;

	// Size of data
	std::queue<size_t> size_queue;
	size_t size_total;
	size_t size_limit;

	// Thread gubbins
	std::mutex m;
	std::condition_variable full;
	std::condition_variable empty;

	// Exit
	std::atomic_bool quit;
	std::atomic_bool finished;

public:
	Queue(const size_t size_max);

	bool push(T &&data, const size_t size);
	bool pop(T &data);

	// Don't wait for more data when queue empty
	void set_finished();
	// Don't push or pop anymore
	void set_quit();

};

typedef Queue<std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>>
	PacketQueue;
typedef Queue<std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>>
	FrameQueue;

template <class T>
Queue<T>::Queue(const size_t size_max) : size_total(0), size_limit(size_max) {
	quit = false;
	finished = false;
}

template <class T>
bool Queue<T>::push(T &&data, const size_t size) {
	std::unique_lock<std::mutex> lock(m);

	while (!quit && !finished) {

		if (size_total + size <= size_limit) {
			data_queue.push(std::move(data));
			size_queue.push(size);
			size_total += size;

			empty.notify_all();
			return true;
		} else {
			full.wait(lock);
		}
	}

	return false;
};

template <class T>
bool Queue<T>::pop(T &data) {
	std::unique_lock<std::mutex> lock(m);

	while (!quit) {

		if (!data_queue.empty()) {
			data = std::move(data_queue.front());
			data_queue.pop();
			size_total -= size_queue.front();
			size_queue.pop();

			full.notify_all();
			return true;
		} else if (data_queue.empty() && finished) {
			return false;
		} else {
			empty.wait(lock);
		}
	}

	return false;
};

template <class T>
void Queue<T>::set_finished() {
	finished = true;
	empty.notify_all();
}

template <class T>
void Queue<T>::set_quit() {
	quit = true;
	empty.notify_all();
	full.notify_all();
}
