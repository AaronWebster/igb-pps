/*******************************************************************************

  Intel(R) Gigabit Ethernet Linux driver
  Copyright(c) 2007-2012 Intel Corporation.

  This program is free software; you can redistribute it and/or modify it
  under the terms and conditions of the GNU General Public License,
  version 2, as published by the Free Software Foundation.

  This program is distributed in the hope it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  more details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.

  The full GNU General Public License is included in this distribution in
  the file called "COPYING".

  Contact Information:
  e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

/******************************************************************************
 Copyright(c) 2011 Richard Cochran <richardcochran@gmail.com> for some of the
 82576 and 82580 code
 Copyright(c) 2013 Balint Ferencz <fernya@sch.bme.hu> for some of the i350 and
 i210 clock features code
******************************************************************************/

#include "igb.h"

#ifdef CONFIG_IGB_PTP
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pci.h>

#define INCVALUE_MASK		0x7fffffff
#define ISGN			0x80000000

/*
 * The 82580 timesync updates the system timer every 8ns by 8ns,
 * and this update value cannot be reprogrammed.
 *
 * Neither the 82576 nor the 82580 offer registers wide enough to hold
 * nanoseconds time values for very long. For the 82580, SYSTIM always
 * counts nanoseconds, but the upper 24 bits are not availible. The
 * frequency is adjusted by changing the 32 bit fractional nanoseconds
 * register, TIMINCA.
 *
 * For the 82576, the SYSTIM register time unit is affect by the
 * choice of the 24 bit TININCA:IV (incvalue) field. Five bits of this
 * field are needed to provide the nominal 16 nanosecond period,
 * leaving 19 bits for fractional nanoseconds.
 *
 * We scale the NIC clock cycle by a large factor so that relatively
 * small clock corrections can be added or subtracted at each clock
 * tick. The drawbacks of a large factor are a) that the clock
 * register overflows more quickly (not such a big deal) and b) that
 * the increment per tick has to fit into 24 bits.  As a result we
 * need to use a shift of 19 so we can fit a value of 16 into the
 * TIMINCA register.
 *
 *
 *             SYSTIMH            SYSTIML
 *        +--------------+   +---+---+------+
 *  82576 |      32      |   | 8 | 5 |  19  |
 *        +--------------+   +---+---+------+
 *         \________ 45 bits _______/  fract
 *
 *        +----------+---+   +--------------+
 *  82580 |    24    | 8 |   |      32      |
 *        +----------+---+   +--------------+
 *          reserved  \______ 40 bits _____/
 *
 *
 * The 45 bit 82576 SYSTIM overflows every
 *   2^45 * 10^-9 / 3600 = 9.77 hours.
 *
 * The 40 bit 82580 SYSTIM overflows every
 *   2^40 * 10^-9 /  60  = 18.3 minutes.
 */

#define IGB_SYSTIM_OVERFLOW_PERIOD	(HZ * 60 * 9)
#define INCPERIOD_82576			(1 << E1000_TIMINCA_16NS_SHIFT)
#define INCVALUE_82576_MASK		((1 << E1000_TIMINCA_16NS_SHIFT) - 1)
#define INCVALUE_82576			(16 << IGB_82576_TSYNC_SHIFT)
#define IGB_NBITS_82580			40

/*
 * SYSTIM read access for the 82576
 */

static cycle_t igb_ptp_read_82576(const struct cyclecounter *cc)
{
	struct igb_adapter *igb = container_of(cc, struct igb_adapter, cc);
	struct e1000_hw *hw = &igb->hw;
	u64 val;
	u32 lo, hi;

	lo = E1000_READ_REG(hw, E1000_SYSTIML);
	hi = E1000_READ_REG(hw, E1000_SYSTIMH);

	val = ((u64) hi) << 32;
	val |= lo;

	return val;
}

/*
 * SYSTIM read access for the 82580
 */

static cycle_t igb_ptp_read_82580(const struct cyclecounter *cc)
{
	struct igb_adapter *igb = container_of(cc, struct igb_adapter, cc);
	struct e1000_hw *hw = &igb->hw;
	u64 val;
	u32 lo, hi, jk;

	/*
	 * The timestamp latches on lowest register read. For the 82580
	 * the lowest register is SYSTIMR instead of SYSTIML.  However we only
	 * need to provide nanosecond resolution, so we just ignore it.
	 */
	jk = E1000_READ_REG(hw, E1000_SYSTIMR);
	lo = E1000_READ_REG(hw, E1000_SYSTIML);
	hi = E1000_READ_REG(hw, E1000_SYSTIMH);

	val = ((u64) hi) << 32;
	val |= lo;

	return val;
}

/*
 * SYSTIM read access for I210/I211
 */

static void igb_ptp_read_i210(struct igb_adapter *adapter, struct timespec *ts)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 sec, nsec, jk;

	/*
	 * The timestamp latches on lowest register read. For I210/I211, the
	 * lowest register is SYSTIMR. Since we only need to provide nanosecond
	 * resolution, we can ignore it.
	 */
	jk = E1000_READ_REG(hw, E1000_SYSTIMR);
	nsec = E1000_READ_REG(hw, E1000_SYSTIML);
	sec = E1000_READ_REG(hw, E1000_SYSTIMH);

	ts->tv_sec = sec;
	ts->tv_nsec = nsec;
}

static void igb_ptp_write_i210(struct igb_adapter *adapter,
			       const struct timespec *ts)
{
	struct e1000_hw *hw = &adapter->hw;

	/*
	 * Writing the SYSTIMR register is not necessary as it only provides
	 * sub-nanosecond resolution.
	 */
	E1000_WRITE_REG(hw, E1000_SYSTIMR, 0);
	E1000_WRITE_REG(hw, E1000_SYSTIML, ts->tv_nsec);
	E1000_WRITE_REG(hw, E1000_SYSTIMH, ts->tv_sec);
}

