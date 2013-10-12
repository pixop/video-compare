#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

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

		bool push(T &data, const size_t size);
		bool push(T &&data, const size_t size);
		bool pop(T &data);

		// Don't wait for more data when queue empty
		void set_finished();
		// Don't push or pop anymore:wq
		void set_quit();
};


template <class T>
Queue<T>::Queue(const size_t size_max) : size_total(0), size_limit(size_max) {
	quit = false;
	finished = false;
}

template <class T>
bool Queue<T>::push(T &data, const size_t size) {
	std::unique_lock<std::mutex> lock(m);

	for (;;) {
		if (quit) {
			return false;
		}

		else if (finished) {
			return false;
		}

		else if (size_total + size <= size_limit) {
			data_queue.push(std::move(data));
			size_queue.push(size);
			size_total += size;

			empty.notify_one();
			return true;
		}
		else {
			full.wait(lock);
		}
	}
};

template <class T>
bool Queue<T>::push(T &&data, const size_t size) {
	std::unique_lock<std::mutex> lock(m);

	for (;;) {
		if (quit) {
			return false;
		}

		else if (finished) {
			return false;
		}

		else if (size_total + size <= size_limit) {
			data_queue.push(std::move(data));
			size_queue.push(size);
			size_total += size;

			empty.notify_one();
			return true;
		}

		else {
			full.wait(lock);

		}
	}
};

template <class T>
bool Queue<T>::pop(T &data) {
	std::unique_lock<std::mutex> lock(m);

	for (;;) {
		if (quit) {
			return false;
		}

		if (!data_queue.empty()) {
			data = std::move(data_queue.front());
			data_queue.pop();
			size_total -= size_queue.front();
			size_queue.pop();
			full.notify_one();
			return true;
		}

		else if (finished) {
			return false;
		}

		else {
			empty.wait(lock);
		}
	}
};

template <class T>
void Queue<T>::set_finished() {
	finished = true;
	empty.notify_one();
}

template <class T>
void Queue<T>::set_quit() {
	quit = true;
	empty.notify_one();
	full.notify_one();
}
