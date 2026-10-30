// SPDX-License-Identifier: MIT
/*
 * pw_irq.c - interrupt top/bottom halves for the three Mali lines, and register polling.
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT): the raw/threaded handler pair
 * in panthor_device.h:560-680. Differences:
 *  - The ISR (pw_irq_top) cannot take locks (pw_os.h has no spinlock), so the line state is an
 *    atomic and transitions ACTIVE->PROCESSING use compare-and-swap.
 *  - If the line asserts while already PROCESSING (a passive-level mask update raced with the
 *    ISR), the ISR masks the line again and claims it instead of returning "not mine": an
 *    unclaimed level-triggered interrupt would otherwise storm until the bottom half ran.
 *    The extra bottom-half run is harmless (it only processes RAWSTAT & mask).
 */

#include "pw_priv.h"

static inline uint32_t irq_state_get(struct pw_irq *irq)
{
	return __atomic_load_n(&irq->state, __ATOMIC_ACQUIRE);
}

static inline void irq_state_set(struct pw_irq *irq, uint32_t st)
{
	__atomic_store_n(&irq->state, st, __ATOMIC_RELEASE);
}

int pw_irq_init(struct pw_device *dev, enum pw_irq_line line, uint32_t base,
		pw_irq_handler_t handler)
{
	struct pw_irq *irq = &dev->irq[line];

	irq->base = base;
	irq->mask = 0;
	irq->handler = handler;
	irq_state_set(irq, PW_IRQ_STATE_SUSPENDED);
	if (!irq->lock)
		irq->lock = pw_os_mutex_create(dev->os);
	if (!irq->bh_lock)
		irq->bh_lock = pw_os_mutex_create(dev->os);
	if (!irq->lock || !irq->bh_lock)
		return PW_ENOMEM;

	gpu_write(dev, base + INT_MASK, 0);
	return PW_OK;
}

void pw_irq_fini(struct pw_device *dev, enum pw_irq_line line)
{
	struct pw_irq *irq = &dev->irq[line];

	if (irq->lock)
		pw_os_mutex_destroy(dev->os, irq->lock);
	if (irq->bh_lock)
		pw_os_mutex_destroy(dev->os, irq->bh_lock);
	irq->lock = NULL;
	irq->bh_lock = NULL;
}

void pw_irq_enable_events(struct pw_device *dev, enum pw_irq_line line, uint32_t mask)
{
	struct pw_irq *irq = &dev->irq[line];

	pw_os_mutex_lock(irq->lock);
	irq->mask |= mask;
	/* While PROCESSING the bottom half restores the mask; while SUSPENDED, resume does. */
	if (irq_state_get(irq) == PW_IRQ_STATE_ACTIVE)
		gpu_write(dev, irq->base + INT_MASK, irq->mask);
	pw_os_mutex_unlock(irq->lock);
}

void pw_irq_disable_events(struct pw_device *dev, enum pw_irq_line line, uint32_t mask)
{
	struct pw_irq *irq = &dev->irq[line];

	pw_os_mutex_lock(irq->lock);
	irq->mask &= ~mask;
	if (irq_state_get(irq) == PW_IRQ_STATE_ACTIVE)
		gpu_write(dev, irq->base + INT_MASK, irq->mask);
	pw_os_mutex_unlock(irq->lock);
}

void pw_irq_resume(struct pw_device *dev, enum pw_irq_line line)
{
	struct pw_irq *irq = &dev->irq[line];

	pw_os_mutex_lock(irq->lock);
	irq_state_set(irq, PW_IRQ_STATE_ACTIVE);
	gpu_write(dev, irq->base + INT_CLEAR, irq->mask);
	gpu_write(dev, irq->base + INT_MASK, irq->mask);
	pw_os_mutex_unlock(irq->lock);
}

/*
 * Note: unlike Linux there is no synchronize_irq(); the OS layer must make sure no ISR or bottom
 * half for this device is running/queued before pw_device_destroy() frees it.
 */
void pw_irq_suspend(struct pw_device *dev, enum pw_irq_line line)
{
	struct pw_irq *irq = &dev->irq[line];

	if (!irq->lock)
		return;
	pw_os_mutex_lock(irq->lock);
	/* Mask first: an ISR racing with us then sees INT_STAT == 0 rather than an asserted line
	 * it would have to refuse because the state is already SUSPENDED. */
	gpu_write(dev, irq->base + INT_MASK, 0);
	irq_state_set(irq, PW_IRQ_STATE_SUSPENDED);
	pw_os_mutex_unlock(irq->lock);
}

