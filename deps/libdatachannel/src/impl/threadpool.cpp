/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "threadpool.hpp"
#include "utils.hpp"

#if defined(__ORBIS__)
#include <orbis/libkernel.h>
#endif

namespace rtc::impl {

ThreadPool &ThreadPool::Instance() {
	static ThreadPool *instance = new ThreadPool;
	return *instance;
}

ThreadPool::ThreadPool() {}

ThreadPool::~ThreadPool() {}

int ThreadPool::count() const {
	std::unique_lock lock(mWorkersMutex);
	return int(mWorkers.size());
}

void ThreadPool::spawn(int count) {
	std::unique_lock lock(mWorkersMutex);
	while (count-- > 0)
		mWorkers.emplace_back(std::bind(&ThreadPool::run, this));
}

void ThreadPool::join() {
	{
		std::unique_lock lock(mMutex);
		mWaitingCondition.wait(lock, [&]() { return mBusyWorkers == 0; });
		mJoining = true;
		mTasksCondition.notify_all();
	}

	std::unique_lock lock(mWorkersMutex);
	for (auto &w : mWorkers)
		w.join();

	mWorkers.clear();

	mJoining = false;
}

void ThreadPool::clear() {
	std::unique_lock lock(mMutex);
	while (!mTasks.empty())
		mTasks.pop();
}

void ThreadPool::run() {
	utils::this_thread::set_name("RTC worker");
	++mBusyWorkers;
	scope_guard guard([&]() { --mBusyWorkers; });
	while (runOne()) {
	}
}

bool ThreadPool::runOne() {
	if (auto task = dequeue()) {
		task();
		return true;
	}
	return false;
}

std::function<void()> ThreadPool::dequeue() {
	std::unique_lock lock(mMutex);
	while (!mJoining) {
		std::optional<clock::time_point> time;
		if (!mTasks.empty()) {
			time = mTasks.top().time;
			if (*time <= clock::now()) {
				auto func = std::move(mTasks.top().func);
				mTasks.pop();
				return func;
			}
		}

		--mBusyWorkers;
		scope_guard guard([&]() { ++mBusyWorkers; });
		mWaitingCondition.notify_all();
		if (time) {
#if defined(__ORBIS__)
			// Orbis' timed condition-variable wait can reach a nanosleep-style
			// syscall while holding libc's condition-variable lock. The PS4
			// rejects that with SIGSYS (copyin: has nonsleeping lock). Poll the
			// task deadline with short kernel sleeps instead, always releasing
			// the queue mutex so schedule()/join() can make progress. Rechecking
			// at most every millisecond also bounds delay for an earlier task
			// enqueued while this worker sleeps.
			auto remaining = *time - clock::now();
			if (remaining > clock::duration::zero()) {
				auto sleepUs = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
				if (sleepUs < 1)
					sleepUs = 1;
				if (sleepUs > 1000)
					sleepUs = 1000;
				lock.unlock();
				(void)sceKernelUsleep(static_cast<uint32_t>(sleepUs));
				lock.lock();
			}
#else
			mTasksCondition.wait_until(lock, *time);
#endif
		} else {
			mTasksCondition.wait(lock);
		}
	}
	return nullptr;
}

} // namespace rtc::impl