/**
 * igb_ptp_systim_to_hwtstamp - convert system time value to hw timestamp
 * @adapter: board private structure
 * @hwtstamps: timestamp structure to update
 * @systim: unsigned 64bit system time value.
 *
 * We need to convert the system time value stored in the RX/TXSTMP registers
 * into a hwtstamp which can be used by the upper level timestamping functions.
 *
 * The 'tmreg_lock' spinlock is used to protect the consistency of the
 * system time value. This is needed because reading the 64 bit time
 * value involves reading two (or three) 32 bit registers. The first
 * read latches the value. Ditto for writing.
 *
 * In addition, here have extended the system time with an overflow
 * counter in software.
 **/
static void igb_ptp_systim_to_hwtstamp(struct igb_adapter *adapter,
				       struct skb_shared_hwtstamps *hwtstamps,
				       u64 systim)
{
	unsigned long flags;
	u64 ns;

	switch (adapter->hw.mac.type) {
	case e1000_82576:
	case e1000_82580:
	case e1000_i350:
		spin_lock_irqsave(&adapter->tmreg_lock, flags);

		ns = timecounter_cyc2time(&adapter->tc, systim);

		spin_unlock_irqrestore(&adapter->tmreg_lock, flags);

		memset(hwtstamps, 0, sizeof(*hwtstamps));
		hwtstamps->hwtstamp = ns_to_ktime(ns);
		break;
	case e1000_i210:
	case e1000_i211:
		memset(hwtstamps, 0, sizeof(*hwtstamps));
		/* Upper 32 bits contain s, lower 32 bits contain ns. */
		hwtstamps->hwtstamp = ktime_set(systim >> 32,
						systim & 0xFFFFFFFF);
		break;
	default:
		break;
	}
}

/*
 * PTP clock operations
 */

static int igb_ptp_adjfreq_82576(struct ptp_clock_info *ptp, s32 ppb)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	struct e1000_hw *hw = &igb->hw;
	int neg_adj = 0;
	u64 rate;
	u32 incvalue;

	if (ppb < 0) {
		neg_adj = 1;
		ppb = -ppb;
	}
	rate = ppb;
	rate <<= 14;
	rate = div_u64(rate, 1953125);

	incvalue = 16 << IGB_82576_TSYNC_SHIFT;

	if (neg_adj)
		incvalue -= rate;
	else
		incvalue += rate;

	E1000_WRITE_REG(hw, E1000_TIMINCA, INCPERIOD_82576 | (incvalue & INCVALUE_82576_MASK));

	return 0;
}

static int igb_ptp_adjfreq_82580(struct ptp_clock_info *ptp, s32 ppb)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	struct e1000_hw *hw = &igb->hw;
	int neg_adj = 0;
	u64 rate;
	u32 inca;

	if (ppb < 0) {
		neg_adj = 1;
		ppb = -ppb;
	}
	rate = ppb;
	rate <<= 26;
	rate = div_u64(rate, 1953125);

	inca = rate & INCVALUE_MASK;
	if (neg_adj)
		inca |= ISGN;

	E1000_WRITE_REG(hw, E1000_TIMINCA, inca);

	return 0;
}

static int igb_ptp_adjtime_82576(struct ptp_clock_info *ptp, s64 delta)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	unsigned long flags;
	s64 now;

	spin_lock_irqsave(&igb->tmreg_lock, flags);

	now = timecounter_read(&igb->tc);
	now += delta;
	timecounter_init(&igb->tc, &igb->cc, now);

	spin_unlock_irqrestore(&igb->tmreg_lock, flags);

	return 0;
}

static int igb_ptp_adjtime_i210(struct ptp_clock_info *ptp, s64 delta)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	unsigned long flags;
	struct timespec now, then = ns_to_timespec(delta);

	spin_lock_irqsave(&igb->tmreg_lock, flags);

	igb_ptp_read_i210(igb, &now);
	now = timespec_add(now, then);
	igb_ptp_write_i210(igb, (const struct timespec *)&now);

	spin_unlock_irqrestore(&igb->tmreg_lock, flags);

	return 0;
}

static int igb_ptp_gettime_82576(struct ptp_clock_info *ptp,
				 struct timespec *ts)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	unsigned long flags;
	u64 ns;
	u32 remainder;

	spin_lock_irqsave(&igb->tmreg_lock, flags);

	ns = timecounter_read(&igb->tc);

	spin_unlock_irqrestore(&igb->tmreg_lock, flags);

	ts->tv_sec = div_u64_rem(ns, 1000000000, &remainder);
	ts->tv_nsec = remainder;

	return 0;
}

static int igb_ptp_gettime_i210(struct ptp_clock_info *ptp,
				struct timespec *ts)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	unsigned long flags;

	spin_lock_irqsave(&igb->tmreg_lock, flags);

	igb_ptp_read_i210(igb, ts);

	spin_unlock_irqrestore(&igb->tmreg_lock, flags);

	return 0;
}

static int igb_ptp_settime_82576(struct ptp_clock_info *ptp,
				 const struct timespec *ts)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	unsigned long flags;
	u64 ns;

	ns = ts->tv_sec * 1000000000ULL;
	ns += ts->tv_nsec;

	spin_lock_irqsave(&igb->tmreg_lock, flags);

	timecounter_init(&igb->tc, &igb->cc, ns);

	spin_unlock_irqrestore(&igb->tmreg_lock, flags);

	return 0;
}

static int igb_ptp_settime_i210(struct ptp_clock_info *ptp,
				const struct timespec *ts)
{
	struct igb_adapter *igb = container_of(ptp, struct igb_adapter,
					       ptp_caps);
	unsigned long flags;

	spin_lock_irqsave(&igb->tmreg_lock, flags);

	igb_ptp_write_i210(igb, ts);

	spin_unlock_irqrestore(&igb->tmreg_lock, flags);

	return 0;
}


/**
 * igb_ptp_extts_work
 * @work: pointer to work struct
 *
 * This work function resets the PPS output registers.
 */
