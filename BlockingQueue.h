#pragma once

#include <iostream>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stop_token>
#include <utility>

template<class T>
class BlockingQueue
{
public:
    explicit BlockingQueue(std::size_t capacity)
        : m_capacity(capacity == 0 ? 1 : capacity)
    {
    }

    bool Push(T item)
    {
        std::unique_lock lock(m_mutex);
        m_notFull.wait(lock, [this] { return m_closed || m_queue.size() < m_capacity; });

        if (m_closed)
            return false;

        m_queue.push_back(std::move(item));
        lock.unlock();
        m_notEmpty.notify_one();
        return true;
    }

    // Suitable for real-time video frames, not compressed packets or audio.
    bool PushDropOldest(T item)
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_closed)
                return false;

            if (m_queue.size() >= m_capacity)
            {
                std::cout << "[VideoQueue] Dropped oldest frame\n";
                m_queue.pop_front();
            }

            m_queue.push_back(std::move(item));
        }

        m_notEmpty.notify_one();
        return true;
    }

    bool Pop(T& item, std::stop_token stopToken = {})
    {
        std::unique_lock lock(m_mutex);
        const bool ready = m_notEmpty.wait(lock, stopToken, [this] { return m_closed || !m_queue.empty(); });

        if (!ready || m_queue.empty())
            return false;

        item = std::move(m_queue.front());
        m_queue.pop_front();

        lock.unlock();
        m_notFull.notify_one();

        return true;
    }

    bool TryPop(T& item)
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_queue.empty())
                return false;

            item = std::move(m_queue.front());
            m_queue.pop_front();
        }

        m_notFull.notify_one();

        return true;
    }

    bool Empty() const
    {
        std::lock_guard lock(m_mutex);
        return m_queue.empty();
    }

    std::size_t Size() const
    {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }

    void Close()
    {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
        }

        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

    void Reset()
    {
        {
            std::lock_guard lock(m_mutex);
            m_queue.clear();
            m_closed = false;
        }

        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable_any m_notEmpty;
    std::condition_variable m_notFull;
    std::deque<T> m_queue;
    const std::size_t m_capacity;
    bool m_closed = false;
};
