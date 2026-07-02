#include <stdlib.h>
#include <math.h>
#include <rte_random.h>

#include "common.h"

static enum load_distribution current_dist = DIST_FIXED_1;

void load_generator_init(enum load_distribution dist)
{
	current_dist = dist;
}

static inline double get_random_double(void)
{
	return (double)rte_rand() / (double)UINT64_MAX;
}

uint16_t load_generator_get_processing_time(void)
{
	double r;
	
	switch (current_dist) {
	case DIST_FIXED_1:
		/* Fixed(1) - always return 1us */
		return 1;
		
	case DIST_FIXED_10:
		/* Fixed(10) - always return 10us */
		return 10;
		
	case DIST_HIGH_BIMODAL:
		/* Bimodal(50:1, 50:100) - 50% chance of 1us, 50% chance of 100us */
		r = get_random_double();
		return (r < 0.5) ? 1 : 100;
		
	case DIST_EXTREME_BIMODAL:
		/* Bimodal(99.5:0.5, 0.5:500) - 99.5% chance of 0.5us, 0.5% chance of 500us */
		r = get_random_double();
		if (r < 0.995) {
			return MAGIC_500_NS;
		} else {
			return 500;
		}
		
	case DIST_ROCKSDB_HT:
		/* Bimodal(50:1.25, 50:613) - 50% chance of 1.25us, 50% chance of 613us */
		r = get_random_double();
		if (r < 0.5) {
			return MAGIC_1250_NS;
		} else {
			return 613;
		}

	case DIST_ROCKSDB_LT:
		/* Fixed(1.25us) - always return 1.25us */
		return MAGIC_1250_NS;
		
	case DIST_EXPONENTIAL_10:
		/* Exponential(mean=10) - exponential distribution with mean 10us */
		/* Use inverse transform sampling: X = -mean * ln(U) where U ~ Uniform(0,1) */
		r = get_random_double();
		if (r == 0.0) r = 1e-10;
		{
			double value = -10.0 * log(r);
			/* Cap at 65535 for uint16_t, minimum 1 */
			if (value < 1.0) {
				return 1;
			} else if (value > 65535.0) {
				return 65535;
			} else {
				return (uint16_t)value;
			}
		}

	case DIST_TPCC:
		/* TPC-C mix:
		 *   Payment     5.7us   44%
		 *   OrderStatus 6us      4%
		 *   NewOrder    20us    44%
		 *   Delivery    88us     4%
		 *   StockLevel  100us    4%
		 */
		r = get_random_double();
		if (r < 0.44) {
			return MAGIC_5700_NS;
		} else if (r < 0.48) {
			return 6;
		} else if (r < 0.92) {
			return 20;
		} else if (r < 0.96) {
			return 88;
		} else {
			return 100;
		}
		
	default:
		return 1;  /* Default: Fixed(1us) */
	}
}

const char *load_generator_get_name(void)
{
	switch (current_dist) {
	case DIST_FIXED_1:
		return "Fixed(1us)";
	case DIST_FIXED_10:
		return "Fixed(10us)";
	case DIST_EXPONENTIAL_10:
		return "Exponential(mean=10us)";
	case DIST_HIGH_BIMODAL:
		return "Bimodal(50%:1us, 50%:100us)";
	case DIST_EXTREME_BIMODAL:
		return "Bimodal(99.5%:0.5us, 0.5%:500us)";
	case DIST_ROCKSDB_HT:
		return "Bimodal(50%:1.25us, 50%:613us)";
	case DIST_ROCKSDB_LT:
		return "Fixed(1.25us)";
	case DIST_TPCC:
		return "TPC-C(Payment 44%:5.7us, OrderStatus 4%:6us, NewOrder 44%:20us, Delivery 4%:88us, StockLevel 4%:100us)";
	default:
		return "Fixed(1us)";
	}
}