void igb_ptp_extts_work(struct work_struct *work)
{
	struct igb_adapter * adapter = container_of(work, struct igb_adapter, ptp_extts_work);
	struct e1000_hw *hw = &adapter->hw;
	struct ptp_clock_event event;
	u32 regval = E1000_READ_REG(hw, E1000_CTRL); 

	/* prepare PPS event */
	event.timestamp = E1000_READ_REG(hw, E1000_AUXSTMPL1);
	event.timestamp += E1000_READ_REG(hw, E1000_AUXSTMPH1) * NSEC_PER_SEC;
	if(!(regval & E1000_TS_SDP1_DATA)) {
		return;
	}
	event.type = PTP_CLOCK_EXTTS;
	event.index = 0;
	/* fire event */
	ptp_clock_event(adapter->ptp_clock, &event);
}

/**
 * igb_ptp_pps_work_i350
 * @work: pointer to work struct
 *
 * This work function resets the PPS output registers.
 */
void igb_ptp_pps_work_i350(struct work_struct *work)
{
	struct igb_adapter *adapter = container_of(work, struct igb_adapter,
			ptp_pps_work);
	struct e1000_hw *hw = &adapter->hw;
	u32 regval;
	unsigned long flags, start;

	start = adapter->ptp_pps_start + (u64)NSEC_PER_SEC / 2;
	start &= E1000_TMAX;
	adapter->ptp_pps_start = start;

	spin_lock_irqsave(&adapter->tmreg_lock,flags);
	E1000_WRITE_REG(hw, E1000_TRGTTIML0, lower_32_bits(start));
	E1000_WRITE_REG(hw, E1000_TRGTTIMH0, upper_32_bits(start));

	regval = E1000_READ_REG(hw, E1000_TSAUXC);
	regval |= (E1000_TSAUXC_EN_TT0);
	E1000_WRITE_REG(hw, E1000_TSAUXC, regval);
	E1000_WRITE_FLUSH(hw); 
	spin_unlock_irqrestore(&adapter->tmreg_lock,flags);
}


/**
 * igb_ptp_fire_pps_event_i350
 * @work: pointer to work struct
 *
 * This work function resets the PPS output registers.
 */
void igb_ptp_fire_pps_event_i350(struct work_struct *work)
{
	struct igb_adapter * adapter = container_of(work, struct igb_adapter, ptp_fire_pps_event_work);
	struct ptp_clock_event event;
	struct e1000_hw *hw = &adapter->hw;
	u32 regval = E1000_READ_REG(hw, E1000_CTRL_EXT); 
	/* prepare PPS event */
	if(!(regval & E1000_TS_SDP0_DATA)) {
		return;
	}
	event.type = PTP_CLOCK_PPS;
	event.index = 0;
	event.timestamp = timecounter_read(&adapter->tc);
	/* fire PPS event */
	ptp_clock_event(adapter->ptp_clock, &event);
}

static int igb_ptp_enable(struct ptp_clock_info *ptp,
		struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}

static int igb_ptp_enable_i350(struct ptp_clock_info *ptp,
		struct ptp_clock_request *rq, int on)
{
	struct igb_adapter * adapter = container_of(ptp, struct igb_adapter, ptp_caps);
	struct e1000_hw * hw = &adapter->hw;
	u32 regval, remainder = 0;
	unsigned long flags, start;
	u64 cc, stamp = 0;


	switch(rq->type) {
		case PTP_CLK_REQ_EXTTS:
			   
			if(rq->extts.flags & PTP_ENABLE_FEATURE) {
				// 1 
				regval = E1000_READ_REG(hw, E1000_TSSDP);
				regval |= E1000_TS_SDP_AUX1(1) | E1000_TS_SDP_AUX1_EN;
				E1000_WRITE_REG(hw, E1000_TSSDP, regval);
				// 2 
				regval = E1000_READ_REG(hw, E1000_CTRL);
				regval &= ~( E1000_TS_SDP1_DIR );
				E1000_WRITE_REG(hw, E1000_CTRL, regval);
				E1000_WRITE_FLUSH(hw); 
				// 3 
	     			regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval |= (E1000_TSAUXC_EN_TS1);
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval);

				// enable interrupts 
				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval |= (E1000_TSIM_AUTT1);
				E1000_WRITE_REG(hw, E1000_TSIM, regval);
				E1000_WRITE_FLUSH(hw); 
			}
			else {
				regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval &= ~(E1000_TSAUXC_EN_TS1);
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval);

				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval &= ~(E1000_TSIM_AUTT1);
				E1000_WRITE_REG(hw, E1000_TSIM, regval);