void pw_irq_rearm_all(struct pw_device *dev)
{
	for (unsigned int i = 0; i < PW_IRQ_COUNT; i++) {
		struct pw_irq *irq = &dev->irq[i];

		if (!irq->lock)
			continue;
		pw_os_mutex_lock(irq->lock);
		if (irq_state_get(irq) == PW_IRQ_STATE_ACTIVE)
			gpu_write(dev, irq->base + INT_MASK, irq->mask);
		pw_os_mutex_unlock(irq->lock);
	}
}

/* ISR: high IRQL, no locks, no allocation. */
bool pw_irq_top(struct pw_device *dev, enum pw_irq_line line)
{
	struct pw_irq *irq;
	uint32_t st, expected;

	if (!dev || (unsigned int)line >= PW_IRQ_COUNT)
		return false;
	irq = &dev->irq[line];

	st = irq_state_get(irq);
	if (st == PW_IRQ_STATE_SUSPENDED || !irq->handler)
		return false;

	if (!gpu_read(dev, irq->base + INT_STAT))
		return false;

	if (st == PW_IRQ_STATE_ACTIVE) {
		expected = PW_IRQ_STATE_ACTIVE;
		if (!__atomic_compare_exchange_n(&irq->state, &expected, PW_IRQ_STATE_PROCESSING,
						 false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
		    expected == PW_IRQ_STATE_SUSPENDED)
			return false;	/* suspend raced us and already wrote MASK = 0 */
	}

	gpu_write(dev, irq->base + INT_MASK, 0);
	__atomic_fetch_add(&irq->top_count, 1, __ATOMIC_RELAXED);
	return true;
}

/* Bottom half: passive level. */
void pw_irq_bottom(struct pw_device *dev, enum pw_irq_line line)
{
	struct pw_irq *irq;

	if (!dev || (unsigned int)line >= PW_IRQ_COUNT)
		return;
	irq = &dev->irq[line];
	if (!irq->bh_lock || !irq->handler)
		return;

	pw_os_mutex_lock(irq->bh_lock);
	irq->bottom_count++;

	/* As panthor: keep handling until RAWSTAT & mask is empty, so events raised while we
	 * were busy are not lost. Reading mask without the lock is a benign race (see
	 * panthor_device.h:592). The loop is bounded to survive a stuck status bit. */
	for (unsigned int iter = 0; iter < 1000; iter++) {
		uint32_t status = gpu_read(dev, irq->base + INT_RAWSTAT) &
				  PW_READ_ONCE(irq->mask);

		if (!status)
			break;
		irq->handler(dev, status);
	}

	/*
	 * Deviation from panthor (which only re-arms from PROCESSING): also re-write the mask when
	 * already ACTIVE. An ISR that sampled PROCESSING (see the header comment) may write
	 * INT_MASK = 0 right after a previous bottom half re-armed the line; it then claims and
	 * queues us, and we are the only one left to restore the mask.
	 */
	pw_os_mutex_lock(irq->lock);
	if (irq_state_get(irq) != PW_IRQ_STATE_SUSPENDED) {
		irq_state_set(irq, PW_IRQ_STATE_ACTIVE);
		gpu_write(dev, irq->base + INT_MASK, irq->mask);
	}
	pw_os_mutex_unlock(irq->lock);
	pw_os_mutex_unlock(irq->bh_lock);
}

int pw_poll_reg32(struct pw_device *dev, uint32_t reg, uint32_t mask, uint32_t expect,
		  uint32_t delay_us, uint32_t timeout_us, uint32_t *last)
{
	uint64_t deadline = pw_os_time_ns(dev->os) + (uint64_t)timeout_us * 1000u;
	uint32_t v;

	for (;;) {
		bool expired = pw_os_time_ns(dev->os) > deadline;

		v = gpu_read(dev, reg);
		if ((v & mask) == expect)
			break;
		if (expired) {
			if (last)
				*last = v;
			return PW_ETIMEDOUT;
		}
		if (delay_us)
			pw_os_udelay(dev->os, delay_us);
	}
	if (last)
		*last = v;
	return PW_OK;
}

int pw_poll_reg64(struct pw_device *dev, uint32_t reg, uint64_t mask, uint64_t expect,
		  uint32_t delay_us, uint32_t timeout_us, uint64_t *last)
{
	uint64_t deadline = pw_os_time_ns(dev->os) + (uint64_t)timeout_us * 1000u;
	uint64_t v;

	for (;;) {
		bool expired = pw_os_time_ns(dev->os) > deadline;

		v = gpu_read64(dev, reg);
		if ((v & mask) == expect)
			break;
		if (expired) {
			if (last)
				*last = v;
			return PW_ETIMEDOUT;
		}
		if (delay_us)
			pw_os_udelay(dev->os, delay_us);
	}
	if (last)
		*last = v;
	return PW_OK;
}
