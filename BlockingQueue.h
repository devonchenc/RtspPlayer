#pragma once

#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stop_token>

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
        {
            std::unique_lock lock(m_mutex);
            m_notFull.wait(lock, [this] { return m_closed || m_queue.size() < m_capacity; });

            if (m_closed)
                return false;

            m_queue.push_back(std::move(item));
        }

        m_notEmpty.notify_one();
        return true;
    }

    bool PushWait(T item, std::stop_token stopToken)
    {
        {
            std::unique_lock lock(m_mutex);

            const bool ready = m_notFull.wait(lock, stopToken, [this] { return m_closed || m_dropOldestWhenFull || m_queue.size() < m_capacity; });

            if (!ready || m_closed || stopToken.stop_requested())
                return false;

            if (m_dropOldestWhenFull && m_queue.size() >= m_capacity)
            {
                m_queue.pop_front();
            }

            m_queue.push_back(std::move(item));
        }

        m_notEmpty.notify_one();

        return true;
    }

    bool PushDropOldest(T item)
    {
        {
            std::lock_guard lock(m_mutex);

            if (m_closed)
                return false;

            if (m_queue.size() >= m_capacity)
            {
                m_queue.pop_front();
            }

            m_queue.push_back(std::move(item));
        }

        m_notEmpty.notify_one();

        return true;
    }

    bool Pop(T& item, std::stop_token stopToken = {})
    {
        {
            std::unique_lock lock(m_mutex);

            const bool ready = m_notEmpty.wait(lock, stopToken, [this] { return m_closed || !m_queue.empty(); });

            if (!ready || m_queue.empty())
                return false;

            item = std::move(m_queue.front());
            m_queue.pop_front();
        }

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

    void SetDropOldestWhenFull(bool enabled)
    {
        {
            std::lock_guard lock(m_mutex);
            m_dropOldestWhenFull = enabled;
        }

        m_notFull.notify_all();
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

    void SetCapacity(size_t capacity)
    {
        {
            std::lock_guard lock(m_mutex);
            m_capacity = capacity;
        }

        m_notFull.notify_all();
    }

    void Clear()
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
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
    std::condition_variable_any m_notFull;
    std::deque<T> m_queue;
    std::size_t m_capacity{ 0 };
    bool m_closed{ false };
    bool m_dropOldestWhenFull{ false };
};
