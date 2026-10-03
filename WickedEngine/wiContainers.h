#pragma once
#include "wiSpinLock.h"
#ifdef GGREDUCED
#include <mutex>
#endif

namespace wiContainers
{
	// Fixed size very simple thread safe ring buffer
	template <typename T, size_t capacity>
	class ThreadSafeRingBuffer
	{
	public:
		// Push an item to the end if there is free space
		//	Returns true if succesful
		//	Returns false if there is not enough space
		inline bool push_back(const T& item)
		{
			bool result = false;
			lock.lock();
			size_t next = (head + 1) % capacity;
			if (next != tail)
			{
				data[head] = item;
				head = next;
				result = true;
			}
			lock.unlock();
			return result;
		}

#ifdef GGREDUCED
		// GG: the item moved in, so a copy that allocates (a job's std::function) is made before the lock, not under it;
		// left as it was if there is no space
		inline bool push_back(T&& item)
		{
			bool result = false;
			lock.lock();
			size_t next = (head + 1) % capacity;
			if (next != tail)
			{
				data[head] = std::move(item);
				head = next;
				result = true;
			}
			lock.unlock();
			return result;
		}
#endif

		// Get an item if there are any
		//	Returns true if succesful
		//	Returns false if there are no items
		inline bool pop_front(T& item)
		{
			bool result = false;
			lock.lock();
			if (tail != head)
			{
#ifdef GGREDUCED
				item = std::move(data[tail]); // GG: no copy under the lock
#else
				item = data[tail];
#endif
				tail = (tail + 1) % capacity;
				result = true;
			}
			lock.unlock();
			return result;
		}

	private:
		T data[capacity];
		size_t head = 0;
		size_t tail = 0;
#ifdef GGREDUCED
		// GG: a lock whose waiters sleep after a short spin. With the spin lock every woken job worker and the waiting main
		// thread spun on it, and when Windows switched out the thread holding it they all spun until it ran again, one or two
		// scheduler ticks (15-31 ms): the 20-50 ms a tiny job sat before any thread started it, about 7 frames a second
		std::mutex lock;
#else
		wiSpinLock lock;
#endif
	};
}