				E1000_WRITE_FLUSH(hw);
			}
		break;

		case PTP_CLK_REQ_PEROUT:

			/* If the period length is zero, disable the output and return */
			regval = E1000_READ_REG(hw, E1000_TSAUXC);
			if(!rq->perout.period.nsec) {
				regval &= ~E1000_TSAUXC_EN_CLK0;
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval); 
				E1000_WRITE_FLUSH(hw);
				return 0;
			}

			regval = rq->perout.period.nsec;
			if(regval > 255*8) {
				printk("%s (%d) Invalid period length specified! Refer 8.15.18 for valid settings.\n",__FUNCTION__, __LINE__);
				return -EINVAL;
			}
			printk("%s (%d) period time %d ns set\n", __FUNCTION__, __LINE__, regval);
			regval /= 16;
			E1000_WRITE_REG(hw, E1000_FREQOUT0, regval);
			printk("%s (%d): Periodic output on SDP1\n", __FUNCTION__, __LINE__);
			E1000_WRITE_FLUSH(hw);
			
			/* Map SDP1 to FREQOUT0 */
			regval = E1000_READ_REG(hw, E1000_TSSDP);
			regval |= E1000_TS_SDP0_SEL(2) | E1000_TS_SDP0_EN;
			E1000_WRITE_REG(hw, E1000_TSSDP, regval);
			/* Set SDP1 to output */
			regval = E1000_READ_REG(hw, E1000_CTRL);
			regval |= ( E1000_TS_SDP0_DIR );
			E1000_WRITE_REG(hw, E1000_CTRL, regval);
			/* SDP2-3 enabling is different */
			/*regval = E1000_READ_REG(hw, E1000_CTRL_EXT);
			regval |= ( E1000_TS_SDP2_DIR(1) );
			E1000_WRITE_REG(hw, E1000_CTRL_EXT, regval);*/
			E1000_WRITE_FLUSH(hw);

			/* Align the first rising edge to the start of the
			 * next second
			 */
			spin_lock_irqsave(&adapter->tmreg_lock, flags);
			cc = igb_ptp_read_82580(&adapter->cc);
			stamp = timecounter_cyc2time(&adapter->tc, cc);
			div_s64_rem(stamp,NSEC_PER_SEC,&remainder);
			cc += 2 * (u64)NSEC_PER_SEC - remainder;
			cc &= E1000_TMAX;
			spin_unlock_irqrestore(&adapter->tmreg_lock, flags);
			E1000_WRITE_REG(hw, E1000_TRGTTIML0, lower_32_bits(cc));
			E1000_WRITE_REG(hw, E1000_TRGTTIMH0, upper_32_bits(cc));
			E1000_WRITE_FLUSH(hw);
			
			/*Enable FREQOUT0 */
			regval = E1000_READ_REG(hw, E1000_TSAUXC);
			regval |= (E1000_TSAUXC_EN_CLK0 | E1000_TSAUXC_ST0 | E1000_TSAUXC_EN_TT0);
			E1000_WRITE_REG(hw, E1000_TSAUXC, regval); 
			E1000_WRITE_FLUSH(hw);

			break;

		case PTP_CLK_REQ_PPS:

			if(hw->mac.type == e1000_82580) {
				printk("%s (%d) This function is not supported on 82580 adapters\n",__FUNCTION__,__LINE__);
				return -EOPNOTSUPP;
			}

			printk("%s (%d): PPS %s\n",__FUNCTION__,__LINE__,on?"enable":"disable");
			if(on) {
				/* SYSTIM Synchronixed Pulse Generation on SDP Pins (7.9.4.1.2) */
				/* 1 */
				regval = E1000_READ_REG(hw, E1000_TSSDP);
				regval = E1000_TS_SDP0_SEL(0) | E1000_TS_SDP0_EN;
				E1000_WRITE_REG(hw, E1000_TSSDP, regval);
				/* 2 */
				regval = E1000_READ_REG(hw, E1000_CTRL);
				regval |= ( E1000_TS_SDP0_DIR  );
				E1000_WRITE_REG(hw, E1000_CTRL, regval);

				spin_lock_irqsave(&adapter->tmreg_lock,flags);
				cc = igb_ptp_read_82580(&adapter->cc);
				stamp = timecounter_cyc2time(&adapter->tc, cc);
				div_s64_rem(stamp,NSEC_PER_SEC,&remainder);
				/*first timer value = remaining time to the 2nd 
				  subsequent second minus correction */
				start = (cc + 2*(u64)NSEC_PER_SEC - remainder);
				start &= E1000_TMAX;
				adapter->ptp_pps_start = start;

				/* 5 */
				E1000_WRITE_REG(hw, E1000_TRGTTIML0, lower_32_bits(start));
				E1000_WRITE_REG(hw, E1000_TRGTTIMH0, upper_32_bits(start));
				E1000_WRITE_FLUSH(hw); 
				spin_unlock_irqrestore(&adapter->tmreg_lock,flags);
				/* 6 */
				regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval |= (E1000_TSAUXC_EN_TT0) ;
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval);
				E1000_WRITE_FLUSH(hw); 

				/* enable interrupts */
				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval |= (E1000_TSIM_TT0);
				E1000_WRITE_REG(hw, E1000_TSIM, regval);
				E1000_WRITE_FLUSH(hw); 
			}
			else {
				/* disable TT0 & TT1 */

				regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval &= ~(E1000_TSAUXC_EN_TT0);
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval);
				/* disable TT0 & TT1 IT */
				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval &= ~(E1000_TSIM_TT0);
				E1000_WRITE_REG(hw, E1000_TSIM, regval);

				E1000_WRITE_FLUSH(hw);
			}
			break;
		default: return -EOPNOTSUPP;

	}

	return 0;
}

static int igb_ptp_enable_i210(struct ptp_clock_info *ptp,
		struct ptp_clock_request *rq, int on)
{
	struct igb_adapter * adapter = container_of(ptp, struct igb_adapter, ptp_caps);
	struct e1000_hw * hw = &adapter->hw;
	u32 regval;
	struct timespec ts;

	switch(rq->type) {
		
		case PTP_CLK_REQ_EXTTS:
			   
			if(rq->extts.flags & PTP_ENABLE_FEATURE) {
				// 1 
				regval = E1000_READ_REG(hw, E1000_TSSDP);
				regval |= E1000_TS_SDP_AUX1(1) | E1000_TS_SDP_AUX1_EN;
				E1000_WRITE_REG(hw, E1000_TSSDP, regval);
				// 2 
				regval = E1000_READ_REG(hw, E1000_CTRL);
				regval &= ~( E1000_TS_SDP1_DIR );
				E1000_WRITE_REG(hw, E1000_CTRL, regval);
				E1000_WRITE_FLUSH(hw); 
				// 3 
	     			regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval |= (E1000_TSAUXC_EN_TS1);
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval);

				// enable interrupts 
				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval |= (E1000_TSIM_AUTT1);
				E1000_WRITE_REG(hw, E1000_TSIM, regval);
				E1000_WRITE_FLUSH(hw); 
			}
			else {
				regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval &= ~(E1000_TSAUXC_EN_TS1);
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval);

				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval &= ~(E1000_TSIM_AUTT1);
				E1000_WRITE_REG(hw, E1000_TSIM, regval);

