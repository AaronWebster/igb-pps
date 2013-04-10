#igb driver with Pulse Per Second I/O for Intel i350 and i210 series

This software implements the necessary clock sync services needed for high
quality clock synchronization.

##Main features

  * clock synthesizer outputs (works on 82580, i350, i210 series)
  * Pulse Per Second outputs (i350, i210)
  * external event timestamping (i350, i210)

The i350 and i210 adapters feature four software definable pins which can be
used for reading events from the environment (such as PPS signal of a GPS) or
outputting signals to the connected devices (e.g. PPS signal from a PTP slave).
The pins can be assigned to every function, one at a time.  The source code has
the following mappings:

  * SDP0 - external time stamping input
  * SDP1 - PPS output / frequency synthesizer output

One can easily remap them with the modification of the respective ptp\_enable
functions, the necessary code snippets are marked.
Please note that the multi port i350 adapters have independent local clocks per port!

WARNING: Never connect 5V level signal source to the pins of the adapter!

##Requirements

  * Intel i350, i210 or 82580 (partially supported) network adapters
  * Linux kernel version >3.0 (>3.2 recommended), with PTP modules compiled

##Installation

  1. checkout the source code by issuing git clone

	https://fernya@bitbucket.org/fernya/igb-pps.git

  1. compile the perpps and ts2phc utilities (run 'make' in their
subdirectories)
  1. compile and install the source code of the driver (run 'make install' in
the src subdirectory)
  1. unload & load the current driver ('rmmod igb && modprobe igb')

If no error messages displayed, you're done!

##Utilites

To exploit the enabled functionality two utilities are included in this release.

The 'perpps' utility selects the desired output frequency, and enables the
PPS/synthesizer outputs of the cards. A common use case is the validation of the
synchronization system, it works on PTP master and slave devices too. Please
note that the outputs should be enabled in synchronized state (when the clock
is slewed to the referece)!

example: sudo ./perpps -d /dev/ptp0 -p 1


The 'ts2phc' utility programs the adapter to receive the external timestamp
events. The program reads the timestamps, and corrects the local clock of the
adapter. It currently works properly with PPS signals only (the driver detects
the rising edges at the start of the second).

example: sudo ./ts2phc -d /dev/ptp0

Both utilities have help messages when they are invoked without parameters.
