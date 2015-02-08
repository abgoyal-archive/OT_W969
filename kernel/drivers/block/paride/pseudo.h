

	
#define PS_VERSION	"1.02"

#include <linux/sched.h>
#include <linux/workqueue.h>

static void ps_tq_int(struct work_struct *work);

static void (* ps_continuation)(void);
static int (* ps_ready)(void);
static unsigned long ps_timeout;
static int ps_tq_active = 0;
static int ps_nice = 0;

static DEFINE_SPINLOCK(ps_spinlock __attribute__((unused)));

static DECLARE_DELAYED_WORK(ps_tq, ps_tq_int);

static void ps_set_intr(void (*continuation)(void), 
			int (*ready)(void),
			int timeout, int nice)
{
	unsigned long	flags;

	spin_lock_irqsave(&ps_spinlock,flags);

	ps_continuation = continuation;
	ps_ready = ready;
	ps_timeout = jiffies + timeout;
	ps_nice = nice;

	if (!ps_tq_active) {
		ps_tq_active = 1;
		if (!ps_nice)
			schedule_delayed_work(&ps_tq, 0);
		else
			schedule_delayed_work(&ps_tq, ps_nice-1);
	}
	spin_unlock_irqrestore(&ps_spinlock,flags);
}

static void ps_tq_int(struct work_struct *work)
{
	void (*con)(void);
	unsigned long flags;

	spin_lock_irqsave(&ps_spinlock,flags);

	con = ps_continuation;
	ps_tq_active = 0;

	if (!con) {
		spin_unlock_irqrestore(&ps_spinlock,flags);
		return;
	}
	if (!ps_ready || ps_ready() || time_after_eq(jiffies, ps_timeout)) {
		ps_continuation = NULL;
		spin_unlock_irqrestore(&ps_spinlock,flags);
		con();
		return;
	}
	ps_tq_active = 1;
	if (!ps_nice)
		schedule_delayed_work(&ps_tq, 0);
	else
		schedule_delayed_work(&ps_tq, ps_nice-1);
	spin_unlock_irqrestore(&ps_spinlock,flags);
}

/* end of pseudo.h */