				E1000_WRITE_FLUSH(hw);
			}
		break;
		case PTP_CLK_REQ_PEROUT:

			/* If the period length is zero, disable the output and return */
			if(!rq->perout.period.nsec) {
				regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval &= ~E1000_TSAUXC_EN_CLK0;
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval); 
			
				regval = E1000_READ_REG(hw, E1000_TSSDP);
				regval &= ~E1000_TS_SDP0_EN;
				E1000_WRITE_REG(hw, E1000_TSSDP, regval);
				E1000_WRITE_FLUSH(hw);
			
				printk("%s (%d) Periodic output on SDP1 disabled\n",__FUNCTION__,__LINE__);
				return 0;
			}
			regval = rq->perout.period.nsec / 2;
			if(regval > 70000000 && (regval != NSEC_PER_SEC/2
					|| regval != NSEC_PER_SEC/4 || regval != NSEC_PER_SEC/8) ) {
				printk("%s (%d) Invalid period length specified! Refer 8.15.18 for valid settings.\n", __FUNCTION__, __LINE__);
				return -EINVAL;
			}
			E1000_WRITE_REG(hw, E1000_FREQOUT0, regval);
			printk("%s (%d) Periodic output on SDP1 (T = %d)\n",__FUNCTION__,__LINE__, regval);
			E1000_WRITE_FLUSH(hw);
			
			/* Map SDP1 to FREQOUT0 */
			regval = E1000_READ_REG(hw, E1000_TSSDP);
			regval |= E1000_TS_SDP0_SEL(2) | E1000_TS_SDP0_EN;
			E1000_WRITE_REG(hw, E1000_TSSDP, regval);
			/* Set SDP1 to output */
			regval = E1000_READ_REG(hw, E1000_CTRL);
			regval |= ( E1000_TS_SDP0_DIR );
			E1000_WRITE_REG(hw, E1000_CTRL, regval);
			/* SDP2-3 enabling is different
			   regval = E1000_READ_REG(hw, E1000_CTRL_EXT);
			   regval |= ( E1000_TS_SDP2_DIR(1) );
			   E1000_WRITE_REG(hw, E1000_CTRL_EXT, regval);
			 */
			E1000_WRITE_FLUSH(hw);

			/* Align the first rising edge to the start of the
			 * next second
			 */
			ptp->gettime(ptp,&ts);
			ts.tv_nsec = NSEC_PER_SEC / 2;
			ts.tv_sec += 1;
			E1000_WRITE_REG(hw, E1000_TRGTTIML0, ts.tv_nsec);
			E1000_WRITE_REG(hw, E1000_TRGTTIMH0, ts.tv_sec);
			E1000_WRITE_FLUSH(hw);
			
			/*Enable FREQOUT0 */
			regval = E1000_READ_REG(hw, E1000_TSAUXC);
			regval |= E1000_TSAUXC_EN_CLK0 | E1000_TSAUXC_ST0 | E1000_TSAUXC_EN_TT0;
			E1000_WRITE_REG(hw, E1000_TSAUXC, regval); 
			E1000_WRITE_FLUSH(hw);

			break;

		case PTP_CLK_REQ_PPS:
			
			printk("%s (%d): PPS %s on SDP0\n",__FUNCTION__,__LINE__,on?"enabled":"disabled");

			if(on) {
				E1000_WRITE_REG(hw, E1000_FREQOUT0, (NSEC_PER_SEC / 2));
				E1000_WRITE_FLUSH(hw);

				/* Map SDP1 to FREQOUT0 */
				regval = E1000_READ_REG(hw, E1000_TSSDP);
				regval |= E1000_TS_SDP0_SEL(2) | E1000_TS_SDP0_EN;
				E1000_WRITE_REG(hw, E1000_TSSDP, regval);
				/* Set SDP1 to output */
				regval = E1000_READ_REG(hw, E1000_CTRL);
				regval |= ( E1000_TS_SDP0_DIR );
				E1000_WRITE_REG(hw, E1000_CTRL, regval);
				/* SDP2-3 enabling is different
				   regval = E1000_READ_REG(hw, E1000_CTRL_EXT);
				   regval |= ( E1000_TS_SDP2_DIR(1) );
				   E1000_WRITE_REG(hw, E1000_CTRL_EXT, regval);
				 */
				E1000_WRITE_FLUSH(hw);

				/* Align the first rising edge to the start of the
				 * next second
				 */
				ptp->gettime(ptp,&ts);
				ts.tv_nsec = (NSEC_PER_SEC / 2);
				ts.tv_sec += 1;
				E1000_WRITE_REG(hw, E1000_TRGTTIML0, ts.tv_nsec);
				E1000_WRITE_REG(hw, E1000_TRGTTIMH0, ts.tv_sec);
				E1000_WRITE_FLUSH(hw);

				/* enable interrupts */
				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval |= E1000_TSIM_TT0;
				E1000_WRITE_REG(hw, E1000_TSIM, regval);
				E1000_WRITE_FLUSH(hw); 
				
				/*Enable FREQOUT0 */
				regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval |= (E1000_TSAUXC_EN_CLK0 | E1000_TSAUXC_ST0 | E1000_TSAUXC_EN_TT0);
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval); 
				E1000_WRITE_FLUSH(hw);
			}
			else {
				regval = E1000_READ_REG(hw, E1000_TSAUXC);
				regval &= ~E1000_TSAUXC_EN_CLK0;
				E1000_WRITE_REG(hw, E1000_TSAUXC, regval);
 
				regval = E1000_READ_REG(hw, E1000_TSIM);
				regval &= ~E1000_TSIM_TT0;
				E1000_WRITE_REG(hw, E1000_TSIM, regval);

				regval = E1000_READ_REG(hw, E1000_TSSDP);
				regval &= ~E1000_TS_SDP0_EN;
				E1000_WRITE_REG(hw, E1000_TSSDP, regval);
				E1000_WRITE_FLUSH(hw);
			}
			break;

		default: return -EOPNOTSUPP;

	}

	return 0;
}


/**
 * igb_ptp_tx_work
 * @work: pointer to work struct
 *
 * This work function polls the TSYNCTXCTL valid bit to determine when a
 * timestamp has been taken for the current stored skb.
 */
