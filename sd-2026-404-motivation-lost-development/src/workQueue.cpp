#include "workQueue.hpp"

#include <unistd.h>

void WorkQueue::enqueue(AcceptedSocket socket)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_stopped)
        {
            ::close(socket.fd);
            return;
        }

        m_queue.push(std::move(socket));
    }

    m_cv.notify_one();
}

std::optional<AcceptedSocket> WorkQueue::dequeue()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    m_cv.wait(lock, [this] { return !m_queue.empty() || m_stopped; });

    if (!m_queue.empty())
    {
        AcceptedSocket socket = std::move(m_queue.front());
        m_queue.pop();
        return socket;
    }

    return std::nullopt;
}

void WorkQueue::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_stopped)
        {
            return;
        }

        m_stopped = true;

        while (!m_queue.empty())
        {
            ::close(m_queue.front().fd);
            m_queue.pop();
        }
    }

    m_cv.notify_all();
}

std::size_t WorkQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

bool WorkQueue::isStopped() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stopped;
}
