#include "Actor.hpp"

#include <numeric>

using namespace laugh;

// ActorContext {{{


ActorContext::ActorContext(int workers)
{
    m_terminationDesired.clear();
    for(int i = 0; i < workers; ++i)
    {
        SpawnWorker();
    }
}

void ActorContext::SpawnWorker()
{
    std::atomic_flag isWorkerEnrolled;
    isWorkerEnrolled.clear();

    std::thread t{&ActorContext::WorkerRun, std::ref(*this), &isWorkerEnrolled};
    std::thread::id tid = t.get_id();

    isWorkerEnrolled.wait(false);

    std::lock_guard<decltype(m_threadInfoBottleneck)> _{m_threadInfoBottleneck};
    m_threadsInfo[tid].m_worker = std::move(t);
}

void ActorContext::WorkerRun(std::atomic_flag* isWorkerEnrolled)
{
    {
        std::lock_guard<decltype(m_threadInfoBottleneck)> _{m_threadInfoBottleneck};

        m_threadsInfo[std::this_thread::get_id()].m_waiting = true;

        isWorkerEnrolled->test_and_set();
        isWorkerEnrolled->notify_all();
    }

    // By now, the atomic flag's destructor has likely already been
    // called, this is a dangling pointer.
    isWorkerEnrolled = nullptr;

    std::unique_ptr<Task> current;
    ThreadContext& info = m_threadsInfo[std::this_thread::get_id()];

    while(true)
    {
        {
            std::shared_lock<std::shared_mutex> lk{m_workSchedule};

            m_hasWork.wait(lk, [&info, &current, this]()
            {
                const bool willWork = !(info.m_waiting = !m_unassigned.try_dequeue(current));
                if (m_terminationDesired.test())
                {
                    const bool threadsIdling = std::accumulate(m_threadsInfo.cbegin(), m_threadsInfo.cend(), true, [](bool c, const auto& pair) -> bool { return c && pair.second.m_waiting; });
                    return willWork || threadsIdling; 
                }
                else
                {
                    return willWork;
                }
            });
        }

        m_hasWork.notify_all();

        if(current)
        {
            current->Let();
            current = nullptr;
        }
        else
        {
            return;
        }
    }
}

ActorContext::TerminateMessage
     ::TerminateMessage(ActorContext& self):
    m_self{self}
{
}

void ActorContext::TerminateMessage::Let()
{
    m_self.m_terminationDesired.test_and_set();
}

void ActorContext::Terminate()
{
    ScheduleMessage(std::make_unique<TerminateMessage>(*this));
}

void ActorContext::ScheduleMessage(std::unique_ptr<Task>&& task)
{
    {
        std::lock_guard<std::shared_mutex> lck{m_workSchedule};
        m_unassigned.enqueue(std::move(task));
    }
    m_hasWork.notify_all();
}

ActorContext::~ActorContext()
{
    std::lock_guard<std::mutex> lck{m_threadInfoBottleneck};
    Terminate();

    for(auto& [tid, ct]: m_threadsInfo)
    {
        ct.m_worker.join();
    }
}


// }}}


ActorLock::ActorLock(ActorCell& cell):
    m_cell{cell}
  , m_lock{m_cell.m_blocking}
{
}


void ActorLock::RestartWithDefaultConstructor()
{
    if(m_cell.m_defaultConstruct == nullptr)
    {
        throw std::exception();
    }
    else
    {
        m_cell.m_actor = m_cell.m_defaultConstruct();
        m_cell.m_actor->m_self = m_cell.m_selfReference;
    }
}


void ActorCell::TerminateDyingActor()
{
    m_dyingActor = nullptr;
}


bool ActorCell::IsAutomaticallyResettable() const
{
    return m_defaultConstruct != nullptr;
}


ActorLock ActorCell::LockActor()
{
    return ActorLock{*this};
}


ActorCell::ActorCell(const ActorRef<Actor>& parent
                   , ActorContext* context):
    m_parent{parent}
  , m_context{context}
  , m_defaultConstruct{nullptr}
  , m_actor{nullptr}
  , m_selfReference()
{
    if(!context)
    {
        throw 1;
    }
}


ActorCell::ActorCell(std::nullptr_t parent
                   , ActorContext* context):
    m_parent{std::nullopt}
  , m_context{context}
  , m_defaultConstruct{nullptr}
  , m_actor{nullptr}
  , m_selfReference()
{
    if(!context)
    {
        throw 1;
    }
}