void igb_ptp_tx_work(struct work_struct *work)
{
	struct igb_adapter *adapter = container_of(work, struct igb_adapter,
						   ptp_tx_work);
	struct e1000_hw *hw = &adapter->hw;
	u32 tsynctxctl;
 
	if (!adapter->ptp_tx_skb)
		return;

	tsynctxctl = E1000_READ_REG(hw, E1000_TSYNCTXCTL);
	if (tsynctxctl & E1000_TSYNCTXCTL_VALID)
		igb_ptp_tx_hwtstamp(adapter);
	else
		/* reschedule to check later */
		schedule_work(&adapter->ptp_tx_work);
}

static void igb_ptp_overflow_check(struct work_struct *work)
{
	struct igb_adapter *igb =
		container_of(work, struct igb_adapter, ptp_overflow_work.work);
	struct timespec ts;

	igb->ptp_caps.gettime(&igb->ptp_caps, &ts);

	pr_debug("igb overflow check at %ld.%09lu\n", ts.tv_sec, ts.tv_nsec);

	schedule_delayed_work(&igb->ptp_overflow_work,
			IGB_SYSTIM_OVERFLOW_PERIOD);
}

/**
 * igb_ptp_tx_hwtstamp - utility function which checks for TX time stamp
 * @adapter: Board private structure.
 *
 * If we were asked to do hardware stamping and such a time stamp is
 * available, then it must have been for this skb here because we only
 * allow only one such packet into the queue.
 */
void igb_ptp_tx_hwtstamp(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	struct skb_shared_hwtstamps shhwtstamps;
	u64 regval;

	regval = E1000_READ_REG(hw, E1000_TXSTMPL);
	regval |= (u64)E1000_READ_REG(hw, E1000_TXSTMPH) << 32;

	igb_ptp_systim_to_hwtstamp(adapter, &shhwtstamps, regval);
	skb_tstamp_tx(adapter->ptp_tx_skb, &shhwtstamps);
	dev_kfree_skb_any(adapter->ptp_tx_skb);
	adapter->ptp_tx_skb = NULL;
}

void igb_ptp_rx_hwtstamp(struct igb_q_vector *q_vector,
			 union e1000_adv_rx_desc *rx_desc,
			 struct sk_buff *skb)
{
	struct igb_adapter *adapter = q_vector->adapter;
	struct e1000_hw *hw = &adapter->hw;
	u64 regval;

	if (!igb_test_staterr(rx_desc, E1000_RXDADV_STAT_TSIP |
				       E1000_RXDADV_STAT_TS))
		return;

	/*
	 * If this bit is set, then the RX registers contain the time stamp. No
	 * other packet will be time stamped until we read these registers, so
	 * read the registers to make them available again. Because only one
	 * packet can be time stamped at a time, we know that the register
	 * values must belong to this one here and therefore we don't need to
	 * compare any of the additional attributes stored for it.
	 *
	 * If nothing went wrong, then it should have a shared tx_flags that we
	 * can turn into a skb_shared_hwtstamps.
	 */
	if (igb_test_staterr(rx_desc, E1000_RXDADV_STAT_TSIP)) {
		u32 *stamp = (u32 *)skb->data;
		regval = le32_to_cpu(*(stamp + 2));
		regval |= (u64)le32_to_cpu(*(stamp + 3)) << 32;
		skb_pull(skb, IGB_TS_HDR_LEN);
	} else {
		if (!(E1000_READ_REG(hw, E1000_TSYNCRXCTL) & E1000_TSYNCRXCTL_VALID))
			return;

		regval = E1000_READ_REG(hw, E1000_RXSTMPL);
		regval |= (u64)E1000_READ_REG(hw, E1000_RXSTMPH) << 32;
	}

	igb_ptp_systim_to_hwtstamp(adapter, skb_hwtstamps(skb), regval);
}

/**
 * igb_ptp_hwtstamp_ioctl - control hardware time stamping
 * @netdev:
 * @ifreq:
 * @cmd:
 *
 * Outgoing time stamping can be enabled and disabled. Play nice and
 * disable it when requested, although it shouldn't case any overhead
 * when no packet needs it. At most one packet in the queue may be
 * marked for time stamping, otherwise it would be impossible to tell
 * for sure to which packet the hardware time stamp belongs.
 *
 * Incoming time stamping has to be configured via the hardware
 * filters. Not all combinations are supported, in particular event
 * type has to be specified. Matching the kind of event packet is
 * not supported, with the exception of "all V2 events regardless of
 * level 2 or 4".
 *
 **/
