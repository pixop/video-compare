#pragma once

//std::atomic_bool quit(false);

template <class T>
class Queue {
	protected:
		std::queue<T> q;
		std::mutex m;
		std::condition_variable cv;
	public:
		virtual ~Queue() {
		};
		virtual void push(T &data) {
			std::lock_guard<std::mutex> lock(m);

			std::clog << "Push" << std::endl;
			q.push(std::move(data));

			cv.notify_one();
		};
		virtual void push(T &&data) {
			std::lock_guard<std::mutex> lock(m);

			std::clog << "Push" << std::endl;
			q.push(std::move(data));

			cv.notify_one();
		};
		virtual bool pop(T &data) {
			std::unique_lock<std::mutex> lock(m);

			for(;;) {
				//if (quit) {
				//	std::clog << "Quit" << std::endl;
				//	return false;
				//}

				if (!q.empty()) {
					std::clog << "Pop" << std::endl;
					data = std::move(q.front());
					q.pop();
					return true;
				}

				else {
					std::clog << "Wait for data" << std::endl;
					cv.wait(lock);
				}
			}
		};
		virtual bool full (size_t max) {
			bool ret = q.size() > max;
			if (ret) {
				std::clog << "Queue full" << std::endl;
			}

			return ret;
		};
};
		       
class PacketQueue : public Queue<AVPacket>{
	private:
		size_t size;
	public:
		PacketQueue() :  size(0) {
		};
		virtual void push(AVPacket &packet) override {
			std::lock_guard<std::mutex> lock(m);

			std::clog << "Push" << std::endl;
			q.push(std::move(packet));	
			size += packet.size;

			cv.notify_one();
		};
		virtual void push(AVPacket &&packet) override {
			std::lock_guard<std::mutex> lock(m);

			std::clog << "Push" << std::endl;
			q.push(std::move(packet));
			size += packet.size;

			cv.notify_one();
		};

		virtual bool pop(AVPacket &packet) override {
			std::unique_lock<std::mutex> lock(m);

			for(;;) {
				//if (quit) {
				//	std::clog << "Quit" << std::endl;
				//	return false;
				//}

				if (!q.empty()) {
					std::clog << "Pop" << std::endl;
					packet = std::move(q.front());
					q.pop();
					size -= packet.size;
					return true;
				}

				else {
					std::clog << "Wait for data" << std::endl;
					cv.wait(lock);
				}
			}
		};
		virtual bool full (size_t max) override {
			bool ret = size > max;
			if (ret) {
				std::clog << "Queue full" << std::endl;
			}

			return ret;
		};
};
