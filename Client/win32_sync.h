#pragma once

#include <Windows.h>

namespace Win32Sync
{
class SharedLock
{
public:
    explicit SharedLock(SRWLOCK& lock) : m_lock(&lock)
    {
        AcquireSRWLockShared(m_lock);
    }

    ~SharedLock() { ReleaseSRWLockShared(m_lock); }
    SharedLock(const SharedLock&) = delete;
    SharedLock& operator=(const SharedLock&) = delete;

private:
    SRWLOCK* m_lock;
};

class ExclusiveLock
{
public:
    explicit ExclusiveLock(SRWLOCK& lock) : m_lock(&lock)
    {
        AcquireSRWLockExclusive(m_lock);
    }

    ~ExclusiveLock() { ReleaseSRWLockExclusive(m_lock); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

private:
    SRWLOCK* m_lock;
};
}
