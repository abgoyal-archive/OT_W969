

#include "config.h"
#include "DOMTimer.h"

#include "InspectorTimelineAgent.h"
#include "ScheduledAction.h"
#include "ScriptExecutionContext.h"
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>

using namespace std;

namespace WebCore {

static const int maxTimerNestingLevel = 5;
static const double oneMillisecond = 0.001;
double DOMTimer::s_minTimerInterval = 0.010; // 10 milliseconds

static int timerNestingLevel = 0;

DOMTimer::DOMTimer(ScriptExecutionContext* context, ScheduledAction* action, int timeout, bool singleShot)
    : ActiveDOMObject(context, this)
    , m_action(action)
    , m_nextFireInterval(0)
    , m_repeatInterval(0)
#if !ASSERT_DISABLED
    , m_suspended(false)
#endif
{
    static int lastUsedTimeoutId = 0;
    ++lastUsedTimeoutId;
    // Avoid wraparound going negative on us.
    if (lastUsedTimeoutId <= 0)
        lastUsedTimeoutId = 1;
    m_timeoutId = lastUsedTimeoutId;

    m_nestingLevel = timerNestingLevel + 1;

    scriptExecutionContext()->addTimeout(m_timeoutId, this);

    double intervalMilliseconds = max(oneMillisecond, timeout * oneMillisecond);

    // Use a minimum interval of 10 ms to match other browsers, but only once we've
    // nested enough to notice that we're repeating.
    // Faster timers might be "better", but they're incompatible.
    if (intervalMilliseconds < s_minTimerInterval && m_nestingLevel >= maxTimerNestingLevel)
        intervalMilliseconds = s_minTimerInterval;
    if (singleShot)
        startOneShot(intervalMilliseconds);
    else
        startRepeating(intervalMilliseconds);
}

DOMTimer::~DOMTimer()
{
    if (scriptExecutionContext())
        scriptExecutionContext()->removeTimeout(m_timeoutId);
}

int DOMTimer::install(ScriptExecutionContext* context, ScheduledAction* action, int timeout, bool singleShot)
{
    // DOMTimer constructor links the new timer into a list of ActiveDOMObjects held by the 'context'.
    // The timer is deleted when context is deleted (DOMTimer::contextDestroyed) or explicitly via DOMTimer::removeById(),
    // or if it is a one-time timer and it has fired (DOMTimer::fired).
    DOMTimer* timer = new DOMTimer(context, action, timeout, singleShot);

#if ENABLE(INSPECTOR)
    if (InspectorTimelineAgent* timelineAgent = InspectorTimelineAgent::retrieve(context))
        timelineAgent->didInstallTimer(timer->m_timeoutId, timeout, singleShot);
#endif    

    return timer->m_timeoutId;
}

void DOMTimer::removeById(ScriptExecutionContext* context, int timeoutId)
{
    // timeout IDs have to be positive, and 0 and -1 are unsafe to
    // even look up since they are the empty and deleted value
    // respectively
    if (timeoutId <= 0)
        return;

#if ENABLE(INSPECTOR)
    if (InspectorTimelineAgent* timelineAgent = InspectorTimelineAgent::retrieve(context))
        timelineAgent->didRemoveTimer(timeoutId);
#endif

    delete context->findTimeout(timeoutId);
}

void DOMTimer::fired()
{
    ScriptExecutionContext* context = scriptExecutionContext();
    timerNestingLevel = m_nestingLevel;

#if ENABLE(INSPECTOR)
    if (InspectorTimelineAgent* timelineAgent = InspectorTimelineAgent::retrieve(context))
        timelineAgent->willFireTimer(m_timeoutId);
#endif

    // Simple case for non-one-shot timers.
    if (isActive()) {
        if (repeatInterval() && repeatInterval() < s_minTimerInterval) {
            m_nestingLevel++;
            if (m_nestingLevel >= maxTimerNestingLevel)
                augmentRepeatInterval(s_minTimerInterval - repeatInterval());
        }

        // No access to member variables after this point, it can delete the timer.
        m_action->execute(context);
#if ENABLE(INSPECTOR)
        if (InspectorTimelineAgent* timelineAgent = InspectorTimelineAgent::retrieve(context))
            timelineAgent->didFireTimer();
#endif
        return;
    }

    // Delete timer before executing the action for one-shot timers.
    ScheduledAction* action = m_action.release();

    // No access to member variables after this point.
    delete this;

    action->execute(context);
#if ENABLE(INSPECTOR)
    if (InspectorTimelineAgent* timelineAgent = InspectorTimelineAgent::retrieve(context))
        timelineAgent->didFireTimer();
#endif
    delete action;
    timerNestingLevel = 0;
}

bool DOMTimer::hasPendingActivity() const
{
    return isActive();
}

void DOMTimer::contextDestroyed()
{
    ActiveDOMObject::contextDestroyed();
    delete this;
}

void DOMTimer::stop()
{
    TimerBase::stop();
    // Need to release JS objects potentially protected by ScheduledAction
    // because they can form circular references back to the ScriptExecutionContext
    // which will cause a memory leak.
    m_action.clear();
}

void DOMTimer::suspend()
{
#if !ASSERT_DISABLED
    ASSERT(!m_suspended);
    m_suspended = true;
#endif
    m_nextFireInterval = nextFireInterval();
    m_repeatInterval = repeatInterval();
    TimerBase::stop();
}

void DOMTimer::resume()
{
#if !ASSERT_DISABLED
    ASSERT(m_suspended);
    m_suspended = false;
#endif
    start(m_nextFireInterval, m_repeatInterval);
}


bool DOMTimer::canSuspend() const
{
    return true;
}

} // namespace WebCore