int igb_ptp_hwtstamp_ioctl(struct net_device *netdev,
			   struct ifreq *ifr, int cmd)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	struct hwtstamp_config config;
	u32 tsync_tx_ctl = E1000_TSYNCTXCTL_ENABLED;
	u32 tsync_rx_ctl = E1000_TSYNCRXCTL_ENABLED;
	u32 tsync_rx_cfg = 0;
	bool is_l4 = false;
	bool is_l2 = false;
	u32 regval;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		tsync_tx_ctl = 0;
	case HWTSTAMP_TX_ON:
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		tsync_rx_ctl = 0;
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
		tsync_rx_ctl |= E1000_TSYNCRXCTL_TYPE_L4_V1;
		tsync_rx_cfg = E1000_TSYNCRXCFG_PTP_V1_SYNC_MESSAGE;
		is_l4 = true;
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		tsync_rx_ctl |= E1000_TSYNCRXCTL_TYPE_L4_V1;
		tsync_rx_cfg = E1000_TSYNCRXCFG_PTP_V1_DELAY_REQ_MESSAGE;
		is_l4 = true;
		break;
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		tsync_rx_ctl |= E1000_TSYNCRXCTL_TYPE_EVENT_V2;
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		is_l2 = true;
		is_l4 = true;
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_ALL:
		/*
		 * 82576 cannot timestamp all packets, which it needs to do to
		 * support both V1 Sync and Delay_Req messages
		 */
		if (hw->mac.type != e1000_82576) {
			tsync_rx_ctl |= E1000_TSYNCRXCTL_TYPE_ALL;
			config.rx_filter = HWTSTAMP_FILTER_ALL;
			break;
		}
		/* fall through */
	default:
		config.rx_filter = HWTSTAMP_FILTER_NONE;
		return -ERANGE;
	}

	if (hw->mac.type == e1000_82575) {
		if (tsync_rx_ctl | tsync_tx_ctl)
			return -EINVAL;
		return 0;
	}

	/*
	 * Per-packet timestamping only works if all packets are
	 * timestamped, so enable timestamping in all packets as
	 * long as one rx filter was configured.
	 */
	if ((hw->mac.type >= e1000_82580) && tsync_rx_ctl) {
		tsync_rx_ctl = E1000_TSYNCRXCTL_ENABLED;
		tsync_rx_ctl |= E1000_TSYNCRXCTL_TYPE_ALL;
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		is_l2 = true;
		is_l4 = true;

		if ((hw->mac.type == e1000_i210) ||
		    (hw->mac.type == e1000_i211)) {
			regval = E1000_READ_REG(hw, E1000_RXPBS);
			regval |= E1000_RXPBS_CFG_TS_EN;
			E1000_WRITE_REG(hw, E1000_RXPBS, regval);
		}
	}

	/* enable/disable TX */
	regval = E1000_READ_REG(hw, E1000_TSYNCTXCTL);
	regval &= ~E1000_TSYNCTXCTL_ENABLED;
	regval |= tsync_tx_ctl;
	E1000_WRITE_REG(hw, E1000_TSYNCTXCTL, regval);

	/* enable/disable RX */
	regval = E1000_READ_REG(hw, E1000_TSYNCRXCTL);
	regval &= ~(E1000_TSYNCRXCTL_ENABLED | E1000_TSYNCRXCTL_TYPE_MASK);
	regval |= tsync_rx_ctl;
	E1000_WRITE_REG(hw, E1000_TSYNCRXCTL, regval);

	/* define which PTP packets are time stamped */
	E1000_WRITE_REG(hw, E1000_TSYNCRXCFG, tsync_rx_cfg);

	/* define ethertype filter for timestamped packets */
	if (is_l2)
		E1000_WRITE_REG(hw, E1000_ETQF(3),
		     (E1000_ETQF_FILTER_ENABLE | /* enable filter */
		      E1000_ETQF_1588 | /* enable timestamping */
		      ETH_P_1588));     /* 1588 eth protocol type */
	else
		E1000_WRITE_REG(hw, E1000_ETQF(3), 0);

#define PTP_PORT 319
	/* L4 Queue Filter[3]: filter by destination port and protocol */
	if (is_l4) {
		u32 ftqf = (IPPROTO_UDP /* UDP */
			| E1000_FTQF_VF_BP /* VF not compared */
			| E1000_FTQF_1588_TIME_STAMP /* Enable Timestamping */
			| E1000_FTQF_MASK); /* mask all inputs */
		ftqf &= ~E1000_FTQF_MASK_PROTO_BP; /* enable protocol check */

		E1000_WRITE_REG(hw, E1000_IMIR(3), htons(PTP_PORT));
		E1000_WRITE_REG(hw, E1000_IMIREXT(3),
		     (E1000_IMIREXT_SIZE_BP | E1000_IMIREXT_CTRL_BP));
		if (hw->mac.type == e1000_82576) {
			/* enable source port check */
			E1000_WRITE_REG(hw, E1000_SPQF(3), htons(PTP_PORT));
			ftqf &= ~E1000_FTQF_MASK_SOURCE_PORT_BP;
		}
		E1000_WRITE_REG(hw, E1000_FTQF(3), ftqf);
	} else {
		E1000_WRITE_REG(hw, E1000_FTQF(3), E1000_FTQF_MASK);
	}
	E1000_WRITE_FLUSH(hw);

	/* clear TX/RX time stamp registers, just to be sure */
	regval = E1000_READ_REG(hw, E1000_TXSTMPL);
	regval = E1000_READ_REG(hw, E1000_TXSTMPH);
	regval = E1000_READ_REG(hw, E1000_RXSTMPL);
	regval = E1000_READ_REG(hw, E1000_RXSTMPH);

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

