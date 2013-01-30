/**
 * @file perpps.c
 * @brief Utility program to set the adapter's ancillary features.
 * @note Copyright (C) 2013 Balint Ferencz <fernya@sch.bme.hu>
 * @note Based on the hwstamp_ctl utility
 * @note Copyright (C) 2012 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <linux/ptp_clock.h>

static void usage(char *progname)
{
	fprintf(stderr,
		"\n"
		"usage: %s [options]\n\n"
		" -h                     prints this message and exits\n"
		" -d [device]            PHC to use, for example '/dev/ptp0'\n"
		" -p 1/0                 enable/disable PPS\n"
		" -P [channel, period]   enable periodic output\n"
		"                        0 period time disables the output\n"
		"\n",
		progname);
}

int main(int argc, char *argv[])
{
	char *device = NULL, *progname;
	int c, err, fd, ppsen = 0, ppsel = 0, per_sel = 0, per_channel = 0, per_period = 0;
	struct ptp_perout_request perout;


	/* Process the command line arguments. */
	progname = strrchr(argv[0], '/');
	progname = progname ? 1+progname : argv[0];
	while (EOF != (c = getopt(argc, argv, "hd:p:P:"))) {
		switch (c) {
		case 'd':
			device = optarg;
			break;
		case 'p':
			ppsel = 1;
			ppsen = atoi(optarg);
			break;
		case 'P':
			per_sel = 1;
			per_channel = strtol(optarg, &optarg, 0);
			if (optarg[0])
				per_period = strtol(optarg + 1, 0, 0);
			break;
		case 'h':
			usage(progname);
			return 0;
		case '?':
		default:
			usage(progname);
			return -1;
		}
	}

	if (!device) {
		usage(progname);
		return -1;
	}

	
	if(!device)
		device = "/dev/ptp0";

	fd = open(device, O_RDWR);

	if (fd < 0) {
		perror("fd");
		return -1;
	}

	if(per_sel) {
		
		memset(&perout, 0, sizeof(perout));
		perout.index   = per_channel;
		perout.period.nsec  = per_period;

		err = ioctl(fd, PTP_PEROUT_REQUEST, &perout);
		if (err < 0){
			perror("PTP_PEROUT_REQUEST failed");
			return err ? errno : 0;
		}
		printf("Periodic out \tchannel: %d\n" "\t\tperiod: %d ns\n", perout.index, perout.period.nsec);
	}

	if(ppsel) {
		err = ioctl(fd, PTP_ENABLE_PPS, ppsen);
		if (err < 0){
			perror("PTP_ENABLE_PPS failed");
			return err ? errno : 0;
		}
		printf("PPS %s\n",ppsen?"enabled":"disabled");
	}

	return 0;
}