void igb_ptp_init(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	struct net_device *netdev = adapter->netdev;

	switch (hw->mac.type) {
	case e1000_82576:
		snprintf(adapter->ptp_caps.name, 16, "%pm", netdev->dev_addr);
		adapter->ptp_caps.owner = THIS_MODULE;
		adapter->ptp_caps.max_adj = 1000000000;
		adapter->ptp_caps.n_ext_ts = 0;
		adapter->ptp_caps.pps = 0;
		adapter->ptp_caps.adjfreq = igb_ptp_adjfreq_82576;
		adapter->ptp_caps.adjtime = igb_ptp_adjtime_82576;
		adapter->ptp_caps.gettime = igb_ptp_gettime_82576;
		adapter->ptp_caps.settime = igb_ptp_settime_82576;
		adapter->ptp_caps.enable = igb_ptp_enable;
		adapter->cc.read = igb_ptp_read_82576;
		adapter->cc.mask = CLOCKSOURCE_MASK(64);
		adapter->cc.mult = 1;
		adapter->cc.shift = IGB_82576_TSYNC_SHIFT;
		/* Dial the nominal frequency. */
		E1000_WRITE_REG(hw, E1000_TIMINCA, INCPERIOD_82576 |
						   INCVALUE_82576);
		break;
	case e1000_82580:
	case e1000_i350:
		snprintf(adapter->ptp_caps.name, 16, "%pm", netdev->dev_addr);
		adapter->ptp_caps.owner = THIS_MODULE;
		adapter->ptp_caps.max_adj = 62499999;
		adapter->ptp_caps.n_ext_ts = 1;
		adapter->ptp_caps.pps = 1;
		adapter->ptp_caps.n_per_out = 1;
		adapter->ptp_caps.adjfreq = igb_ptp_adjfreq_82580;
		adapter->ptp_caps.adjtime = igb_ptp_adjtime_82576;
		adapter->ptp_caps.gettime = igb_ptp_gettime_82576;
		adapter->ptp_caps.settime = igb_ptp_settime_82576;
		adapter->ptp_caps.enable = igb_ptp_enable_i350;
		adapter->cc.read = igb_ptp_read_82580;
		adapter->cc.mask = CLOCKSOURCE_MASK(IGB_NBITS_82580);
		adapter->cc.mult = 1;
		adapter->cc.shift = 0;
		/* Enable the timer functions by clearing bit 31. */
		E1000_WRITE_REG(hw, E1000_TSAUXC, 0x0);
		/* Initialize the PPS signal rearming trigger */
		INIT_WORK(&adapter->ptp_fire_pps_event_work,
				igb_ptp_fire_pps_event_i350);
		INIT_WORK(&adapter->ptp_pps_work,
				igb_ptp_pps_work_i350);
		break;
	case e1000_i210:
	case e1000_i211:
		snprintf(adapter->ptp_caps.name, 16, "%pm", netdev->dev_addr);
		adapter->ptp_caps.owner = THIS_MODULE;
		adapter->ptp_caps.max_adj = 62499999;
		adapter->ptp_caps.n_ext_ts = 1;
		adapter->ptp_caps.pps = 1;
		adapter->ptp_caps.n_per_out = 1;
		adapter->ptp_caps.adjfreq = igb_ptp_adjfreq_82580;
		adapter->ptp_caps.adjtime = igb_ptp_adjtime_i210;
		adapter->ptp_caps.gettime = igb_ptp_gettime_i210;
		adapter->ptp_caps.settime = igb_ptp_settime_i210;
		adapter->ptp_caps.enable = igb_ptp_enable_i210;
		/* Enable the timer functions by clearing bit 31. */
		E1000_WRITE_REG(hw, E1000_TSAUXC, 0x0);
		break;
	default:
		adapter->ptp_clock = NULL;
		return;
	}

	spin_lock_init(&adapter->tmreg_lock);
	INIT_WORK(&adapter->ptp_tx_work, igb_ptp_tx_work);
	INIT_WORK(&adapter->ptp_extts_work, igb_ptp_extts_work);	
	/* Initialize the clock and overflow work for devices that need it. */
	if ((hw->mac.type == e1000_i210) || (hw->mac.type == e1000_i211)) {
		struct timespec ts = ktime_to_timespec(ktime_get_real());

		igb_ptp_settime_i210(&adapter->ptp_caps, &ts);
	} else {
		timecounter_init(&adapter->tc, &adapter->cc,
				 ktime_to_ns(ktime_get_real()));

		INIT_DELAYED_WORK(&adapter->ptp_overflow_work,
				  igb_ptp_overflow_check);

		schedule_delayed_work(&adapter->ptp_overflow_work,
				      IGB_SYSTIM_OVERFLOW_PERIOD);
	}


	/* Initialize the time sync interrupts for devices that support it. */
	if (hw->mac.type >= e1000_82580) {
		E1000_WRITE_REG(hw, E1000_TSIM, E1000_TSIM_TXTS);
		E1000_WRITE_REG(hw, E1000_IMS, E1000_IMS_TS);
	}

	adapter->ptp_clock = ptp_clock_register(&adapter->ptp_caps);
	if (IS_ERR(adapter->ptp_clock)) {
		adapter->ptp_clock = NULL;
		dev_err(&adapter->pdev->dev, "ptp_clock_register failed\n");
	} else {
		dev_info(&adapter->pdev->dev, "added PHC on %s\n",
			 adapter->netdev->name);
		adapter->flags |= IGB_FLAG_PTP;
	}

	printk("%s (%d): PPS compensation offset: %d ns\n",__FUNCTION__,__LINE__,adapter->pps_delay);
}

/**
 * igb_ptp_stop - Disable PTP device and stop the overflow check.
 * @adapter: Board private structure.
 *
 * This function stops the PTP support and cancels the delayed work.
 **/
void igb_ptp_stop(struct igb_adapter *adapter)
{
	switch (adapter->hw.mac.type) {
	case e1000_82576:
	case e1000_82580:
	case e1000_i350:
		cancel_delayed_work_sync(&adapter->ptp_overflow_work);
		cancel_work_sync(&adapter->ptp_pps_work);
		cancel_work_sync(&adapter->ptp_fire_pps_event_work);
		break;
	case e1000_i210:
	case e1000_i211:
		/* No delayed work to cancel. */
		break;
	default:
		return;
	}

	cancel_work_sync(&adapter->ptp_tx_work);
	cancel_work_sync(&adapter->ptp_extts_work);

	if (adapter->ptp_clock) {
		ptp_clock_unregister(adapter->ptp_clock);
		dev_info(&adapter->pdev->dev, "removed PHC on %s\n",
			 adapter->netdev->name);
		adapter->flags &= ~IGB_FLAG_PTP;
	}
}

/**
 * igb_ptp_reset - Re-enable the adapter for PTP following a reset.
 * @adapter: Board private structure.
 *
 * This function handles the reset work required to re-enable the PTP device.
 **/
void igb_ptp_reset(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;

	if (!(adapter->flags & IGB_FLAG_PTP))
		return;

	switch (adapter->hw.mac.type) {
	case e1000_82576:
		/* Dial the nominal frequency. */
		E1000_WRITE_REG(hw, E1000_TIMINCA, INCPERIOD_82576 |
						   INCVALUE_82576);
		break;
	case e1000_82580:
	case e1000_i350:
	case e1000_i210:
	case e1000_i211:
		/* Enable the timer functions and interrupts. */
		E1000_WRITE_REG(hw, E1000_TSAUXC, 0x0);
		E1000_WRITE_REG(hw, E1000_TSIM, E1000_TSIM_TXTS);
		E1000_WRITE_REG(hw, E1000_IMS, E1000_IMS_TS);
		break;
	default:
		/* No work to do. */
		return;
	}

	/* Re-initialize the timer. */
	if ((hw->mac.type == e1000_i210) || (hw->mac.type == e1000_i211)) {
		struct timespec ts = ktime_to_timespec(ktime_get_real());

		igb_ptp_settime_i210(&adapter->ptp_caps, &ts);
	} else {
		timecounter_init(&adapter->tc, &adapter->cc,
				 ktime_to_ns(ktime_get_real()));
	}
}
#endif /* CONFIG_IGB_PTP */
